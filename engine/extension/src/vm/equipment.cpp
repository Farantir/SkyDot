// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/equipment.hpp"

#include <algorithm>

namespace skydot::vm {

const char* category_name(ItemCategory category) noexcept {
    switch (category) {
    case ItemCategory::misc: return "misc";
    case ItemCategory::weapon: return "weapon";
    case ItemCategory::armor: return "armor";
    case ItemCategory::clothing: return "clothing";
    case ItemCategory::jewelry: return "jewelry";
    case ItemCategory::potion: return "potion";
    case ItemCategory::food: return "food";
    case ItemCategory::ingredient: return "ingredient";
    case ItemCategory::book: return "book";
    case ItemCategory::key: return "key";
    case ItemCategory::soul_gem: return "soul_gem";
    case ItemCategory::scroll: return "scroll";
    case ItemCategory::ammo: return "ammo";
    case ItemCategory::light: return "light";
    }
    return "misc";
}

bool is_shield(const ItemInfo& item) noexcept {
    // ETYP Shield, or body slot 39.
    return item.category == ItemCategory::armor &&
           (item.equip_slot == etyp::shield || (item.biped_slots & (1U << 9)) != 0);
}

bool is_two_handed(const ItemInfo& item) noexcept {
    if (item.category != ItemCategory::weapon) {
        return false;
    }
    if (item.equip_slot != 0) {
        return item.equip_slot == etyp::both_hands;
    }
    // No ETYP: what the animation type implies.
    return item.weapon_type == 5 || item.weapon_type == 6 || item.weapon_type == 7 || item.weapon_type == 9;
}

bool Equipment::holds(std::uint32_t form) const noexcept {
    return form != 0 && (right == form || left == form || ammo == form || worn.contains(form));
}

std::uint32_t Equipment::worn_extra_of(std::uint32_t form) const noexcept {
    const auto it = worn_extra.find(form);
    return it == worn_extra.end() ? 0 : it->second;
}

bool Equipment::holds(std::uint32_t form, std::uint32_t extra) const noexcept {
    return form != 0 && ((right == form && right_extra == extra) || (left == form && left_extra == extra) ||
                         (ammo == form && ammo_extra == extra) ||
                         (worn.contains(form) && worn_extra_of(form) == extra));
}

std::uint32_t Equipment::extra_of(std::uint32_t form) const noexcept {
    if (form == 0) {
        return 0;
    }
    if (right == form) {
        return right_extra;
    }
    if (left == form) {
        return left_extra;
    }
    if (ammo == form) {
        return ammo_extra;
    }
    return worn_extra_of(form);
}

namespace {

/// Whether the item in the right hand is a two-hander, by the lookup.
bool right_is_two_handed(const Equipment& equipment, const ItemLookup& lookup) {
    return equipment.right != 0 && is_two_handed(lookup(equipment.right));
}

void displace(EquipResult& result, std::uint32_t& slot, std::uint32_t& slot_extra) {
    if (slot != 0) {
        result.displaced.push_back(slot);
        slot = 0;
        slot_extra = 0;
    }
}

} // namespace

EquipResult equip(Equipment& equipment, const ItemInfo& item, bool left, const ItemLookup& lookup,
                  std::uint32_t extra) {
    EquipResult result;
    switch (item.category) {
    case ItemCategory::potion:
    case ItemCategory::food:
    case ItemCategory::ingredient:
    case ItemCategory::scroll:
        result.action = EquipAction::consumed;
        return result;
    case ItemCategory::book:
        result.action = EquipAction::read;
        return result;
    case ItemCategory::misc:
    case ItemCategory::key:
    case ItemCategory::soul_gem:
        return result;
    case ItemCategory::ammo:
        if (equipment.ammo != item.form) {
            displace(result, equipment.ammo, equipment.ammo_extra);
        }
        equipment.ammo = item.form;
        equipment.ammo_extra = extra;
        result.action = EquipAction::equipped;
        return result;
    case ItemCategory::light:
        // A torch is held in the left hand.
        if (right_is_two_handed(equipment, lookup)) {
            displace(result, equipment.right, equipment.right_extra);
        }
        if (equipment.left != item.form) {
            displace(result, equipment.left, equipment.left_extra);
        }
        equipment.left = item.form;
        equipment.left_extra = extra;
        result.action = EquipAction::equipped;
        return result;
    case ItemCategory::weapon: {
        if (is_two_handed(item)) {
            if (equipment.right != item.form) {
                displace(result, equipment.right, equipment.right_extra);
            }
            displace(result, equipment.left, equipment.left_extra);
            equipment.right = item.form;
            equipment.right_extra = extra;
            result.action = EquipAction::equipped;
            return result;
        }
        // Any one-handed weapon can be asked into the left hand, as the game's
        // menu does; one made for the left goes there by itself.
        const bool to_left = item.equip_slot == etyp::left_hand || left;
        if (to_left) {
            if (right_is_two_handed(equipment, lookup)) {
                displace(result, equipment.right, equipment.right_extra);
            }
            if (equipment.left != item.form) {
                displace(result, equipment.left, equipment.left_extra);
            }
            equipment.left = item.form;
        equipment.left_extra = extra;
        } else {
            if (equipment.right != item.form) {
                displace(result, equipment.right, equipment.right_extra);
            }
            equipment.right = item.form;
            equipment.right_extra = extra;
        }
        result.action = EquipAction::equipped;
        return result;
    }
    case ItemCategory::armor:
    case ItemCategory::clothing:
    case ItemCategory::jewelry:
        break;
    }
    if (is_shield(item)) {
        if (right_is_two_handed(equipment, lookup)) {
            displace(result, equipment.right, equipment.right_extra);
        }
        if (equipment.left != item.form) {
            displace(result, equipment.left, equipment.left_extra);
        }
        equipment.left = item.form;
        equipment.left_extra = extra;
        result.action = EquipAction::equipped;
        return result;
    }
    // Worn: whatever overlaps its slots comes off.
    for (auto it = equipment.worn.begin(); it != equipment.worn.end();) {
        if (*it != item.form && (lookup(*it).biped_slots & item.biped_slots) != 0) {
            result.displaced.push_back(*it);
            equipment.worn_extra.erase(*it);
            it = equipment.worn.erase(it);
        } else {
            ++it;
        }
    }
    equipment.worn.insert(item.form);
    if (extra != 0) {
        equipment.worn_extra[item.form] = extra;
    } else {
        equipment.worn_extra.erase(item.form);
    }
    result.action = EquipAction::equipped;
    return result;
}

bool unequip(Equipment& equipment, std::uint32_t form, std::optional<std::uint32_t> extra) noexcept {
    if (form == 0) {
        return false;
    }
    bool was = false;
    const auto clear = [&](std::uint32_t& slot, std::uint32_t& slot_extra) {
        if (slot == form && (!extra || slot_extra == *extra)) {
            slot = 0;
            slot_extra = 0;
            was = true;
        }
    };
    clear(equipment.right, equipment.right_extra);
    clear(equipment.left, equipment.left_extra);
    clear(equipment.ammo, equipment.ammo_extra);
    if (equipment.worn.contains(form) && (!extra || equipment.extra_of(form) == *extra)) {
        equipment.worn.erase(form);
        equipment.worn_extra.erase(form);
        was = true;
    }
    return was;
}

int equipped_item_type(const ItemInfo& item) noexcept {
    switch (item.category) {
    case ItemCategory::weapon:
        // Animation types 1..8 are the script values; the crossbow is 12.
        if (item.weapon_type == 9) {
            return 12;
        }
        return item.weapon_type <= 8 ? item.weapon_type : 0;
    case ItemCategory::armor:
        return is_shield(item) ? 10 : 0;
    case ItemCategory::light:
        return 11;
    default:
        return 0;
    }
}

} // namespace skydot::vm
