// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's worn enchantments, poisons and enchanted weapons: what wearing and hitting do
// to the magic of the wearer and the weapon (rules in magic/enchanted.hpp).
#include "vm/papyrus_magic_state.hpp"

namespace skydot {

// ---- worn enchantments, poisons, enchanted weapons ---------------------------------------------------

void SkydotPapyrus::sync_worn_magic(std::uint32_t owner) {
    if (!magic_) {
        return; // priming derives them when magic first runs
    }
    static_cast<MagicState*>(magic_.get())->sync_worn(owner);
    flush_magic_events();
}

void SkydotPapyrus::move_unit_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to) {
    if (from == to || owner == 0) {
        return;
    }
    auto& inventory = vm_->inventory();
    if (inventory.remove_extra(owner, form, from, 1) > 0) {
        inventory.add(owner, form, 1, to);
    }
    retarget_equipment(owner, form, from, to);
}

void SkydotPapyrus::retarget_equipment(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to) {
    if (from == to || owner == 0 || item_.states().find(owner) == nullptr) {
        return;
    }
    auto& equipment = item_.states().equipment(owner);
    if (equipment.right == form && equipment.right_extra == from) {
        equipment.right_extra = to;
    } else if (equipment.left == form && equipment.left_extra == from) {
        equipment.left_extra = to;
    } else if (equipment.ammo == form && equipment.ammo_extra == from) {
        equipment.ammo_extra = to;
    } else if (equipment.worn.contains(form) && equipment.worn_extra_of(form) == from) {
        if (to == 0) {
            equipment.worn_extra.erase(form);
        } else {
            equipment.worn_extra[form] = to;
        }
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
    m->prime(attacker);
    m->prime(target);
    magic::HitMagicInput in;
    in.attacker = attacker;
    in.target = target;
    in.weapon = weapon;
    in.extra = extra;
    in.attacker_is_player = attacker == k_player_ref;
    in.target_alive = target_alive;
    in.extra_unique = extra != 0 && vm_->inventory().units_with_extra(extra) == 1 && !item_.states().extra_dropped(extra);
    const auto poison_inputs = [m](std::uint32_t poison, std::vector<magic::EffectInput>& inputs) {
        magic::SourceKind kind = magic::SourceKind::potion;
        bool is_poison = false;
        return m->item_effects(poison, inputs, kind, is_poison);
    };
    const auto result = magic::hit_magic(m->world_, m->effects, item_.states().extras(), in, poison_inputs, m->settings(), &created_);
    if (result.new_extra != extra) {
        move_unit_extra(attacker, weapon, extra, result.new_extra);
    }
    flush_magic_events();
    out["poison"] = static_cast<std::int64_t>(result.poison);
    out["enchantment"] = static_cast<std::int64_t>(result.enchantment);
    out["fired"] = result.fired;
    out["charge"] = static_cast<double>(result.charge);
    out["charge_max"] = static_cast<double>(result.max);
    if (result.enchantment != 0 && attacker == k_player_ref) {
        const std::string line = "magic: " + hex(weapon) + " enchantment " + hex(result.enchantment) +
                                 (result.fired ? " fires" : " has no charge") + " (charge " +
                                 number(static_cast<double>(result.charge)) + "/" +
                                 number(static_cast<double>(result.max)) + ")";
        emit_signal("trace", to_godot(line));
        godot::UtilityFunctions::print(to_godot(line));
    }
    return out;
}

Dictionary SkydotPapyrus::poison_weapon(std::uint32_t actor, std::uint32_t form, std::int64_t extra) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    out["weapon"] = 0;
    out["doses"] = 0;
    auto* m = magic_state();
    if (m == nullptr || !actors_.is_actor(actor)) {
        out["reason"] = "no actor";
        return out;
    }
    std::vector<magic::EffectInput> inputs;
    magic::SourceKind kind = magic::SourceKind::potion;
    bool poison = false;
    if (!m->item_effects(form, inputs, kind, poison) || !poison) {
        out["reason"] = "not a poison";
        return out;
    }
    item_.resolve_owner(actor);
    const std::uint32_t poison_extra = item_.resolve_extra(actor, form, extra);
    if (vm_->inventory().count_extra(actor, form, poison_extra) <= 0) {
        out["reason"] = "not carried";
        return out;
    }
    const vm::Equipment* equipment = item_.states().find(actor);
    const std::uint32_t weapon = equipment != nullptr ? equipment->right : 0;
    const vm::ItemInfo info = item_.item_info_of(weapon);
    // UESP: a poison goes on the weapon in the right hand, a bow included; a staff or fists have no blade.
    if (weapon == 0 || !info.known || info.category != vm::ItemCategory::weapon || info.weapon_type == 0 ||
        info.weapon_type == 8) {
        out["reason"] = "no weapon";
        return out;
    }
    const std::uint32_t weapon_extra = equipment->right_extra;
    vm::ItemExtra changed;
    if (const auto* now = item_.states().extras().find(weapon_extra)) {
        changed = *now;
    }
    if (!magic::can_poison(changed)) {
        out["reason"] = "already poisoned";
        return out;
    }
    (void)item_.remove_item_extra(actor, form, poison_extra, 1);
    const std::uint32_t after = item_.states().extras().intern(magic::poisoned(changed, form, magic::k_placeholder_poison_doses));
    move_unit_extra(actor, weapon, weapon_extra, after);
    const std::string line = "magic: poison " + hex(form) + " put on " + hex(weapon) + " of " + hex(actor);
    emit_signal("trace", to_godot(line));
    godot::UtilityFunctions::print(to_godot(line));
    out["ok"] = true;
    out["weapon"] = static_cast<std::int64_t>(weapon);
    out["doses"] = static_cast<std::int64_t>(magic::k_placeholder_poison_doses);
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
    const auto actor = static_cast<std::uint32_t>(actor_in);
    const auto form = static_cast<std::uint32_t>(form_in);
    item_.resolve_owner(actor);
    std::uint32_t unit = 0;
    const vm::Equipment* equipment = item_.states().find(actor);
    if (extra >= 0) {
        unit = static_cast<std::uint32_t>(extra);
    } else if (equipment != nullptr && equipment->right == form) {
        unit = equipment->right_extra;
    } else {
        unit = item_.resolve_extra(actor, form, -1);
    }
    const auto info = magic::weapon_charge(m->world_, form, item_.states().extras().find(unit), m->settings(), &created_);
    out["enchantment"] = static_cast<std::int64_t>(info.enchantment);
    out["charge"] = static_cast<double>(info.charge);
    out["max"] = static_cast<double>(info.max);
    out["cost"] = info.cost;
    return out;
}


} // namespace skydot
