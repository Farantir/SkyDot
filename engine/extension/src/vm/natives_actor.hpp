// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Actor natives over `ActorStates` (vm/actor_states.hpp).
#pragma once

#include "vm/actor_states.hpp"
#include "vm/vm.hpp"

#include <functional>

namespace skydot::vm {

/// Bind Actor.GetActorValue, GetBaseActorValue, SetActorValue,
/// ModActorValue, ForceActorValue, DamageActorValue, RestoreActorValue,
/// GetActorValuePercentage, IsDead and GetKiller. An unknown actor value name or a self
/// that is no actor is reported once and reads 0 (does nothing). `states`
/// must outlive the VM.
/// `on_mod` hears every ModActorValue a script makes (actor, actor value, amount).
void bind_actor_natives(Vm& vm, ActorStates& states,
                        std::function<void(std::uint32_t, std::uint32_t, double)> on_mod = {});

} // namespace skydot::vm
