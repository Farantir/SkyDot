// SPDX-License-Identifier: GPL-3.0-or-later
//
// `WorldAccess`: what natives ask of the world, as typed questions. A native
// written against it needs neither Godot nor a pack, so a unit test can
// stand in a world of a few references. `SkydotPapyrus` answers it from the
// world's records and its own view of the world (vm/papyrus.cpp); the natives
// bound before it read Dictionaries instead and stay as they are.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skydot::vm {

/// A MESG: `title` (FULL), `text` (DESC) with its %f placeholders, and whether
/// it is a message box rather than a notification, with its buttons' texts.
struct MessageText {
    std::string title;
    std::string text;
    bool box = false;
    std::vector<std::string> buttons;
};

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
    /// Whether `form` has `keyword`: a base form by its KWDA, a reference by
    /// its base's (references carry none of their own).
    [[nodiscard]] virtual bool has_keyword(std::uint32_t form, std::uint32_t keyword) const = 0;
    /// The KYWD with this editor id (any case); 0 if none.
    [[nodiscard]] virtual std::uint32_t find_keyword(const std::string& editor_id) const = 0;
    /// The forms of FLST `list` into `forms`; false if `list` is not one.
    [[nodiscard]] virtual bool form_list(std::uint32_t list, std::vector<std::uint32_t>& forms) const = 0;
    /// MESG `id` into `out`; false if it is not one.
    [[nodiscard]] virtual bool message(std::uint32_t id, MessageText& out) const = 0;
    /// The player's level; 1 until the player has stats.
    [[nodiscard]] virtual std::int32_t player_level() const = 0;
};

} // namespace skydot::vm
