// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's part of a save (save/save_file.hpp, docs/adr/0002-...): the
// VM's sections come from vm/save.cpp, RFST is vm/ref_states.hpp; here are
// HEAD, PAPY and QUST, and the two phases of a load.
//
//   HEAD  text engine version; text pack world hash; u32 count, then the
//         pack's load order as texts. A save from another pack or load order
//         is refused: form ids would map to the wrong objects.
//   PAPY  u32 count + the references whose scripts were attached; u32 count
//         of triggers, per trigger the ref and its actors (u32 count + ids);
//         u32 count of actors with f64 x3 position; u32 next form id
//   QUST  u32 quests (id; running, started, completed, failed, active as
//         u8s; i32 stage; u32 count + done stages; u32 count + objective
//         index and bits); u32 count + quests with scripts; u32 aliases
//         (quest, alias, handle, fill); u32 globals (id, f64); u32 next alias
//         handle
//
// `prepare_load` decodes into a PendingLoad and touches nothing else, so a
// damaged or foreign save leaves the running game as it is; `commit_load`
// swaps it in.
#include "vm/papyrus.hpp"

#include "save/bytes.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

namespace skydot {

using godot::String;
using save::ByteReader;
using save::ByteWriter;

namespace {

constexpr std::uint32_t k_section_version = 1;
constexpr std::uint32_t k_papy_version = 2; // 2: the sound instance counter

std::string utf8_of(const String& s) {
    const auto bytes = s.utf8();
    return {bytes.get_data(), static_cast<std::size_t>(bytes.length())};
}

String godot_of(const std::string& s) { return String::utf8(s.c_str(), static_cast<int>(s.size())); }

bool open_section(const save::SaveReader& file, const char* tag, std::uint32_t version, ByteReader& out,
                  std::string& error) {
    const auto* s = file.find(tag);
    if (s == nullptr) {
        error = std::string("the save lacks its ") + tag + " section";
        return false;
    }
    if (s->version != version) {
        error = std::string("section ") + tag + " has version " + std::to_string(s->version) +
                ", which this engine does not read (it reads " + std::to_string(version) + ")";
        return false;
    }
    out = file.open(*s);
    return true;
}

} // namespace

void SkydotPapyrus::write_host(save::SaveWriter& out) const {
    ByteWriter p;
    p.u32(static_cast<std::uint32_t>(attached_.size()));
    for (const auto ref : attached_) {
        p.u32(ref);
    }
    triggers_.write(p);
    p.u32(test_ids_.next());
    p.u32(sound_ids_.next());
    out.add("PAPY", k_papy_version, p.out);

    out.add("QUST", vm::QuestStates::k_section_version, quests_.save());
}

bool SkydotPapyrus::decode_host(const save::SaveReader& file, HostSnapshot& out, std::string& error) const {
    ByteReader r({}, 0);
    const auto fail = [&](const char* tag) {
        error = std::string("the ") + tag + " section of the save is damaged";
        return false;
    };
    if (!open_section(file, "PAPY", k_papy_version, r, error)) {
        return false;
    }
    std::uint32_t n = 0;
    if (!r.count(n, 4)) {
        return fail("PAPY");
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0;
        if (!r.u32(ref)) {
            return fail("PAPY");
        }
        out.attached.insert(ref);
    }
    if (!vm::Triggers::read(r, out.triggers)) {
        return fail("PAPY");
    }
    std::uint32_t next_test = 0, next_sound = 0;
    if (!r.u32(next_test) || !r.u32(next_sound) || !r.at_end() || !out.test_ids.restore(next_test) ||
        !out.sound_ids.restore(next_sound)) {
        return fail("PAPY");
    }

    if (!open_section(file, "QUST", vm::QuestStates::k_section_version, r, error)) {
        return false;
    }
    if (!vm::QuestStates::load(r, out.quests)) {
        return fail("QUST");
    }
    return true;
}

void SkydotPapyrus::write_sections(const godot::Ref<SkydotSaveWriter>& writer) const {
    save::SaveWriter& out = writer->core();
    ByteWriter head;
    head.text(SKYDOT_VERSION);
    head.text(pack_.is_valid() ? utf8_of(pack_->get_world_hash()) : std::string{});
    const godot::PackedStringArray order = pack_.is_valid() ? pack_->get_load_order() : godot::PackedStringArray();
    head.u32(static_cast<std::uint32_t>(order.size()));
    for (const auto& plugin : order) {
        head.text(utf8_of(plugin));
    }
    out.add("HEAD", k_section_version, head.out);
    vm_->save_to(out);
    out.add("RFST", vm::RefStates::k_section_version, states_.save());
    out.add("ACTV", vm::ActorStates::k_section_version, actors_.save());
    out.add("DLGS", dialogue::DialogueStates::k_section_version, dialogue_states_.save());
    write_host(out);
}

godot::Error SkydotPapyrus::prepare(const save::SaveReader& file) {
    pending_.reset();
    const auto fail = [this](const std::string& why) {
        last_error_ = godot_of(why);
        return godot::ERR_FILE_CORRUPT;
    };
    if (world_.is_null() || pack_.is_null()) {
        return fail("setup has not run");
    }
    std::string why;
    ByteReader r({}, 0);
    if (!open_section(file, "HEAD", k_section_version, r, why)) {
        return fail(why);
    }
    std::string version, hash;
    std::uint32_t plugins = 0;
    if (!r.text(version) || !r.text(hash) || !r.count(plugins, 4)) {
        return fail("the HEAD section of the save is damaged");
    }
    std::vector<std::string> order;
    for (std::uint32_t i = 0; i < plugins; ++i) {
        std::string plugin;
        if (!r.text(plugin)) {
            return fail("the HEAD section of the save is damaged");
        }
        order.push_back(std::move(plugin));
    }
    std::vector<std::string> ours;
    for (const auto& plugin : pack_->get_load_order()) {
        ours.push_back(utf8_of(plugin));
    }
    if (hash != utf8_of(pack_->get_world_hash()) || order != ours) {
        return fail("the save is from another pack or load order (saved with the world " + hash + ")");
    }
    auto pending = std::make_unique<PendingLoad>();
    if (!vm_->decode(file, pending->vm, why)) {
        return fail(why);
    }
    const auto* rfst = file.find("RFST");
    if (rfst == nullptr) {
        return fail("the save lacks its RFST section");
    }
    if (rfst->version != vm::RefStates::k_section_version) {
        return fail("section RFST has a version this engine does not read");
    }
    if (!vm::RefStates::load(file.open(*rfst), pending->states)) {
        return fail("the RFST section of the save is damaged");
    }
    // ACTV is optional: a save without it has no changed actor values.
    if (const auto* actv = file.find("ACTV")) {
        if (actv->version != vm::ActorStates::k_section_version) {
            return fail("section ACTV has a version this engine does not read");
        }
        if (!vm::ActorStates::load(file.open(*actv), pending->actors)) {
            return fail("the ACTV section of the save is damaged");
        }
    }
    // DLGS is optional: a save without it has said nothing.
    if (const auto* dlgs = file.find("DLGS")) {
        if (dlgs->version != dialogue::DialogueStates::k_section_version) {
            return fail("section DLGS has a version this engine does not read");
        }
        if (!dialogue::DialogueStates::load(file.open(*dlgs), pending->dialogue)) {
            return fail("the DLGS section of the save is damaged");
        }
    }
    if (!decode_host(file, pending->host, why)) {
        return fail(why);
    }
    pending_ = std::move(pending);
    last_error_ = String();
    return godot::OK;
}

godot::Error SkydotPapyrus::prepare_load(const godot::Ref<SkydotSaveReader>& reader) {
    if (reader.is_null()) {
        last_error_ = "no save to read";
        return godot::ERR_INVALID_PARAMETER;
    }
    return prepare(reader->file());
}

void SkydotPapyrus::cancel_load() { pending_.reset(); }

godot::Error SkydotPapyrus::commit_load() {
    if (!pending_) {
        last_error_ = "no save was prepared";
        return godot::ERR_UNCONFIGURED;
    }
    auto pending = std::move(pending_);
    std::vector<std::uint32_t> gone;
    for (const auto& [id, placed] : vm_->placed().refs()) {
        gone.push_back(id);
    }
    reset();
    for (const auto id : gone) {
        emit_signal("ref_deleted", static_cast<std::int64_t>(id));
    }
    vm_->commit(std::move(pending->vm));
    states_ = std::move(pending->states);
    actors_.adopt(std::move(pending->actors));
    dialogue_states_.adopt(std::move(pending->dialogue));
    HostSnapshot& host = pending->host;
    for (const auto ref : host.attached) {
        attached_.insert(ref);
        if (!vm_->instances(ref).empty()) {
            register_trigger(ref, ref_info(ref));
        }
    }
    triggers_.restore(std::move(host.triggers));
    test_ids_ = host.test_ids;
    sound_ids_ = host.sound_ids;
    quests_ = std::move(host.quests);
    // The kept placed references are built again.
    for (const auto& [id, placed] : vm_->placed().refs()) {
        emit_signal("ref_placed", static_cast<std::int64_t>(id), static_cast<std::int64_t>(placed.base),
                    static_cast<std::int64_t>(placed.cell), vec3_of(placed.position), vec3_of(placed.rotation),
                    placed.scale, disabled(id));
    }
    last_error_ = String();
    return godot::OK;
}

godot::PackedByteArray SkydotPapyrus::save_state() const {
    godot::Ref<SkydotSaveWriter> writer;
    writer.instantiate();
    write_sections(writer);
    return writer->to_bytes();
}

godot::Error SkydotPapyrus::load_state(const godot::PackedByteArray& bytes) {
    godot::Ref<SkydotSaveReader> reader;
    reader.instantiate();
    if (const godot::Error e = reader->parse(bytes); e != godot::OK) {
        last_error_ = reader->get_error();
        return e;
    }
    if (const godot::Error e = prepare_load(reader); e != godot::OK) {
        return e;
    }
    return commit_load();
}

} // namespace skydot
