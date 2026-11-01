// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus.hit_facts and land_hit (ARCHITECTURE.md 8.2, AttackRules): the
// inputs of one hit resolved from the world and the session, and, once the
// rules have worked out the damage, Health damaged through ActorStates, then
// OnHit and, on a death, OnDying and OnDeath. The formula is AttackRules'.
#include "vm/papyrus.hpp"

#include "data/actors.hpp"
#include "data/fb_search.hpp"
#include "vm/papyrus_common.hpp"

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

Dictionary SkydotPapyrus::hit_facts(std::int64_t attacker_id, std::int64_t target_id, std::int64_t weapon_id,
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
    if (ghost_ignores_hit(target)) {
        out["error"] = "the target is a ghost";
        return out;
    }
    const bool power = (flags & HIT_POWER) != 0;

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
    double attack_mult = 1.0;
    if (race != nullptr && race->attacks() != nullptr) {
        for (const auto* a : *race->attacks()) {
            if (same_event(a->event(), event)) {
                attack_mult = static_cast<double>(a->damage_mult());
                break;
            }
        }
    }

    double weapon_damage = 0.0;
    std::int32_t skill_av = -1;
    if (weapon != 0) {
        const auto* base = lookup(root->bases(), weapon);
        const auto* item = base != nullptr ? base->item() : nullptr;
        if (item != nullptr) {
            weapon_damage = static_cast<double>(item->damage());
            skill_av = item->weapon_skill();
        }
    } else if (race != nullptr) {
        weapon_damage = static_cast<double>(race->unarmed_damage());
    }
    double ammo_damage = 0.0;
    if (ammo_id != 0) {
        ammo_damage = static_cast<double>(item_info_of(static_cast<std::uint32_t>(ammo_id)).damage);
    }
    double weapon_skill = 0.0;
    if (skill_av >= 0) {
        weapon_skill = actors_.get(attacker, static_cast<std::uint32_t>(skill_av));
    }
    std::int64_t temper_quality = 0;
    if (extra_id != 0) {
        if (const auto* extra = items_.extras().find(static_cast<std::uint32_t>(extra_id))) {
            temper_quality = vm::temper_quality(extra->health);
        }
    }

    resolve_owner(target);
    godot::Array armor;
    if (const vm::Equipment* worn = items_.find(target)) {
        const auto add = [&](std::uint32_t form) {
            const vm::ItemInfo info = item_info_of(form);
            if (info.known && (info.category == vm::ItemCategory::armor || info.category == vm::ItemCategory::clothing)) {
                const auto* extra = items_.extras().find(worn->worn_extra_of(form));
                Dictionary piece;
                piece["rating"] = static_cast<double>(info.armor_rating);
                piece["armor_type"] = static_cast<std::int64_t>(info.armor_type);
                piece["quality"] = static_cast<std::int64_t>(extra != nullptr ? vm::temper_quality(extra->health) : 0);
                piece["chest"] = (info.biped_slots & (1U << 2)) != 0;
                armor.push_back(piece);
            }
        };
        for (const std::uint32_t form : worn->worn) {
            add(form);
        }
        if (worn->left != 0 && vm::is_shield(item_info_of(worn->left))) {
            add(worn->left);
        }
    }

    out["attacker"] = attacker_id;
    out["target"] = target_id;
    out["weapon"] = weapon_id;
    out["ammo"] = ammo_id;
    out["event"] = natives::to_godot(event);
    out["attacker_is_player"] = attacker == k_player_ref;
    out["target_is_player"] = target == k_player_ref;
    out["weapon_damage"] = weapon_damage;
    out["ammo_damage"] = ammo_damage;
    out["has_weapon_skill"] = skill_av >= 0;
    out["weapon_skill"] = weapon_skill;
    out["temper_quality"] = temper_quality;
    out["attack_mult"] = attack_mult;
    out["light_armor_skill"] = actors_.get(target, vm::av::k_first_skill + 6);  // LightArmor 12
    out["heavy_armor_skill"] = actors_.get(target, vm::av::k_first_skill + 5);  // HeavyArmor 11
    out["armor"] = armor;
    return out;
}

Dictionary SkydotPapyrus::land_hit(std::int64_t attacker_id, std::int64_t target_id, std::int64_t weapon_id,
                                   std::int64_t extra_id, double damage, const String& attack_event,
                                   std::int64_t flags, const Dictionary& breakdown) {
    Dictionary out;
    const auto attacker = static_cast<std::uint32_t>(attacker_id);
    const auto target = static_cast<std::uint32_t>(target_id);
    const auto weapon = static_cast<std::uint32_t>(weapon_id);
    const bool power = (flags & HIT_POWER) != 0;
    const bool sneak = (flags & HIT_SNEAK) != 0;
    const bool bash = (flags & HIT_BASH) != 0;
    const bool blocked = (flags & HIT_BLOCKED) != 0;
    const std::string event = natives::utf8(attack_event);

    const double before = actors_.get(target, vm::av::k_health);
    actors_.damage(target, vm::av::k_health, damage);
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
                  attacker, target, weapon, event.c_str(), static_cast<double>(breakdown.get("base", 0.0)),
                  static_cast<double>(breakdown.get("skill", 1.0)), static_cast<double>(breakdown.get("attack", 1.0)),
                  static_cast<double>(breakdown.get("difficulty", 1.0)),
                  static_cast<double>(breakdown.get("weapon_damage", 0.0)),
                  static_cast<double>(breakdown.get("armor_percent", 0.0)), damage, before, after);
    godot::UtilityFunctions::print(String::utf8(line));
    if (target == k_player_ref) {
        combat_.set_last_damager(attacker);
        check_player_death();
    }

    using vm::Value;
    const Value none;
    const Value aggressor = attacker != 0 ? vm_->object(attacker, "ObjectReference") : none;
    dispatch(target, "OnHit",
             {aggressor, weapon != 0 ? vm_->object(weapon, "Form") : none, none, Value::boolean(power),
              Value::boolean(sneak), Value::boolean(bash), Value::boolean(blocked)});
    if (killed) {
        flush_deaths(); // announces the death: actor_died, OnDying and OnDeath
    }

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

void SkydotPapyrus::dragon_soul_absorbed() {
    const auto* g = world_.is_valid() ? WorldData::find_gmst(world_->data().root(), "sDragonSoulAcquired") : nullptr;
    const std::string text = g != nullptr && g->text_value() != nullptr && g->text_value()->size() > 0
                                 ? g->text_value()->str()
                                 : std::string("Dragon Soul Absorbed");
    godot::UtilityFunctions::print(natives::to_godot("[Notification] " + text));
    emit_signal("message", natives::to_godot(text), false);
}

} // namespace skydot
