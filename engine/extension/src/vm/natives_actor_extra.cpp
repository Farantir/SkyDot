// SPDX-License-Identifier: GPL-3.0-or-later
//
// What scripts do to an actor beyond its values (step 7b-7, the dragon's death): factions at runtime,
// ghosts, a visual effect or a sub graph animation (signals for the game layer), arrows in the body, the
// worldspace a reference stands in, and the message that a dragon soul was absorbed.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdio>

namespace skydot {

void SkydotPapyrus::bind_actor_extra_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    const auto arg = natives::arg;
    const auto flag = [](bool v) { return NativeResult{.value = Value::boolean(v), .wait = -1.0}; };
    const auto faction_of = [arg](NativeCall& c) { return arg(c, 0).kind() == vm::Kind::object ? arg(c, 0).form() : 0U; };

    // ---- factions: the NPC_'s, then what scripts did (ActorStates)
    const auto rank_of = [this](std::uint32_t ref, std::uint32_t faction) {
        return condition_world().faction_rank(ref, faction).value_or(-1);
    };
    vm_->bind("Actor", "IsInFaction", [faction_of, rank_of, flag](NativeCall& c) {
        const std::uint32_t faction = faction_of(c);
        return flag(faction != 0 && rank_of(c.self.form(), faction) >= 0);
    });
    vm_->bind("Actor", "GetFactionRank", [faction_of, rank_of](NativeCall& c) {
        const std::uint32_t faction = faction_of(c);
        return NativeResult{.value = Value::integer(faction != 0 ? rank_of(c.self.form(), faction) : -1), .wait = -1.0};
    });
    vm_->bind("Actor", "SetFactionRank", [this, faction_of](NativeCall& c) {
        if (const std::uint32_t faction = faction_of(c); faction != 0) {
            actors_.set_faction_rank(c.self.form(), faction, natives::arg(c, 1).i());
        }
        return NativeResult{};
    });
    vm_->bind("Actor", "ModFactionRank", [this, faction_of, rank_of](NativeCall& c) {
        const std::uint32_t faction = faction_of(c);
        const std::int32_t rank = faction != 0 ? rank_of(c.self.form(), faction) : -1;
        if (rank >= 0) {
            actors_.set_faction_rank(c.self.form(), faction, rank + natives::arg(c, 1).i());
        }
        return NativeResult{};
    });
    vm_->bind("Actor", "RemoveFromFaction", [this, faction_of](NativeCall& c) {
        if (const std::uint32_t faction = faction_of(c); faction != 0) {
            actors_.set_faction_rank(c.self.form(), faction, -1);
        }
        return NativeResult{};
    });
    vm_->bind("Actor", "RemoveFromAllFactions", [this](NativeCall& c) {
        for (const std::uint32_t faction : condition_world().factions_of(c.self.form())) {
            actors_.set_faction_rank(c.self.form(), faction, -1);
        }
        return NativeResult{};
    });

    // ---- ghosts take no hits (apply_hit, magic landing)
    vm_->bind("Actor", "SetGhost", [this](NativeCall& c) {
        actors_.set_ghost(c.self.form(), !c.args.empty() && vm::Vm::truthy(c.args[0]));
        return NativeResult{};
    });
    vm_->bind("Actor", "IsGhost", [this, flag](NativeCall& c) { return flag(actors_.is_ghost(c.self.form())); });

    // ---- effects and animations the game layer shows (placeholders there)
    vm_->bind("VisualEffect", "Play", [this, arg](NativeCall& c) {
        emit_signal("visual_effect", static_cast<std::int64_t>(c.self.form()), static_cast<std::int64_t>(arg(c, 0).form()),
                    static_cast<std::int64_t>(arg(c, 2).form()), static_cast<double>(natives::number(arg(c, 1))), true);
        return NativeResult{};
    });
    vm_->bind("VisualEffect", "Stop", [this, arg](NativeCall& c) {
        emit_signal("visual_effect", static_cast<std::int64_t>(c.self.form()), static_cast<std::int64_t>(arg(c, 0).form()),
                    static_cast<std::int64_t>(0), 0.0, false);
        return NativeResult{};
    });
    vm_->bind("Actor", "PlaySubGraphAnimation", [this, arg](NativeCall& c) {
        emit_signal("sub_graph_animation", static_cast<std::int64_t>(c.self.form()),
                    natives::to_godot(vm::Vm::to_string(arg(c, 0))));
        return NativeResult{};
    });

    // Arrows stuck in a body are not simulated: nothing to clear.
    vm_->bind("Actor", "ClearExtraArrows", [](NativeCall&) { return NativeResult{}; });

    // ObjectReference.GetCurrentLocation: the location of the reference's cell; None without one.
    vm_->bind("ObjectReference", "GetCurrentLocation", [this](NativeCall& c) {
        auto& cw = condition_world();
        const std::uint32_t cell = cw.cell_of(c.self.form());
        const std::uint32_t location = cell != 0 ? cw.cell_location(cell) : 0;
        return NativeResult{.value = location != 0 ? c.vm.object(location, "Location") : Value{}, .wait = -1.0};
    });

    // ObjectReference.GetWorldSpace: the worldspace of the reference's cell; None in an interior.
    vm_->bind("ObjectReference", "GetWorldSpace", [this](NativeCall& c) {
        auto& cw = condition_world();
        const std::uint32_t cell = cw.cell_of(c.self.form());
        const std::uint32_t space = cell != 0 ? cw.cell_world(cell) : 0;
        return NativeResult{.value = space != 0 ? c.vm.object(space, "WorldSpace") : Value{}, .wait = -1.0};
    });
}

bool SkydotPapyrus::ghost_ignores_hit(std::uint32_t ref) {
    if (!actors_.is_ghost(ref)) {
        return false;
    }
    if (ghost_reported_.insert(ref).second) {
        char line[96];
        std::snprintf(line, sizeof line, "combat: 0x%X is a ghost and takes no hits", ref);
        godot::UtilityFunctions::print(godot::String::utf8(line));
    }
    return true;
}

void SkydotPapyrus::dragon_soul_absorbed() {
    const auto* g = world_.is_valid() ? WorldData::find_gmst(world_->data().root(), "sDragonSoulAcquired") : nullptr;
    const std::string text = g != nullptr && g->text_value() != nullptr && g->text_value()->size() > 0
                                 ? g->text_value()->str()
                                 : std::string("Dragon Soul Absorbed");
    godot::UtilityFunctions::print(natives::to_godot("[Notification] " + text));
    emit_signal("message", natives::to_godot(text), false);
}

} // namespace skydot
