// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Actor natives over `ActorStates` (vm/actor_states.hpp).
#pragma once

#include "vm/actor_states.hpp"
#include "vm/vm.hpp"

namespace skydot::vm {

/// Bind Actor.GetActorValue, GetBaseActorValue, SetActorValue,
/// ModActorValue, ForceActorValue, DamageActorValue, RestoreActorValue,
/// GetActorValuePercentage and IsDead. An unknown actor value name or a self
/// that is no actor is reported once and reads 0 (does nothing). `states`
/// must outlive the VM.
void bind_actor_natives(Vm& vm, ActorStates& states);

} // namespace skydot::vm
