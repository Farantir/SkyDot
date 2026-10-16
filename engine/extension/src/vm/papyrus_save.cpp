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
using godot::Vector3;
using save::ByteReader;
using save::ByteWriter;

namespace {

constexpr std::uint32_t k_section_version = 1;

std::string utf8_of(const String& s) {
    const auto bytes = s.utf8();
    return {bytes.get_data(), static_cast<std::size_t>(bytes.length())};
}

String godot_of(const std::string& s) { return String::utf8(s.c_str(), static_cast<int>(s.size())); }

bool open_section(const save::SaveReader& file, const char* tag, ByteReader& out, std::string& error) {
    const auto* s = file.find(tag);
    if (s == nullptr) {
        error = std::string("the save lacks its ") + tag + " section";
        return false;
    }
    if (s->version != k_section_version) {
        error = std::string("section ") + tag + " has version " + std::to_string(s->version) +
                ", which this engine does not read (it reads " + std::to_string(k_section_version) + ")";
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
    p.u32(static_cast<std::uint32_t>(triggers_.size()));
    for (const auto& [ref, t] : triggers_) {
        p.u32(ref);
        p.u32(static_cast<std::uint32_t>(t.inside.size()));
        for (const auto a : t.inside) {
            p.u32(a);
        }
    }
    p.u32(static_cast<std::uint32_t>(actors_.size()));
    for (const auto& [ref, position] : actors_) {
        p.u32(ref);
        p.f64(static_cast<double>(position.x));
        p.f64(static_cast<double>(position.y));
        p.f64(static_cast<double>(position.z));
    }
    p.u32(next_form_);
    out.add("PAPY", k_section_version, p.out);

    ByteWriter q;
    q.u32(static_cast<std::uint32_t>(quests_.size()));
    for (const auto& [quest, state] : quests_) {
        q.u32(quest);
        q.u8(state.running ? 1 : 0);
        q.u8(state.started ? 1 : 0);
        q.u8(state.completed ? 1 : 0);
        q.u8(state.failed ? 1 : 0);
        q.u8(state.active ? 1 : 0);
        q.u32(static_cast<std::uint32_t>(state.stage));
        q.u32(static_cast<std::uint32_t>(state.done.size()));
        for (const auto stage : state.done) {
            q.u32(static_cast<std::uint32_t>(stage));
        }
        q.u32(static_cast<std::uint32_t>(state.objectives.size()));
        for (const auto& [index, bits] : state.objectives) {
            q.u32(static_cast<std::uint32_t>(index));
            q.u8(bits);
        }
    }
    q.u32(static_cast<std::uint32_t>(quest_scripts_.size()));
    for (const auto quest : quest_scripts_) {
        q.u32(quest);
    }
    q.u32(static_cast<std::uint32_t>(alias_handles_.size()));
    for (const auto& [key, handle] : alias_handles_) {
        const auto fill = alias_fill_.find(handle);
        q.u32(key.first);
        q.u32(key.second);
        q.u32(handle);
        q.u32(fill != alias_fill_.end() ? fill->second : 0U);
    }
    q.u32(static_cast<std::uint32_t>(globals_.size()));
    for (const auto& [global, value] : globals_) {
        q.u32(global);
        q.f64(value);
    }
    q.u32(next_alias_handle_);
    out.add("QUST", k_section_version, q.out);
}

bool SkydotPapyrus::decode_host(const save::SaveReader& file, HostSnapshot& out, std::string& error) const {
    ByteReader r({}, 0);
    const auto fail = [&](const char* tag) {
        error = std::string("the ") + tag + " section of the save is damaged";
        return false;
    };
    if (!open_section(file, "PAPY", r, error)) {
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
    if (!r.count(n, 8)) {
        return fail("PAPY");
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0, inside = 0;
        if (!r.u32(ref) || !r.count(inside, 4)) {
            return fail("PAPY");
        }
        auto& list = out.inside[ref];
        for (std::uint32_t k = 0; k < inside; ++k) {
            std::uint32_t actor = 0;
            if (!r.u32(actor)) {
                return fail("PAPY");
            }
            list.insert(actor);
        }
    }
    if (!r.count(n, 28)) {
        return fail("PAPY");
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0;
        double x = 0.0, y = 0.0, z = 0.0;
        if (!r.u32(ref) || !r.f64(x) || !r.f64(y) || !r.f64(z)) {
            return fail("PAPY");
        }
        out.actors[ref] = Vector3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    }
    if (!r.u32(out.next_form) || !r.at_end()) {
        return fail("PAPY");
    }

    if (!open_section(file, "QUST", r, error)) {
        return false;
    }
    if (!r.count(n, 24)) {
        return fail("QUST");
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t quest = 0, stage = 0, done = 0, objectives = 0;
        std::uint8_t flags[5] = {};
        QuestState q;
        bool ok = r.u32(quest);
        for (auto& f : flags) {
            ok = ok && r.u8(f);
        }
        ok = ok && r.u32(stage) && r.count(done, 4);
        if (!ok) {
            return fail("QUST");
        }
        q.running = flags[0] != 0;
        q.started = flags[1] != 0;
        q.completed = flags[2] != 0;
        q.failed = flags[3] != 0;
        q.active = flags[4] != 0;
        q.stage = static_cast<std::int32_t>(stage);
        for (std::uint32_t k = 0; k < done; ++k) {
            std::uint32_t d = 0;
            if (!r.u32(d)) {
                return fail("QUST");
            }
            q.done.insert(static_cast<std::int32_t>(d));
        }
        if (!r.count(objectives, 5)) {
            return fail("QUST");
        }
        for (std::uint32_t k = 0; k < objectives; ++k) {
            std::uint32_t index = 0;
            std::uint8_t bits = 0;
            if (!r.u32(index) || !r.u8(bits)) {
                return fail("QUST");
            }
            q.objectives[static_cast<std::int32_t>(index)] = bits;
        }
        out.quests[quest] = std::move(q);
    }
    if (!r.count(n, 4)) {
        return fail("QUST");
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t quest = 0;
        if (!r.u32(quest)) {
            return fail("QUST");
        }
        out.quest_scripts.insert(quest);
    }
    if (!r.count(n, 16)) {
        return fail("QUST");
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::array<std::uint32_t, 4> alias{};
        for (auto& v : alias) {
            if (!r.u32(v)) {
                return fail("QUST");
            }
        }
        out.aliases.push_back(alias);
    }
    if (!r.count(n, 12)) {
        return fail("QUST");
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t global = 0;
        double value = 0.0;
        if (!r.u32(global) || !r.f64(value)) {
            return fail("QUST");
        }
        out.globals[global] = value;
    }
    if (!r.u32(out.next_alias_handle) || !r.at_end()) {
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
    if (!open_section(file, "HEAD", r, why)) {
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
    HostSnapshot& host = pending->host;
    for (const auto ref : host.attached) {
        attached_.insert(ref);
        if (!vm_->instances(ref).empty()) {
            register_trigger(ref, ref_info(ref));
        }
    }
    for (const auto& [ref, actors] : host.inside) {
        if (const auto it = triggers_.find(ref); it != triggers_.end()) {
            it->second.inside = actors;
        }
    }
    actors_ = std::move(host.actors);
    next_form_ = host.next_form;
    quests_ = std::move(host.quests);
    quest_scripts_ = std::move(host.quest_scripts);
    for (const auto& a : host.aliases) {
        const auto key = std::pair{a[0], a[1]};
        alias_handles_[key] = a[2];
        handle_alias_[a[2]] = key;
        alias_fill_[a[2]] = a[3];
    }
    globals_ = std::move(host.globals);
    next_alias_handle_ = host.next_alias_handle;
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
