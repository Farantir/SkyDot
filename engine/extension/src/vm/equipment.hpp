// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an item is, and what an owner holds and wears: the equip rules of the
// game for the VM, without Godot or a pack (the data comes in as `ItemInfo`).
//
// An owner has a right hand, a left hand, an ammo slot and a set of worn
// armor. Equipping one item:
// - ARMO, clothing and jewelry replace the worn pieces whose body slots
//   overlap (bit n of `biped_slots` is slot 30 + n).
// - A shield or a torch goes in the left hand and clears a two-hander.
// - A two-hander (ETYP BothHands; bows, most staves) takes both hands.
// - Other weapons go right unless asked for the left hand (or made for it).
//   A weapon in the left hand clears a two-hander in the right.
// - Ammo has its slot.
// - Potions, food, ingredients and scrolls are consumed instead (one unit, the
//   caller removes it), books are read; neither does anything yet.
// - Keys, soul gems and misc items cannot be equipped.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace skydot::vm {

/// `ItemData.category` of world.fbs, same values.
enum class ItemCategory : std::uint8_t {
    misc,
    weapon,
    armor,
    clothing,
    jewelry,
    potion,
    food,
    ingredient,
    book,
    key,
    soul_gem,
    scroll,
    ammo,
    light,
};

[[nodiscard]] const char* category_name(ItemCategory category) noexcept;

/// EQUP ids of Skyrim.esm. One-handed swords, daggers, axes and maces carry
/// right_hand, two-handers both_hands, shields shield (read from the data).
namespace etyp {
inline constexpr std::uint32_t right_hand = 0x00013F42;
inline constexpr std::uint32_t left_hand = 0x00013F43;
inline constexpr std::uint32_t either_hand = 0x00013F44;
inline constexpr std::uint32_t both_hands = 0x00013F45;
inline constexpr std::uint32_t shield = 0x000141E8;
} // namespace etyp

/// What the data says about one item base.
struct ItemInfo {
    /// False for a form that is no item (or a pack without item data).
    bool known = false;
    std::uint32_t form = 0;
    std::string name;
    ItemCategory category = ItemCategory::misc;
    float weight = 0.0F;
    std::int32_t value = 0;
    bool playable = true;
    std::uint32_t equip_slot = 0;
    /// WEAP animation type: 0 hand to hand, 1 sword, 2 dagger, 3 axe, 4 mace,
    /// 5 two-handed sword, 6 two-handed axe, 7 bow, 8 staff, 9 crossbow.
    std::uint8_t weapon_type = 0;
    float damage = 0.0F;
    float armor_rating = 0.0F;
    std::uint8_t armor_type = 0;
    std::uint32_t biped_slots = 0;
    bool food = false;
};

[[nodiscard]] bool is_shield(const ItemInfo& item) noexcept;
/// A weapon that takes both hands.
[[nodiscard]] bool is_two_handed(const ItemInfo& item) noexcept;

/// What one owner holds in hand and wears.
struct Equipment {
    std::uint32_t right = 0;
    std::uint32_t left = 0;
    std::uint32_t ammo = 0;
    std::set<std::uint32_t> worn;
    /// The extra data (item_extras.hpp) of the unit in each slot, 0 if plain.
    std::uint32_t right_extra = 0;
    std::uint32_t left_extra = 0;
    std::uint32_t ammo_extra = 0;
    /// Worn form -> extra; only the worn pieces that carry one.
    std::map<std::uint32_t, std::uint32_t> worn_extra;

    [[nodiscard]] bool empty() const noexcept { return right == 0 && left == 0 && ammo == 0 && worn.empty(); }
    [[nodiscard]] bool holds(std::uint32_t form) const noexcept;
    /// Whether the unit of `form` carrying `extra` is held or worn.
    [[nodiscard]] bool holds(std::uint32_t form, std::uint32_t extra) const noexcept;
    /// The extra of the held or worn unit of `form` (0 if plain or not held).
    [[nodiscard]] std::uint32_t extra_of(std::uint32_t form) const noexcept;
    [[nodiscard]] std::uint32_t worn_extra_of(std::uint32_t form) const noexcept;
};

/// The item data of a form (a function so a test can stand in the pack).
using ItemLookup = std::function<ItemInfo(std::uint32_t form)>;

enum class EquipAction : std::uint8_t {
    refused,  ///< The item cannot be equipped.
    equipped, ///< It is in a hand, the ammo slot or worn now.
    consumed, ///< Potion, food, ingredient or scroll: the caller removes one.
    read,     ///< A book.
};

struct EquipResult {
    EquipAction action = EquipAction::refused;
    /// Items taken off to make room.
    std::vector<std::uint32_t> displaced;
};

/// Equip `item` (the unit carrying `extra`) on `equipment`; `left` asks for
/// the left hand where either will do. Only the state changes: whether the
/// owner has the item is for the caller.
EquipResult equip(Equipment& equipment, const ItemInfo& item, bool left, const ItemLookup& lookup,
                  std::uint32_t extra = 0);

/// The gear an NPC fights with, chosen among what it carries (0: none).
struct GearChoice {
    std::uint32_t melee = 0;  ///< The one-handed or two-handed melee weapon of the most damage.
    std::uint32_t shield = 0; ///< The shield of the best rating, only with a one-handed `melee`.
    std::uint32_t bow = 0;    ///< The bow of the most damage.
    std::uint32_t ammo = 0;   ///< The ammo of the most damage.
};

/// Best melee weapon (animation types 1 to 6), shield, bow (type 7) and ammo among `forms`.
/// Ties go to the first. A pure rule so a test needs no pack.
[[nodiscard]] GearChoice choose_gear(const std::vector<std::uint32_t>& forms, const ItemLookup& lookup);

/// Take `form` off wherever it is; with `extra`, only if the unit there
/// carries it. False if it was not held or worn.
bool unequip(Equipment& equipment, std::uint32_t form, std::optional<std::uint32_t> extra = std::nullopt) noexcept;

/// Actor.GetEquippedItemType for `item`: 0 nothing or fists, 1 sword, 2
/// dagger, 3 axe, 4 mace, 5 two-handed sword, 6 two-handed axe, 7 bow, 8
/// staff, 9 spell, 10 shield, 11 torch, 12 crossbow.
[[nodiscard]] int equipped_item_type(const ItemInfo& item) noexcept;

} // namespace skydot::vm
