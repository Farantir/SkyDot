// SPDX-License-Identifier: GPL-3.0-or-later
//
// ItemAccess: what the other domains (activation, crafting, magic) need of
// items. `vm::ItemRuntime` implements it; a unit test stands in a few fields.
#pragma once

#include "state/equipment.hpp"

#include <cstdint>
#include <string>

namespace skydot::vm {

class ItemAccess {
public:
    virtual ~ItemAccess() = default;
    /// Give `owner` its start inventory and outfit, once.
    virtual void resolve_owner(std::uint32_t owner) const = 0;
    /// What the data (or alchemy) says of item `form`; `known` is false for anything else.
    [[nodiscard]] virtual ItemInfo item_info_of(std::uint32_t form) const = 0;
    /// A name for any item, also created forms and ones the pack lacks.
    [[nodiscard]] virtual std::string item_name_of(std::uint32_t form) const = 0;
    /// Unequip what is equipped of `form` once no unit of the equipped stack is left (after units left).
    virtual void refresh_equipment(std::uint32_t owner, std::uint32_t form) = 0;
    /// How many a dropped reference stands for; 0 if it is none.
    [[nodiscard]] virtual std::int64_t dropped_count(std::uint32_t ref) const = 0;
    /// The extra id of the units a dropped reference stands for (0: plain).
    [[nodiscard]] virtual std::uint32_t dropped_extra(std::uint32_t ref) const = 0;
};

} // namespace skydot::vm
