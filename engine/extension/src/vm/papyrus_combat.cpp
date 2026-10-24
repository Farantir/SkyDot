// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus.apply_hit (design: GAMEPLAY-PLAN.md, 6b): the inputs of one
// melee hit resolved from the world and the session, the formula of
// combat/damage.hpp, Health damaged through ActorStates, then OnHit and, on a
// death, OnDying and OnDeath.
#include "vm/papyrus.hpp"

#include "combat/damage.hpp"
#include "data/actors.hpp"
#include "data/fb_search.hpp"
#include "vm/natives_common.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>

using godot::Dictionary;
using godot::String;

namespace skydot {

namespace {

namespace wfb = bethconv::pack::wfb;

bool same_event(const flatbuffers::String* have, const std::string& wanted) {
    if (have == nullptr || have->size() != wanted.size()) {
        return false;
    }
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(have->c_str()[i])) !=
            std::tolower(static_cast<unsigned char>(wanted[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

Dictionary SkydotPapyrus::apply_hit(std::int64_t attacker_id, std::int64_t target_id, std::int64_t weapon_id,
                                    std::int64_t extra_id, const String& attack_event, std::int64_t flags,
                                    std::int64_t ammo_id) {
    Dictionary out;
    const auto attacker = static_cast<std::uint32_t>(attacker_id);
    const auto target = static_cast<std::uint32_t>(target_id);
    const auto weapon = static_cast<std::uint32_t>(weapon_id);
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (root == nullptr || !actors_.is_actor(target)) {
        out["error"] = "the target is no actor";
        return out;
    }
    if (actors_.is_dead(target)) {
        out["error"] = "the target is dead";
        return out;
    }
    const bool power = (flags & HIT_POWER) != 0;
    const bool sneak = (flags & HIT_SNEAK) != 0;
    const bool bash = (flags & HIT_BASH) != 0;
    const bool blocked = (flags & HIT_BLOCKED) != 0;

    const WorldData& data = world_->data();
    combat::CombatSettings settings;
    settings.damage_pc_skill_max = data.gmst_float("fDamagePCSkillMax", settings.damage_pc_skill_max);
    settings.armor_scaling_factor = data.gmst_float("fArmorScalingFactor", settings.armor_scaling_factor);
    settings.max_armor_rating = data.gmst_float("fMaxArmorRating", settings.max_armor_rating);
    settings.armor_base_factor = data.gmst_float("fArmorBaseFactor", settings.armor_base_factor);
    settings.armor_rating_base = data.gmst_float("fArmorRatingBase", settings.armor_rating_base);
    settings.armor_rating_pc_max = data.gmst_float("fArmorRatingPCMax", settings.armor_rating_pc_max);
    settings.armor_rating_max = data.gmst_float("fArmorRatingMax", settings.armor_rating_max);
    settings.clothing_armor_scale = data.gmst_float("fClothingArmorScale", settings.clothing_armor_scale);

    // The attacker's race: its unarmed damage and the attack of the event.
    const wfb::Race* race = nullptr;
    if (attacker != 0) {
        std::uint32_t npc = items_.pick_of(attacker);
        if (npc == 0) {
            npc = dialogue_npc(attacker);
        }
        if (npc != 0) {
            const auto* n = resolve_npc(*root, npc, wfb::NpcTemplateFlags::use_traits, attacker);
            race = n != nullptr ? lookup(root->races(), n->race()) : nullptr;
        }
    }
    std::string event = natives::utf8(attack_event);
    if (event.empty()) {
        event = power ? "attackPowerStartInPlace" : "attackStart";
    }
    combat::HitInputs in;
    in.attack_mult = 1.0F;
    if (race != nullptr && race->attacks() != nullptr) {
        for (const auto* a : *race->attacks()) {
            if (same_event(a->event(), event)) {
                in.attack_mult = a->damage_mult();
                break;
            }
        }
    }

    // The weapon (or the fists).
    in.attacker_is_player = attacker == k_player_ref;
    std::int32_t skill_av = -1;
    if (weapon != 0) {
        const auto* base = lookup(root->bases(), weapon);
        const auto* item = base != nullptr ? base->item() : nullptr;
        if (item != nullptr) {
            in.weapon_damage = item->damage();
            skill_av = item->weapon_skill();
        }
    } else if (race != nullptr) {
        in.weapon_damage = race->unarmed_damage();
    }
    if (ammo_id != 0) {
        in.ammo_damage = item_info_of(static_cast<std::uint32_t>(ammo_id)).damage;
    }
    if (skill_av >= 0) {
        in.weapon_skill = actors_.get(attacker, static_cast<std::uint32_t>(skill_av));
    }
    if (extra_id != 0) {
        if (const auto* extra = items_.extras().find(static_cast<std::uint32_t>(extra_id))) {
            in.temper_health = extra->health;
        }
    }

    // The target's worn armor and shield.
    resolve_owner(target);
    in.target_is_player = target == k_player_ref;
    in.light_armor_skill = actors_.get(target, vm::av::k_first_skill + 6);  // LightArmor 12
    in.heavy_armor_skill = actors_.get(target, vm::av::k_first_skill + 5);  // HeavyArmor 11
    if (const vm::Equipment* worn = items_.find(target)) {
        const auto add = [&](std::uint32_t form) {
            const vm::ItemInfo info = item_info_of(form);
            if (info.known && (info.category == vm::ItemCategory::armor || info.category == vm::ItemCategory::clothing)) {
                in.armor.push_back({info.armor_rating, info.armor_type});
            }
        };
        for (const std::uint32_t form : worn->worn) {
            add(form);
        }
        if (worn->left != 0 && vm::is_shield(item_info_of(worn->left))) {
            add(worn->left);
        }
    }
    const combat::HitResult r = combat::compute_hit(in, settings);

    const double before = actors_.get(target, vm::av::k_health);
    actors_.damage(target, vm::av::k_health, static_cast<double>(r.final_damage));
    const double after = actors_.get(target, vm::av::k_health);
    bool killed = actors_.is_dead(target);
    if (killed) {
        actors_.set_killer(target, attacker);
    }
    // The weapon's poison and enchantment (7a-3): effects on the target, charge spent for the player.
    const Dictionary hit_magic_out = hit_magic(attacker, target, weapon, static_cast<std::uint32_t>(extra_id), !killed);
    if (!killed && actors_.is_dead(target)) {
        killed = true; // the enchantment finished it
        actors_.set_killer(target, attacker);
    }

    char line[320];
    std::snprintf(line, sizeof line,
                  "combat: 0x%X hits 0x%X with 0x%X (%s): base %.3f x skill %.3f x attack %.3f x diff %.3f = %.3f, "
                  "armor %.1f%% -> %.3f (health %.2f -> %.2f)",
                  attacker, target, weapon, event.c_str(), static_cast<double>(r.base),
                  static_cast<double>(r.skill), static_cast<double>(r.attack), static_cast<double>(r.difficulty),
                  static_cast<double>(r.weapon), static_cast<double>(r.armor_percent),
                  static_cast<double>(r.final_damage), before, after);
    godot::UtilityFunctions::print(String::utf8(line));
    if (target == k_player_ref) {
        last_damager_ = attacker;
        check_player_death();
    }

    using vm::Value;
    const Value none;
    const Value aggressor = attacker != 0 ? vm_->object(attacker, "ObjectReference") : none;
    dispatch(target, "OnHit",
             {aggressor, weapon != 0 ? vm_->object(weapon, "Form") : none, none, Value::boolean(power),
              Value::boolean(sneak), Value::boolean(bash), Value::boolean(blocked)});
    if (killed) {
        flush_deaths();
        const Value killer = attacker != 0 ? vm_->object(attacker, "Actor") : none;
        dispatch(target, "OnDying", {killer});
        dispatch(target, "OnDeath", {killer});
    }

    out["attacker"] = attacker_id;
    out["target"] = target_id;
    out["weapon"] = weapon_id;
    out["ammo"] = ammo_id;
    out["event"] = natives::to_godot(event);
    out["base"] = static_cast<double>(r.base);
    out["skill_value"] = in.weapon_skill;
    out["skill"] = static_cast<double>(r.skill);
    out["attack"] = static_cast<double>(r.attack);
    out["difficulty"] = static_cast<double>(r.difficulty);
    out["sneak"] = static_cast<double>(r.sneak);
    out["weapon_damage"] = static_cast<double>(r.weapon);
    out["armor_rating"] = static_cast<double>(r.armor_rating);
    out["armor_percent"] = static_cast<double>(r.armor_percent);
    out["damage"] = static_cast<double>(r.final_damage);
    out["health_before"] = before;
    out["health_after"] = after;
    out["killed"] = killed;
    out["poison"] = hit_magic_out["poison"];
    out["enchantment"] = hit_magic_out["enchantment"];
    out["charge"] = hit_magic_out["charge"];
    out["charge_max"] = hit_magic_out["charge_max"];
    return out;
}

Dictionary SkydotPapyrus::get_corpse_place(std::int64_t ref) const {
    Dictionary out;
    if (const auto place = actors_.corpse_place(static_cast<std::uint32_t>(ref))) {
        out["space"] = static_cast<std::int64_t>(place->space);
        out["position"] = godot::Vector3(static_cast<real_t>(place->x), static_cast<real_t>(place->y),
                                         static_cast<real_t>(place->z));
        out["rot_z"] = place->rot_z;
    }
    return out;
}

void SkydotPapyrus::set_corpse_place(std::int64_t ref, std::int64_t space, const godot::Vector3& position,
                                     double rot_z) {
    if (world_.is_valid()) {
        // The cell builder puts the dead actor there.
        world_->placement().set_place(static_cast<std::uint32_t>(ref), static_cast<std::uint32_t>(space), position,
                                      static_cast<float>(rot_z));
    }
    actors_.set_corpse_place(static_cast<std::uint32_t>(ref),
                             {.space = static_cast<std::uint32_t>(space),
                              .x = static_cast<double>(position.x),
                              .y = static_cast<double>(position.y),
                              .z = static_cast<double>(position.z),
                              .rot_z = rot_z});
}

std::int64_t SkydotPapyrus::get_killer(std::int64_t ref) const {
    return static_cast<std::int64_t>(actors_.killer_of(static_cast<std::uint32_t>(ref)));
}

} // namespace skydot
