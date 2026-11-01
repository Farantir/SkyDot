// SPDX-License-Identifier: GPL-3.0-or-later
//
// References made at run time (PlaceAtMe, Delete) and destruction (DamageObject
// and the stages of a base's DEST). The state is the VM's (state/runtime_refs.hpp)
// and is saved with it; what the player sees is signalled to the game layer.
#include "vm/papyrus_common.hpp"
#include "vm/papyrus.hpp"
#include "data/actors.hpp"
#include "data/fb_search.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdio>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;
using godot::Vector3;

namespace skydot {

namespace {

constexpr std::int64_t k_stage_disable = 2;
constexpr std::int64_t k_stage_destroy = 4;

} // namespace

Dictionary SkydotPapyrus::placed_info(const vm::PlacedRef& placed) const {
    Dictionary info;
    info["base"] = static_cast<std::int64_t>(placed.base);
    info["position"] = vec3_of(placed.position);
    info["rotation"] = vec3_of(placed.rotation);
    info["scale"] = placed.scale;
    info["disabled"] = false;
    info["links"] = Array();
    // What activating it needs of the base: its record type and name.
    const Dictionary base = world_.is_valid() ? world_->get_base(placed.base) : Dictionary();
    info["type"] = base.get("type", String());
    info["editor_id"] = base.get("editor_id", String());
    info["scripts"] = base.get("scripts", Array());
    return info;
}

std::uint32_t SkydotPapyrus::cell_of(std::uint32_t ref) const {
    if (const auto* placed = vm_->placed().find(ref)) {
        return placed->cell;
    }
    if (world_.is_valid()) {
        // An actor a script moved stands in the cell of its new place.
        if (world_->placement().moved_place(ref) != nullptr) {
            if (const auto where = world_->placement().where(ref); where && where->cell != 0) {
                return where->cell;
            }
        }
        return static_cast<std::uint32_t>(world_->get_ref_cell(ref));
    }
    return 0;
}

void SkydotPapyrus::move_ref_to(std::uint32_t ref, std::uint32_t target, const Vector3& offset, bool match_rotation) {
    if (target == 0 || world_.is_null()) {
        return;
    }
    relocate_ref(ref, cell_of(target), position_of(target) + offset, match_rotation ? rotation_of(target) : rotation_of(ref));
}

void SkydotPapyrus::relocate_ref(std::uint32_t ref, std::uint32_t cell, const Vector3& position, const Vector3& rotation) {
    if (world_.is_null()) {
        return;
    }
    if (ref == k_player_ref) {
        // The player stands where the game layer put him; nothing teleports him from a script yet.
        std::printf("papyrus: the player was asked to move to (%.0f, %.0f, %.0f); not done\n", static_cast<double>(position.x),
                    static_cast<double>(position.y), static_cast<double>(position.z));
        return;
    }
    vm::MovedTo to;
    for (int i = 0; i < 3; ++i) {
        to.position[i] = static_cast<double>(position[i]);
        to.rotation[i] = static_cast<double>(rotation[i]);
    }
    const bool runtime = vm_->placed().find(ref) != nullptr;
    if (runtime) {
        vm_->placed().move(ref, cell, to.position, to.rotation);
    }
    if (actors_.is_actor(ref)) {
        // A placed actor's place is the placement's (the AI and the cell builder read it); a runtime
        // actor's is its record. The triggers see the move as any other step.
        if (cell != 0) {
            world_->placement().set_place(ref, world_->data().cell_space(cell), position, rotation.z);
        }
        update_actor(ref, position);
    } else {
        states_.moved[ref] = to;
    }
    emit_signal("ref_moved", static_cast<std::int64_t>(ref), position, rotation);
}

bool SkydotPapyrus::location_loaded(std::uint32_t location) {
    auto& cw = condition_world();
    for (const std::uint32_t cell : attached_cells_) {
        std::uint32_t at = cw.cell_location(cell);
        for (int depth = 0; at != 0 && depth < 32; ++depth) {
            if (at == location) {
                return true;
            }
            at = cw.location_parent(at);
        }
    }
    return false;
}

std::uint32_t SkydotPapyrus::place_at(std::uint32_t source, std::uint32_t base, bool initially_disabled) {
    const auto* explosion = world_.is_valid() ? world_->data().explosion_ptr(base) : nullptr;
    vm::PlacedRef placed;
    placed.base = base;
    placed.cell = cell_of(source);
    const Vector3 position = position_of(source);
    const Vector3 rotation = rotation_of(source);
    for (int i = 0; i < 3; ++i) {
        placed.position[i] = static_cast<double>(position[i]);
        placed.rotation[i] = static_cast<double>(rotation[i]);
    }
    vm::RefInfo source_info;
    placed.scale = ref_record(source, source_info) ? source_info.placement.scale : 1.0;
    placed.transient = explosion != nullptr;
    return place_ref(placed, initially_disabled);
}

std::uint32_t SkydotPapyrus::place_ref(const vm::PlacedRef& placed, bool initially_disabled) {
    const auto* explosion = world_.is_valid() ? world_->data().explosion_ptr(placed.base) : nullptr;
    const Vector3 position(static_cast<float>(placed.position[0]), static_cast<float>(placed.position[1]),
                           static_cast<float>(placed.position[2]));
    const Vector3 rotation(static_cast<float>(placed.rotation[0]), static_cast<float>(placed.rotation[1]),
                           static_cast<float>(placed.rotation[2]));
    const std::uint32_t base = placed.base;
    // Fresh ids first; once they run out a forgotten one is reused if nothing can still use it: not a Value of the
    // VM, and not a reference some store keeps as a value (`ForgetRegistry::collect_held`).
    const std::uint32_t id = vm_->placed().add(placed, [this] {
        auto held = vm_->held_forms(vm::id_ranges::runtime_refs);
        std::set<std::uint32_t> stores;
        vm_->forgets().collect_held(stores);
        outer_forgets_.collect_held(stores);
        held.insert(stores.begin(), stores.end());
        return held;
    });
    if (id == 0) {
        godot::UtilityFunctions::push_error("Papyrus: the runtime reference ids are used up; nothing placed");
        return 0;
    }
    if (vm_->placed().take_id_warning()) {
        godot::UtilityFunctions::push_warning("Papyrus: the runtime reference ids are 90 % used; they are reused once the fresh ones run out");
    }
    if (initially_disabled) {
        states_.disabled[id] = true;
    }
    // The base's scripts attach as for a placed reference, then OnInit; OnLoad
    // comes when the game layer has built it (ref_built).
    attached_.insert(id);
    if (attach_scripts(id, placed_info(placed).get("scripts", Array()), 0) != 0) {
        vm_->send_event(id, "OnInit", {});
        // Its OnInit runs now, not at the next update: a script puts itself in its state there (a dragon's
        // "alive"), and an event sent before that (a kill in the same frame) would find no handler.
        vm_->run_ready();
    }
    if (explosion != nullptr) {
        if (const auto sound = static_cast<std::int64_t>(explosion->sound()); sound != 0) {
            emit_signal("play_sound", sound, static_cast<std::int64_t>(id), new_sound_instance());
        }
    }
    emit_signal("ref_placed", static_cast<std::int64_t>(id), static_cast<std::int64_t>(base),
                static_cast<std::int64_t>(placed.cell), position, rotation, placed.scale, initially_disabled);
    return id;
}

std::uint32_t SkydotPapyrus::create_at_reference(std::uint32_t base, std::uint32_t at, bool disabled) {
    if (world_.is_null() || world_->data().root() == nullptr) {
        return 0;
    }
    const std::uint32_t npc = pick_actor_form(*world_->data().root(), base, 0);
    if (npc == 0 || lookup(world_->data().root()->npcs(), npc) == nullptr) {
        return 0; // an object (a book at a marker): not made yet
    }
    vm::PlacedRef placed;
    placed.base = npc;
    placed.cell = cell_of(at);
    const Vector3 position = position_of(at);
    const Vector3 rotation = rotation_of(at);
    for (int i = 0; i < 3; ++i) {
        placed.position[i] = static_cast<double>(position[i]);
    }
    placed.rotation[2] = static_cast<double>(rotation.z);
    placed.scale = 1.0;
    return place_ref(placed, disabled);
}

std::int64_t SkydotPapyrus::spawn_actor(std::int64_t base, std::int64_t cell, const Vector3& position,
                                        double rotation_z) {
    if (!world_.is_valid() || world_->data().root() == nullptr) {
        return 0;
    }
    const std::uint32_t npc = pick_actor_form(*world_->data().root(), static_cast<std::uint32_t>(base), 0);
    if (npc == 0 || lookup(world_->data().root()->npcs(), npc) == nullptr) {
        return 0;
    }
    vm::PlacedRef placed;
    placed.base = npc;
    placed.cell = static_cast<std::uint32_t>(cell);
    for (int i = 0; i < 3; ++i) {
        placed.position[i] = static_cast<double>(position[i]);
    }
    placed.rotation[0] = 0.0;
    placed.rotation[1] = 0.0;
    placed.rotation[2] = rotation_z;
    placed.scale = 1.0;
    return static_cast<std::int64_t>(place_ref(placed, false));
}

void SkydotPapyrus::remove_placed(std::int64_t ref) {
    const auto id = static_cast<std::uint32_t>(ref);
    if (vm_->placed().find(id) == nullptr) {
        return;
    }
    forget_ref(ref);
}

void SkydotPapyrus::forget_ref(std::int64_t ref) {
    const auto id = static_cast<std::uint32_t>(ref);
    const bool placed = vm_->placed().find(id) != nullptr;
    // Every store keyed by a reference is in a registry (state/ref_forget.hpp): the VM's and this object's, then
    // the subsystems' that registered through `forgets()`.
    vm_->forgets().forget(id);
    outer_forgets_.forget(id);
    if (placed) {
        // Every store has forgotten it: the id may be handed out again once nothing holds it (`place_ref`).
        vm_->placed().release(id);
        emit_signal("ref_deleted", ref);
    }
}

void SkydotPapyrus::register_forgets() {
    auto& r = vm_->forgets();
    // The stores live in this object, which owns the VM and its registry: no tokens needed.
    r.add_owned("placed", [this](std::uint32_t id) { vm_->placed().remove(id); },
                [this] { return vm_->placed().refs().size(); });
    r.add_owned("destruction", [this](std::uint32_t id) { vm_->destruction().erase(id); },
                [this] { return vm_->destruction().states().size(); });
    r.add_owned("ref_states", [this](std::uint32_t id) { states_.forget(id); }, [this] { return states_.entries(); });
    r.add_owned("actor_states", [this](std::uint32_t id) { actors_.forget(id); }, [this] { return actors_.entries(); },
                [this](std::set<std::uint32_t>& out) { actors_.held_refs(out); });
    r.add_owned("item_states", [this](std::uint32_t id) { item_.states().erase(id); }, [this] { return item_.states().entries(); });
    r.add_owned("attached", [this](std::uint32_t id) { attached_.erase(id); }, [this] { return attached_.size(); });
    r.add_owned("loaded", [this](std::uint32_t id) { loaded_.erase(id); }, [this] { return loaded_.size(); });
    r.add_owned("volume_loaded", [this](std::uint32_t id) { volume_loaded_.erase(id); },
                [this] { return volume_loaded_.size(); });
    r.add_owned("ghost_reported", [this](std::uint32_t id) { combat_.forget_ghost(id); },
                [this] { return combat_.ghost_count(); });
    r.add_owned("triggers", [this](std::uint32_t id) { triggers_.forget(id); },
                [this] { return triggers_.triggers().size() + triggers_.actors().size(); },
                [this](std::set<std::uint32_t>& out) { triggers_.held_refs(out); });
    r.add_owned("harvests", [this](std::uint32_t id) { activation_.harvests().erase(id); },
                [this] { return activation_.harvests().size(); });
    r.add_owned("last_damager", [this](std::uint32_t id) { combat_.forget_damager(id); },
                [this] { return combat_.damager_count(); });
    r.add_owned("pending_activations", [this](std::uint32_t id) { activation_.forget(id); },
                [this] { return activation_.pending_count(); },
                [this](std::set<std::uint32_t>& out) { activation_.held_refs(out); });
    r.add_owned("dialogue_talked", [this](std::uint32_t id) { dialogue_.forget_talked(id); },
                [this] { return dialogue_.talked_count(); });
    r.add_owned("scene_pushed_packages", [this](std::uint32_t id) { scene_.forget(id); },
                [this] { return scene_.pushed_count(); },
                [this](std::set<std::uint32_t>& out) { scene_.held_refs(out); });
    r.add_owned("quest_alias_fills", [this](std::uint32_t id) { quests_.forget_fill(id); },
                [this] { return quests_.fills().size(); });
    r.add_owned("actor_placement", [this](std::uint32_t id) {
        if (world_.is_valid()) {
            world_->placement().forget(id);
        }
    }, [this] { return world_.is_valid() ? world_->placement().entries() : std::size_t{0}; });
}

godot::Dictionary SkydotPapyrus::forget_store_sizes() const {
    godot::Dictionary out;
    for (const auto& [name, size] : vm_->forgets().sizes()) {
        out[godot::String::utf8(name.c_str())] = static_cast<std::int64_t>(size);
    }
    for (const auto& [name, size] : outer_forgets_.sizes()) {
        out[godot::String::utf8(name.c_str())] = static_cast<std::int64_t>(size);
    }
    return out;
}

std::int64_t SkydotPapyrus::ref_built(std::int64_t ref) {
    const auto id = static_cast<std::uint32_t>(ref);
    if (vm_->instances(id).empty()) {
        return 0;
    }
    if (const auto* placed = vm_->placed().find(id)) {
        loaded_[id] = placed->cell;
        attached_cells_.insert(placed->cell);
    }
    dispatch(id, "OnCellAttach", {});
    dispatch(id, "OnCellLoad", {});
    return static_cast<std::int64_t>(dispatch(id, "OnLoad", {}));
}

const bethconv::pack::wfb::Destruction* SkydotPapyrus::destruction_of(std::uint32_t ref) const {
    const auto base = base_form_of(ref);
    return base != 0 && world_.is_valid() ? world_->data().destruction_ptr(base) : nullptr;
}

void SkydotPapyrus::damage_object(std::uint32_t ref, double amount) {
    const auto* dest = destruction_of(ref);
    if (dest == nullptr || amount <= 0.0) {
        return;
    }
    const double health = static_cast<double>(dest->health());
    const auto* stages = dest->stages();
    const auto stage_count = stages != nullptr ? static_cast<std::int32_t>(stages->size()) : 0;
    vm::DestructionState state = vm_->destruction().get(ref);
    if (state.destroyed) {
        return;
    }
    state.damage = health > 0.0 ? std::min(state.damage + amount, health) : state.damage + amount;
    const double remaining = health > 0.0 ? (health - state.damage) * 100.0 / health : 0.0;
    state.stage = std::clamp(state.stage, std::int32_t{0}, stage_count); // a stage from a save may lie outside the model
    std::int32_t stage = state.stage;
    for (std::int32_t i = 0; i < stage_count; ++i) {
        if (remaining <= static_cast<double>(stages->Get(static_cast<flatbuffers::uoffset_t>(i))->health_percent())) {
            stage = std::max(stage, static_cast<std::int32_t>(i + 1));
        }
    }
    const std::int32_t before = state.stage;
    state.stage = stage;
    // Every stage opened by this blow takes effect, in order.
    for (std::int32_t s = before + 1; s <= stage; ++s) {
        const auto* opened = stages->Get(static_cast<flatbuffers::uoffset_t>(s - 1));
        if (opened->model() != nullptr && opened->model()->size() != 0) {
            emit_signal("destruction_model", static_cast<std::int64_t>(ref),
                        String::utf8(opened->model()->c_str()), static_cast<std::int64_t>(s));
        }
        if (const auto explosion = opened->explosion(); explosion != 0) {
            place_at(ref, explosion, false);
        }
        const std::int64_t flags = opened->flags();
        if ((flags & k_stage_disable) != 0) {
            set_disabled(ref, true);
        }
        if ((flags & k_stage_destroy) != 0) {
            state.destroyed = true;
        }
    }
    vm_->destruction().set(ref, state);
    if (stage != before) {
        char line[96];
        std::snprintf(line, sizeof line, "0x%08X destruction stage %d -> %d", ref, before, stage);
        godot::UtilityFunctions::print(String(line));
        dispatch(ref, "OnDestructionStageChanged", {vm::Value::integer(before), vm::Value::integer(stage)});
    }
}

void SkydotPapyrus::clear_destruction(std::uint32_t ref) {
    const vm::DestructionState state = vm_->destruction().get(ref);
    vm_->destruction().erase(ref);
    if (state.stage > 0) {
        emit_signal("destruction_model", static_cast<std::int64_t>(ref), String(), static_cast<std::int64_t>(0));
    }
}

void SkydotPapyrus::bind_runtime_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    using namespace natives;
    auto& v = *vm_;
    const auto arg = natives::arg;
    const auto number = natives::number;

    // ---- PlaceAtMe and Delete
    // `count` copies at the caller's position and rotation, in its cell; the
    // last is returned. The game layer builds them (ref_placed).
    v.bind("ObjectReference", "PlaceAtMe", [this, arg](NativeCall& c) {
        if (arg(c, 0).kind() != vm::Kind::object || world_.is_null()) {
            return NativeResult{};
        }
        const std::int32_t count = c.args.size() > 1 ? std::max(1, arg(c, 1).i()) : 1;
        std::uint32_t last = 0;
        for (std::int32_t i = 0; i < std::min(count, 64); ++i) {
            last = place_at(c.self.form(), arg(c, 0).form(), vm::Vm::truthy(arg(c, 3)));
        }
        return natives::result(c.vm.object(last, "ObjectReference"));
    });
    // A placed reference is gone; one placed in the editor only goes
    // disabled (Skyrim deletes it for good, which nothing here can undo).
    for (const char* name : {"Delete", "DeleteWhenAble"}) {
        v.bind("ObjectReference", name, [this](NativeCall& c) {
            if (vm_->placed().find(c.self.form()) != nullptr) {
                remove_placed(c.self.form());
            } else {
                set_disabled(c.self.form(), true);
            }
            return NativeResult{};
        });
    }

    // ---- destruction
    v.bind("ObjectReference", "DamageObject", [this, arg, number](NativeCall& c) {
        damage_object(c.self.form(), number(arg(c, 0)));
        return NativeResult{};
    });
    v.bind("ObjectReference", "ClearDestruction", [this](NativeCall& c) {
        clear_destruction(c.self.form());
        return NativeResult{};
    });
    v.bind("ObjectReference", "SetDestroyed", [this, arg](NativeCall& c) {
        vm::DestructionState state = vm_->destruction().get(c.self.form());
        state.destroyed = c.args.empty() || vm::Vm::truthy(arg(c, 0));
        vm_->destruction().set(c.self.form(), state);
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsDestroyed", [this](NativeCall& c) {
        return natives::result(Value::boolean(vm_->destruction().get(c.self.form()).destroyed));
    });
    v.bind("ObjectReference", "GetCurrentDestructionStage", [this](NativeCall& c) {
        return natives::result(Value::integer(vm_->destruction().get(c.self.form()).stage));
    });
}

} // namespace skydot
