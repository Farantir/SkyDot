// SPDX-License-Identifier: GPL-3.0-or-later
//
// References made at run time (PlaceAtMe, Delete) and destruction (DamageObject
// and the stages of a base's DEST). The state is the VM's (vm/runtime_refs.hpp)
// and is saved with it; what the player sees is signalled to the game layer.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"

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
    info["scripts"] = world_.is_valid() ? world_->get_base(placed.base).get("scripts", Array()) : Variant(Array());
    return info;
}

std::uint32_t SkydotPapyrus::cell_of(std::uint32_t ref) const {
    if (const auto* placed = vm_->placed().find(ref)) {
        return placed->cell;
    }
    return world_.is_valid() ? static_cast<std::uint32_t>(world_->get_ref_cell(ref)) : 0;
}

std::uint32_t SkydotPapyrus::place_at(std::uint32_t source, std::uint32_t base, bool initially_disabled) {
    const Dictionary explosion = world_.is_valid() ? world_->get_explosion(base) : Dictionary();
    vm::PlacedRef placed;
    placed.base = base;
    placed.cell = cell_of(source);
    const Vector3 position = position_of(source);
    const Vector3 rotation = rotation_of(source);
    for (int i = 0; i < 3; ++i) {
        placed.position[i] = static_cast<double>(position[i]);
        placed.rotation[i] = static_cast<double>(rotation[i]);
    }
    placed.scale = static_cast<double>(ref_info(source).get("scale", 1.0));
    placed.transient = !explosion.is_empty();
    const std::uint32_t id = vm_->placed().add(placed);
    if (initially_disabled) {
        states_.disabled[id] = true;
    }
    // The base's scripts attach as for a placed reference, then OnInit; OnLoad
    // comes when the game layer has built it (ref_built).
    attached_.insert(id);
    if (attach_scripts(id, placed_info(placed).get("scripts", Array()), 0) != 0) {
        vm_->send_event(id, "OnInit", {});
    }
    if (!explosion.is_empty()) {
        if (const auto sound = static_cast<std::int64_t>(explosion.get("sound", 0)); sound != 0) {
            emit_signal("play_sound", sound, static_cast<std::int64_t>(id), next_sound_instance_++);
        }
    }
    emit_signal("ref_placed", static_cast<std::int64_t>(id), static_cast<std::int64_t>(base),
                static_cast<std::int64_t>(placed.cell), position, rotation, placed.scale, initially_disabled);
    return id;
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
    const bool placed = vm_->placed().remove(id);
    vm_->detach(id);
    vm_->destruction().erase(id);
    states_.forget(id);
    attached_.erase(id);
    triggers_.erase(id);
    actors_.erase(id);
    if (placed) {
        emit_signal("ref_deleted", ref);
    }
}

std::int64_t SkydotPapyrus::ref_built(std::int64_t ref) {
    const auto id = static_cast<std::uint32_t>(ref);
    if (vm_->instances(id).empty()) {
        return 0;
    }
    dispatch(id, "OnCellAttach", {});
    dispatch(id, "OnCellLoad", {});
    return static_cast<std::int64_t>(dispatch(id, "OnLoad", {}));
}

Dictionary SkydotPapyrus::destruction_of(std::uint32_t ref) const {
    const auto base = form_of(ref_info(ref).get("base", 0));
    return base != 0 && world_.is_valid() ? world_->get_destruction(base) : Dictionary();
}

void SkydotPapyrus::damage_object(std::uint32_t ref, double amount) {
    const Dictionary dest = destruction_of(ref);
    if (dest.is_empty() || amount <= 0.0) {
        return;
    }
    const double health = static_cast<double>(dest["health"]);
    const Array stages = dest["stages"];
    vm::DestructionState state = vm_->destruction().get(ref);
    if (state.destroyed) {
        return;
    }
    state.damage = health > 0.0 ? std::min(state.damage + amount, health) : state.damage + amount;
    const double remaining = health > 0.0 ? (health - state.damage) * 100.0 / health : 0.0;
    std::int32_t stage = state.stage;
    for (std::int64_t i = 0; i < stages.size(); ++i) {
        if (remaining <= static_cast<double>(Dictionary(stages[i]).get("health_percent", 0))) {
            stage = std::max(stage, static_cast<std::int32_t>(i + 1));
        }
    }
    const std::int32_t before = state.stage;
    state.stage = stage;
    // Every stage opened by this blow takes effect, in order.
    for (std::int32_t s = before + 1; s <= stage; ++s) {
        const Dictionary opened = stages[s - 1];
        if (const String model = opened.get("model", String()); !model.is_empty()) {
            emit_signal("destruction_model", static_cast<std::int64_t>(ref), model, static_cast<std::int64_t>(s));
        }
        if (const auto explosion = static_cast<std::int64_t>(opened.get("explosion", 0)); explosion != 0) {
            place_at(ref, static_cast<std::uint32_t>(explosion), false);
        }
        const auto flags = static_cast<std::int64_t>(opened.get("flags", 0));
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
