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
//         u32 ammo, u32 worn count and u32 forms; version 3 adds after the
//         worn forms u32 right, left and ammo extras, u32 count and per entry
//         u32 worn form and u32 extra
//         version 2: u32 dropped references, each u32 ref and u64 count (3:
//         and u32 extra)
//         version 4: the extras table is in layout 2 (item_extras.hpp: charge, poison)
//         version 3 ends with the extras table (item_extras.hpp) and the
//         splits: u32 owners, each u32 owner, u32 items, each u32 form, u32
//         entries, each u32 extra and u64 count
#pragma once

#include "vm/equipment.hpp"
#include "vm/inventory.hpp"
#include "vm/item_extras.hpp"
#include "save/bytes.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace skydot::vm {

class ItemStates {
public:
    static constexpr std::uint32_t k_section_version = 4;

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

    /// How many items a reference that was dropped stands for (not in the
    /// world's data, so the VM keeps it); 0 if it is not one.
    [[nodiscard]] std::int64_t dropped_count(std::uint32_t ref) const {
        const auto it = dropped_.find(ref);
        return it == dropped_.end() ? 0 : it->second;
    }
    /// The extra id of the units a dropped reference stands for (0: plain).
    [[nodiscard]] std::uint32_t dropped_extra(std::uint32_t ref) const {
        const auto it = dropped_extra_.find(ref);
        return it == dropped_extra_.end() ? 0 : it->second;
    }
    /// Whether a dropped reference carries `extra`.
    [[nodiscard]] bool extra_dropped(std::uint32_t extra) const {
        for (const auto& [ref, id] : dropped_extra_) {
            if (id == extra) {
                return true;
            }
        }
        return false;
    }
    void set_dropped(std::uint32_t ref, std::int64_t count, std::uint32_t extra = 0) {
        dropped_[ref] = count;
        if (extra != 0) {
            dropped_extra_[ref] = extra;
        } else {
            dropped_extra_.erase(ref);
        }
    }
    void forget_dropped(std::uint32_t ref) {
        dropped_.erase(ref);
        dropped_extra_.erase(ref);
    }

    /// The extras the session interned; ids in inventories, equipment and
    /// dropped references refer to it.
    ItemExtras& extras() { return extras_; }
    [[nodiscard]] const ItemExtras& extras() const { return extras_; }
    /// The splits a load read (the inventory is the VM's, which takes them
    /// from here); empty otherwise.
    Inventory::Splits take_splits() { return std::move(loaded_splits_); }

    void clear();
    void adopt(ItemStates&& other) { *this = std::move(other); }

    /// `inventory` supplies the splits.
    [[nodiscard]] std::vector<std::uint8_t> save(const Inventory& inventory) const;
    /// Without splits (every unit plain).
    [[nodiscard]] std::vector<std::uint8_t> save() const { return save(Inventory{}); }
    /// `version` is the section's (1: no dropped references).
    [[nodiscard]] static bool load(save::ByteReader r, ItemStates& out, std::uint32_t version = k_section_version);

private:
    std::set<std::uint32_t> resolved_;
    std::map<std::uint32_t, std::uint32_t> picks_;
    std::map<std::uint32_t, Equipment> equipment_;
    std::map<std::uint32_t, std::int64_t> dropped_;
    std::map<std::uint32_t, std::uint32_t> dropped_extra_;
    ItemExtras extras_;
    Inventory::Splits loaded_splits_;
};

} // namespace skydot::vm
