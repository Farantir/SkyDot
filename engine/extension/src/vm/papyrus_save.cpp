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
//   ITST  (optional) resolved owners, leveled actor picks and equipment, vm/item_states.hpp
//   CRFT  (optional) created potions and alchemy knowledge, vm/created_forms.hpp
//   HRVS  (optional) harvested references, vm/harvest.hpp
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

#include "magic/magic_save.hpp"
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

void SkydotPapyrus::capture_actor_places() {
    if (world_.is_null()) {
        return;
    }
    actors_.clear_living_places();
    const auto keep = [this](std::uint32_t ref, std::uint32_t space, const godot::Vector3& at, double rot_z) {
        if (ref != k_player_ref && space != 0 && actors_.is_actor(ref) && !actors_.is_dead(ref)) {
            actors_.set_living_place(ref, {.space = space,
                                           .x = static_cast<double>(at.x),
                                           .y = static_cast<double>(at.y),
                                           .z = static_cast<double>(at.z),
                                           .rot_z = rot_z});
        }
    };
    // Actors on screen: where they are now. A script-placed one also updates its record, which is its place.
    for (const auto& [ref, at] : triggers_.actors()) {
        if (ref == k_player_ref || !actors_.is_actor(ref)) {
            continue;
        }
        const godot::Vector3 position(static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(at.z));
        if (const auto* record = vm_->placed().find(ref)) {
            const double xyz[3] = {static_cast<double>(at.x), static_cast<double>(at.y), static_cast<double>(at.z)};
            vm_->placed().move(ref, 0, xyz, record->rotation);
        } else {
            keep(ref, get_ref_space(ref), position, static_cast<double>(rotation_of(ref).z));
        }
    }
    // The others: where the placement put them (the AI's walks, MoveTo).
    for (const auto& [ref, place] : world_->placement().places()) {
        if (!triggers_.actors().contains(ref) && vm_->placed().find(ref) == nullptr) {
            keep(ref, place.space, place.position, static_cast<double>(place.rotation_z));
        }
    }
}

void SkydotPapyrus::write_sections(const godot::Ref<SkydotSaveWriter>& writer) {
    capture_actor_places();
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
    if (const auto magc = magic_section(); !magc.empty()) {
        out.add("MAGC", magic::k_magc_version, magc);
    }
    out.add("ITST", vm::ItemStates::k_section_version, items_.save(vm_->inventory()));
    out.add("CRFT", vm::CreatedForms::k_section_version, created_.save());
    out.add("HRVS", vm::HarvestStates::k_section_version, harvests_.save());
    out.add("DLGS", dialogue::DialogueStates::k_section_version, dialogue_states_.save());
    if (scenes_ != nullptr) {
        out.add("SCEN", scenes::Scenes::k_section_version, scenes_->save());
    }
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
        if (actv->version < 1 || actv->version > vm::ActorStates::k_section_version) {
            return fail("section ACTV has a version this engine does not read");
        }
        if (!vm::ActorStates::load(file.open(*actv), pending->actors, actv->version)) {
            return fail("the ACTV section of the save is damaged");
        }
    }
    // ITST is optional: a save without it has nothing equipped, and the owners
    // that hold items count as resolved; the others resolve on touch.
    if (const auto* itst = file.find("ITST")) {
        if (itst->version < 1 || itst->version > vm::ItemStates::k_section_version) {
            return fail("section ITST has a version this engine does not read");
        }
        if (!vm::ItemStates::load(file.open(*itst), pending->items, itst->version)) {
            return fail("the ITST section of the save is damaged");
        }
        // The splits belong to the inventory the VM takes over.
        pending->vm.inventory.set_splits(pending->items.take_splits());
    } else {
        // An old save: what is in INVT already has its start items.
        pending->items.mark_holders_resolved(pending->vm.inventory);
    }
    // MAGC is optional: a save without it has no effect running and no spell changed.
    if (const auto* magc = file.find("MAGC")) {
        if (magc->version < magic::k_magc_min_version || magc->version > magic::k_magc_version) {
            return fail("section MAGC has a version this engine does not read");
        }
        if (!magic_decode(file.open(*magc), pending->magic_effects, pending->magic_book, pending->magic_shouts)) {
            return fail("the MAGC section of the save is damaged");
        }
        pending->has_magic = true;
    }
    // CRFT and HRVS are optional: an older save has nothing created, known or harvested.
    if (const auto* crft = file.find("CRFT")) {
        if (crft->version < 1 || crft->version > vm::CreatedForms::k_section_version) {
            return fail("section CRFT has a version this engine does not read");
        }
        if (!vm::CreatedForms::load(file.open(*crft), pending->created, crft->version)) {
            return fail("the CRFT section of the save is damaged");
        }
    }
    if (const auto* hrvs = file.find("HRVS")) {
        if (hrvs->version != vm::HarvestStates::k_section_version) {
            return fail("section HRVS has a version this engine does not read");
        }
        if (!vm::HarvestStates::load(file.open(*hrvs), pending->harvests)) {
            return fail("the HRVS section of the save is damaged");
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
    // SCEN is optional: a save without it has no scene running.
    if (const auto* scen = file.find("SCEN")) {
        if (scen->version != scenes::Scenes::k_section_version) {
            return fail("section SCEN has a version this engine does not read");
        }
        auto* logic = scene_logic();
        pending->scenes = logic != nullptr ? std::make_unique<scenes::Scenes>(*world_->data().root(), *scene_host_) : nullptr;
        if (pending->scenes == nullptr || !scenes::Scenes::load(file.open(*scen), *pending->scenes)) {
            return fail("the SCEN section of the save is damaged");
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
    player_dead_ = false; // a loaded game has its own Health
    last_damager_ = 0;
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
    if (world_.is_valid()) {
        // Actors scripts enabled are built by the cell builder again.
        world_->placement().clear_enabled();
        for (const auto& [ref, off] : states_.disabled) {
            if (!off && world_->data().actor_ptr(ref) != nullptr) {
                world_->placement().set_enabled(ref, true);
            }
        }
    }
    // The dead lie where they fell, the living where they stood.
    if (world_.is_valid()) {
        for (const auto& [ref, place] : actors_.living_places()) {
            world_->placement().set_place(ref, place.space,
                                          godot::Vector3(static_cast<real_t>(place.x), static_cast<real_t>(place.y),
                                                         static_cast<real_t>(place.z)),
                                          static_cast<float>(place.rot_z));
        }
        for (const auto& [ref, place] : actors_.corpse_places()) {
            world_->placement().set_place(ref, place.space,
                                          godot::Vector3(static_cast<real_t>(place.x), static_cast<real_t>(place.y),
                                                         static_cast<real_t>(place.z)),
                                          static_cast<float>(place.rot_z));
        }
    }
    items_.adopt(std::move(pending->items));
    created_.adopt(std::move(pending->created));
    harvests_.adopt(std::move(pending->harvests));
    dialogue_states_.adopt(std::move(pending->dialogue));
    // After the items: worn enchantments are derived from what is worn.
    magic_adopt(pending->has_magic, pending->magic_effects, pending->magic_book, pending->magic_shouts);
    if (auto* logic = scene_logic()) {
        if (pending->scenes != nullptr) {
            logic->adopt(std::move(*pending->scenes));
            // The AI forgot the pushed packages with the rest of its state.
            for (const auto& [actor, package] : logic->pushed_packages()) {
                emit_signal("scene_package", static_cast<std::int64_t>(actor), static_cast<std::int64_t>(package), true);
            }
        } else {
            logic->clear();
        }
    }
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

godot::PackedByteArray SkydotPapyrus::save_state() {
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
