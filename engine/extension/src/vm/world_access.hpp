// SPDX-License-Identifier: GPL-3.0-or-later
//
// `WorldAccess`: what natives ask of the world, as typed questions. A native
// written against it needs neither Godot nor a pack, so a unit test can
// stand in a world of a few references. `SkydotPapyrus` answers it from the
// world's records and its own view of the world (vm/papyrus.cpp); the natives
// bound before it read Dictionaries instead and stay as they are.
#pragma once

#include <cstdint>

namespace skydot::vm {

class WorldAccess {
public:
    virtual ~WorldAccess() = default;

    /// The base form of reference `ref`; 0 if it has none or is unknown.
    [[nodiscard]] virtual std::uint32_t base_form(std::uint32_t ref) const = 0;
    /// What `ref` is linked to with `keyword` (0: the link without a keyword);
    /// 0 if there is no such link.
    [[nodiscard]] virtual std::uint32_t linked_ref(std::uint32_t ref, std::uint32_t keyword) const = 0;
    /// The cell `ref` is placed in; 0 if unknown.
    [[nodiscard]] virtual std::uint32_t parent_cell(std::uint32_t ref) const = 0;
    /// Whether `cell` is an interior.
    [[nodiscard]] virtual bool cell_interior(std::uint32_t cell) const = 0;
    /// Whether `cell` is loaded (its references were handed to the VM).
    [[nodiscard]] virtual bool cell_attached(std::uint32_t cell) const = 0;
    /// The player's level; 1 until the player has stats.
    [[nodiscard]] virtual std::int32_t player_level() const = 0;
};

} // namespace skydot::vm
