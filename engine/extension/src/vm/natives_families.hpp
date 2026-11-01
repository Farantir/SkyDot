// SPDX-License-Identifier: GPL-3.0-or-later
//
// The natives families bound as free functions over narrow interfaces
// (ARCHITECTURE.md, "Natives and the world"). Each is in vm/natives_<family>.cpp.
#pragma once

#include "vm/vm.hpp"

namespace skydot::vm {

/// Natives that are meant to do nothing: silent, or logged as not shown.
void bind_noop_natives(Vm& vm);

/// Utility.Wait, GetCurrentRealTime, RandomInt and RandomFloat.
void bind_utility_natives(Vm& vm);

} // namespace skydot::vm
