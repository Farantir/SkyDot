// SPDX-License-Identifier: GPL-3.0-or-later
//
// Natives written against `WorldAccess` (vm/world_access.hpp).
#pragma once

#include "vm/vm.hpp"
#include "vm/world_access.hpp"

#include <string>
#include <vector>

namespace skydot::vm {

/// Bind the natives that only ask the world: ObjectReference.GetNthLinkedRef,
/// CalculateEncounterLevel and GetParentCell, Cell.IsInterior and IsAttached,
/// Keyword.GetKeyword, Form.HasKeyword and the FormList natives (what scripts
/// add is in `Vm::form_lists`). `world` must outlive the VM.
void bind_world_natives(Vm& vm, const WorldAccess& world);

/// A message's text with Skyrim's placeholders filled from `args`: `%.0f`,
/// `%d` and the like take the next argument (0 when there is none), `%%` is a
/// percent sign. A '%' that starts no conversion stays.
[[nodiscard]] std::string format_message(const std::string& text, const std::vector<double>& args);

/// The forms of a list: the world's, then what scripts added.
[[nodiscard]] std::vector<std::uint32_t> form_list_contents(const Vm& vm, const WorldAccess& world,
                                                            std::uint32_t list);

/// The reference `n` steps along the chain of keyword-less links from `ref`:
/// 1 is its linked ref, 2 that one's linked ref, and 0 is `ref` itself. 0 if
/// the chain ends first or `n` is negative.
[[nodiscard]] std::uint32_t nth_linked_ref(const WorldAccess& world, std::uint32_t ref, std::int32_t n);

} // namespace skydot::vm
