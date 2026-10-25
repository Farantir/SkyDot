// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/magic_choice.hpp"

#include "magic/magic_rules.hpp"
#include "vm/actor_values.hpp"

#include <algorithm>
#include <cmath>

namespace skydot::combat_ai {
namespace {

namespace arch = skydot::magic::archetype;

constexpr std::uint32_t k_self = 0;
constexpr std::uint32_t k_touch = 1;
constexpr std::uint32_t k_aimed = 2;
constexpr std::uint32_t k_target_actor = 3;
constexpr std::uint32_t k_target_location = 4;

bool value_modifier(std::uint32_t a) noexcept {
    return a == arch::value_modifier || a == arch::peak_value_modifier || a == arch::dual_value_modifier;
}

bool affordable(const SpellFacts& s, double magicka) noexcept { return s.cost <= magicka; }

const SpellFacts* first_of(const MageInputs& in, SpellRole role, double reserve_after) {
    for (const auto& s : in.spells) {
        if (classify(s) == role && !s.active && affordable(s, in.magicka) && in.magicka - s.cost >= reserve_after) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace

SpellRole classify(const SpellFacts& s) noexcept {
    if (!s.plain) {
        return SpellRole::other;
    }
    if (s.hostile) {
        const bool at_enemy = s.delivery == k_touch || s.delivery == k_aimed || s.delivery == k_target_actor;
        return at_enemy ? SpellRole::attack : SpellRole::other;
    }
    // Conjure spells are delivered to a target location (CK), the rest of the buffs to self.
    if (s.archetype == arch::summon_creature && (s.delivery == k_self || s.delivery == k_target_location)) {
        return SpellRole::summon;
    }
    if (s.delivery != k_self) {
        return SpellRole::other;
    }
    if (s.archetype == arch::cloak) {
        return SpellRole::cloak;
    }
    if (value_modifier(s.archetype) && s.actor_value >= 0) {
        if (static_cast<std::uint32_t>(s.actor_value) == vm::av::k_health && !s.recover) {
            return SpellRole::heal;
        }
        return SpellRole::fortify;
    }
    return SpellRole::other;
}

bool prefers_magic(const MageInputs& in) noexcept {
    if (in.style.magic <= 0.0) {
        return false;
    }
    double weapons = in.style.unarmed;
    if (in.has_melee) {
        weapons = std::max(weapons, in.style.melee);
    }
    if (in.has_bow) {
        weapons = std::max(weapons, in.style.ranged);
    }
    return in.style.magic > weapons;
}

double hold_seconds(double roll, double cost_per_second, double magicka) noexcept {
    const double wanted = k_hold_min + std::clamp(roll, 0.0, 1.0) * (k_hold_max - k_hold_min);
    const double lasts = cost_per_second > 0.0 ? magicka / cost_per_second : wanted;
    return std::max(0.1, std::min(wanted, lasts));
}

MageDecision decide(const MageInputs& in) noexcept {
    MageDecision out;
    bool any_attack = false;
    for (const auto& s : in.spells) {
        any_attack = any_attack || classify(s) == SpellRole::attack;
    }
    const bool magic_fight = prefers_magic(in);
    const auto cast = [&](const SpellFacts& s, double hold) {
        out.action = MageAction::cast;
        out.role = classify(s);
        out.spell = s.form;
        out.hold = s.concentration ? hold : 0.0;
        return out;
    };

    if (!in.ready) {
        if (any_attack && magic_fight) {
            const bool close = in.distance < k_mage_min_distance;
            bool far_spell = false;
            for (const auto& s : in.spells) {
                far_spell = far_spell || (classify(s) == SpellRole::attack && s.delivery == k_aimed && s.reach >= 1000.0);
            }
            out.action = close && far_spell ? MageAction::keep_distance : MageAction::wait;
        }
        return out;
    }

    // 1. Heal itself when hurt.
    if (in.health_fraction < k_heal_below) {
        if (const auto* heal = first_of(in, SpellRole::heal, 0.0)) {
            return cast(*heal, hold_seconds(1.0, heal->cost, in.magicka));
        }
    }
    // 2. A buff it does not have on, while the fight is young enough to spare the Magicka.
    const double reserve = in.magicka_max * k_buff_reserve;
    if (const auto* summon = first_of(in, SpellRole::summon, reserve)) {
        return cast(*summon, 0.0);
    }
    if (const auto* fortify = first_of(in, SpellRole::fortify, reserve)) {
        return cast(*fortify, 0.0);
    }
    if (in.distance <= k_cloak_distance) {
        if (const auto* cloak = first_of(in, SpellRole::cloak, reserve)) {
            return cast(*cloak, 0.0);
        }
    }
    // 3. An attack spell.
    if (!magic_fight || !any_attack) {
        return out;
    }
    std::vector<const SpellFacts*> in_reach;
    const SpellFacts* longest = nullptr;
    for (const auto& s : in.spells) {
        if (classify(s) != SpellRole::attack || !affordable(s, in.magicka)) {
            continue;
        }
        if (longest == nullptr || s.reach > longest->reach) {
            longest = &s;
        }
        if (in.line_of_sight && in.distance <= s.reach) {
            in_reach.push_back(&s);
        }
    }
    if (in_reach.empty()) {
        if (longest != nullptr) {
            out.action = MageAction::approach;
            out.role = SpellRole::attack;
            out.spell = longest->form;
        }
        return out; // nothing affordable (or in sight): the weapons
    }
    // A die picks among the spells in reach; the same die, rescaled, picks how long a held one is held.
    const double scaled = std::clamp(in.roll, 0.0, 0.999999) * static_cast<double>(in_reach.size());
    const auto index = static_cast<std::size_t>(scaled);
    const SpellFacts& pick = *in_reach[index];
    return cast(pick, hold_seconds(scaled - std::floor(scaled), pick.cost, in.magicka));
}

bool shout_aimed(double aimed_seconds, double min_aim_seconds) noexcept {
    return aimed_seconds >= min_aim_seconds;
}

std::uint32_t decide_shout(const ShoutInputs& in) noexcept {
    if (in.recovering || !in.aimed || !in.line_of_sight || in.shout_mult <= 0.0) {
        return 0;
    }
    const double chance = std::min(in.shout_mult, 1.0);
    const double roll = std::clamp(in.roll, 0.0, 0.999999);
    if (roll >= chance) {
        return 0;
    }
    std::vector<const ShoutFacts*> usable;
    for (const auto& s : in.shouts) {
        if (s.hostile && in.distance <= s.reach * k_reach_fraction) {
            usable.push_back(&s);
        }
    }
    if (usable.empty()) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(roll / chance * static_cast<double>(usable.size()));
    return usable[std::min(index, usable.size() - 1)]->form;
}

} // namespace skydot::combat_ai
