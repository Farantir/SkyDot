// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the game did to the items of references, beside the counts the
// Inventory holds: which owners had their start inventory resolved, the actor
// a placed leveled NPC list picked, and what each owner holds and wears.
// Section ITST of a save (optional: an old save has all owners unresolved, and
// they resolve on touch). Nothing here touches Godot.
//
//   ITST  u32 resolved owners, each u32 ref; u32 picks, each u32 ref and u32
//         NPC_; u32 owners with equipment, each u32 ref, u32 right, u32 left,
//         u32 ammo, u32 worn count and u32 forms
#pragma once

#include "vm/equipment.hpp"
#include "vm/inventory.hpp"
#include "save/bytes.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace skydot::vm {

class ItemStates {
public:
    static constexpr std::uint32_t k_section_version = 1;

    [[nodiscard]] bool resolved(std::uint32_t owner) const { return resolved_.contains(owner); }
    void mark_resolved(std::uint32_t owner) { resolved_.insert(owner); }
    [[nodiscard]] const std::set<std::uint32_t>& resolved_owners() const { return resolved_; }

    /// Every owner that holds something counts as resolved: for a save that
    /// has no ITST, whose inventories already include their start items.
    void mark_holders_resolved(const Inventory& inventory) {
        for (const auto& [owner, items] : inventory.owners()) {
            resolved_.insert(owner);
        }
    }

    /// The NPC_ a placed leveled NPC list picked for `ref`; 0 if none was.
    [[nodiscard]] std::uint32_t pick_of(std::uint32_t ref) const;
    void set_pick(std::uint32_t ref, std::uint32_t npc) { picks_[ref] = npc; }
    [[nodiscard]] const std::map<std::uint32_t, std::uint32_t>& picks() const { return picks_; }

    /// What `owner` holds and wears (empty if nothing).
    [[nodiscard]] const Equipment* find(std::uint32_t owner) const;
    Equipment& equipment(std::uint32_t owner) { return equipment_[owner]; }
    [[nodiscard]] const std::map<std::uint32_t, Equipment>& equipments() const { return equipment_; }
    /// Forget `owner`'s equipment, keeping it resolved.
    void clear_equipment(std::uint32_t owner) { equipment_.erase(owner); }
    /// Forget everything about `owner`.
    void erase(std::uint32_t owner);

    void clear();
    void adopt(ItemStates&& other) { *this = std::move(other); }

    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] static bool load(save::ByteReader r, ItemStates& out);

private:
    std::set<std::uint32_t> resolved_;
    std::map<std::uint32_t, std::uint32_t> picks_;
    std::map<std::uint32_t, Equipment> equipment_;
};

} // namespace skydot::vm
