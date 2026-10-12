// SPDX-License-Identifier: GPL-3.0-or-later
//
// Natives written against `WorldAccess` (vm/world_access.hpp).
#pragma once

#include "vm/vm.hpp"
#include "vm/world_access.hpp"

namespace skydot::vm {

/// Bind ObjectReference.GetNthLinkedRef and CalculateEncounterLevel. `world`
/// must outlive the VM.
void bind_world_natives(Vm& vm, const WorldAccess& world);

/// The reference `n` steps along the chain of keyword-less links from `ref`:
/// 1 is its linked ref, 2 that one's linked ref, and 0 is `ref` itself. 0 if
/// the chain ends first or `n` is negative.
[[nodiscard]] std::uint32_t nth_linked_ref(const WorldAccess& world, std::uint32_t ref, std::int32_t n);

} // namespace skydot::vm
