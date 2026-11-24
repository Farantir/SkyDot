// SPDX-License-Identifier: GPL-3.0-or-later
//
// The combat domain of the Papyrus runtime (vm/combat_runtime.hpp).
#include "vm/combat_runtime.hpp"
#include "vm/natives_args.hpp"

#include "state/actor_values.hpp"
#include "vm/activation.hpp"

#include <cstdio>

namespace skydot::vm {

bool CombatRuntime::ghost_ignores_hit(std::uint32_t ref) {
    if (!actors_->actor_states().is_ghost(ref)) {
        return false;
    }
    if (ghost_reported_.insert(ref).second) {
        char line[96];
        std::snprintf(line, sizeof line, "combat: 0x%X is a ghost and takes no hits", ref);
        host_->log(line);
    }
    return true;
}

bool CombatRuntime::check_player_death() {
    auto& actors = actors_->actor_states();
    const double health = actors.get(k_player_ref, av::k_health);
    if (player_dead_) {
        // The dead do not regenerate: only a load or a restore brings him back.
        if (health > 0.0) {
            actors.damage(k_player_ref, av::k_health, health);
        }
        return false;
    }
    if (health <= 0.0 && actors.is_actor(k_player_ref)) {
        player_dead_ = true;
        char hex[16];
        std::snprintf(hex, sizeof hex, "%X", last_damager_);
        host_->log(std::string("death: player killed by 0x") + hex);
        return true;
    }
    return false;
}

void CombatRuntime::bind_natives(Vm& vm, ActorHost& actors, Host& host) {
    using namespace natives;
    vm_ = &vm;
    actors_ = &actors;
    host_ = &host;
    // The state lives in SkydotAi (hooks); without it nobody fights.
    const auto self_state = [this](NativeCall& c) -> std::int64_t {
        return hooks_.state ? hooks_.state(c.self.form()) : 0;
    };
    vm.bind("Actor", "IsInCombat", [self_state](NativeCall& c) {
        return flag(self_state(c) != 0);
    });
    vm.bind("Actor", "GetCombatState", [self_state](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(self_state(c))));
    });
    vm.bind("Actor", "GetCombatTarget", [this](NativeCall& c) {
        const std::uint32_t target = hooks_.target ? hooks_.target(c.self.form()) : 0;
        if (target == 0) {
            return NativeResult{};
        }
        return result(c.vm.object(target, "Actor"));
    });
    vm.bind("Actor", "StartCombat", [this](NativeCall& c) {
        if (hooks_.start && is_object(c, 0)) {
            hooks_.start(c.self.form(), object_form(c, 0));
        }
        return NativeResult{};
    });
    // HasLOS(ObjectReference): a ray from the actor's eyes to the target (SkydotAi::has_los). Without
    // the AI nobody sees anything.
    vm.bind("Actor", "HasLOS", [this](NativeCall& c) {
        const bool seen = hooks_.has_los && is_object(c, 0) &&
                          hooks_.has_los(c.self.form(), object_form(c, 0));
        return flag(seen);
    });
    // Flight: the node's mover answers (SkydotAi hooks); a grounded or unbuilt actor does not fly.
    vm.bind("Actor", "IsFlying", [this](NativeCall& c) {
        return flag(hooks_.is_flying && hooks_.is_flying(c.self.form()));
    });
    vm.bind("Actor", "GetFlyingState", [this](NativeCall& c) {
        const std::int64_t state = hooks_.flying_state ? hooks_.flying_state(c.self.form()) : 0;
        return result(Value::integer(static_cast<std::int32_t>(state)));
    });
    vm.bind("Actor", "CanFlyHere", [this](NativeCall& c) {
        return flag(hooks_.can_fly_here && hooks_.can_fly_here(c.self.form()));
    });
    vm.bind("ObjectReference", "GetLength", [this](NativeCall& c) {
        const double length = hooks_.length ? hooks_.length(c.self.form()) : 0.0;
        return result(Value::floating(static_cast<float>(length)));
    });
    vm.bind("Actor", "StopCombat", [this](NativeCall& c) {
        if (hooks_.stop) {
            hooks_.stop(c.self.form());
        }
        return NativeResult{};
    });
}

} // namespace skydot::vm
