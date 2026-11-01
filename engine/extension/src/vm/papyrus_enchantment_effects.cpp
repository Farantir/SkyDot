// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's worn enchantments, poisons and enchanted weapons: what wearing and hitting do
// to the magic of the wearer and the weapon (rules in magic/enchanted.hpp).
#include "vm/papyrus_magic_state.hpp"

namespace skydot {

// ---- worn enchantments, poisons, enchanted weapons ---------------------------------------------------

void SkydotPapyrus::sync_worn_magic(std::uint32_t owner) {
    if (magic_) { // priming derives them when magic first runs
        magic_->sync_worn_magic(owner);
    }
}

void SkydotPapyrus::move_unit_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to) {
    if (auto* m = magic_state()) {
        m->move_unit_extra(owner, form, from, to);
    }
}

void SkydotPapyrus::retarget_equipment(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to) {
    if (auto* m = magic_state()) {
        m->retarget_equipment(owner, form, from, to);
    }
}

Dictionary SkydotPapyrus::hit_magic(std::uint32_t attacker, std::uint32_t target, std::uint32_t weapon,
                                    std::uint32_t extra, bool target_alive) {
    Dictionary out;
    out["poison"] = 0;
    out["enchantment"] = 0;
    out["fired"] = false;
    out["charge"] = 0.0;
    out["charge_max"] = 0.0;
    auto* m = magic_state();
    if (m == nullptr || weapon == 0) {
        return out;
    }
    const auto result = m->hit_magic(attacker, target, weapon, extra, target_alive);
    out["poison"] = static_cast<std::int64_t>(result.poison);
    out["enchantment"] = static_cast<std::int64_t>(result.enchantment);
    out["fired"] = result.fired;
    out["charge"] = result.charge;
    out["charge_max"] = result.charge_max;
    return out;
}

Dictionary SkydotPapyrus::poison_weapon(std::uint32_t actor, std::uint32_t form, std::int64_t extra) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    out["weapon"] = 0;
    out["doses"] = 0;
    auto* m = magic_state();
    if (m == nullptr) {
        out["reason"] = "no actor";
        return out;
    }
    const auto result = m->poison_weapon(actor, form, extra);
    if (!result.ok) {
        out["reason"] = result.reason.c_str();
        return out;
    }
    out["ok"] = true;
    out["weapon"] = static_cast<std::int64_t>(result.weapon);
    out["doses"] = static_cast<std::int64_t>(result.doses);
    return out;
}

Dictionary SkydotPapyrus::apply_poison(std::int64_t actor, std::int64_t poison) {
    return poison_weapon(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(poison), -1);
}

Dictionary SkydotPapyrus::weapon_charge(std::int64_t actor_in, std::int64_t form_in, std::int64_t extra) {
    Dictionary out;
    out["enchantment"] = 0;
    out["charge"] = 0.0;
    out["max"] = 0.0;
    out["cost"] = 0.0;
    auto* m = magic_state();
    if (m == nullptr) {
        return out;
    }
    const auto info = m->weapon_charge(static_cast<std::uint32_t>(actor_in), static_cast<std::uint32_t>(form_in), extra);
    out["enchantment"] = static_cast<std::int64_t>(info.enchantment);
    out["charge"] = info.charge;
    out["max"] = info.max;
    out["cost"] = info.cost;
    return out;
}

} // namespace skydot
