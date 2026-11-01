// SPDX-License-Identifier: GPL-3.0-or-later
//
// The natives families bound as free functions over narrow interfaces
// (ARCHITECTURE.md, "Natives and the world"). Each is in vm/natives_<family>.cpp.
#pragma once

#include "vm/vm.hpp"
#include "vm/world_access.hpp"
#include "vm/world_events.hpp"

namespace skydot::vm {

/// Natives that are meant to do nothing: silent, or logged as not shown.
void bind_noop_natives(Vm& vm);

/// Utility.Wait, GetCurrentRealTime, RandomInt and RandomFloat.
void bind_utility_natives(Vm& vm);

/// Debug.Trace, Notification, MessageBox and Message.Show. `world` and
/// `events` must outlive the VM.
void bind_debug_natives(Vm& vm, const WorldAccess& world, WorldEvents& events);

/// EffectShader.Play and Stop.
void bind_effectshader_natives(Vm& vm, WorldEvents& events);

} // namespace skydot::vm
