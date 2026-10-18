// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/combat_rules.hpp"

#include <algorithm>

namespace skydot::combat_ai {

namespace {

constexpr std::uint32_t k_reaction_enemy = 1;
constexpr std::uint32_t k_reaction_ally = 2;
constexpr std::uint32_t k_reaction_friend = 3;

bool member(const Membership& m) { return m.rank >= 0; }

} // namespace

Reaction reaction(const std::vector<Membership>& a, const std::vector<Membership>& b,
                  std::optional<std::uint32_t> relationship_rank, const RelationsOf& relations) {
    if (relationship_rank) {
        switch (*relationship_rank) {
        case 0: // lover
        case 2: // confidant
        case 3: // friend
            return Reaction::friend_;
        case 1: return Reaction::ally;
        case 6: // foe
        case 7: // enemy
        case 8: // archnemesis
            return Reaction::enemy;
        default: break; // acquaintance, rival
        }
    }
    for (const Membership& x : a) {
        if (!member(x)) {
            continue;
        }
        for (const Membership& y : b) {
            if (member(y) && y.faction == x.faction) {
                return Reaction::ally;
            }
        }
    }
    bool hostile = false;
    bool friendly_entry = false;
    const auto look = [&](const std::vector<Membership>& from, const std::vector<Membership>& to) {
        for (const Membership& x : from) {
            if (!member(x)) {
                continue;
            }
            for (const FactionRelation& r : relations(x.faction)) {
                const bool listed = std::ranges::any_of(
                    to, [&](const Membership& y) { return member(y) && y.faction == r.faction; });
                if (!listed) {
                    continue;
                }
                hostile = hostile || r.reaction == k_reaction_enemy;
                friendly_entry = friendly_entry || r.reaction == k_reaction_ally || r.reaction == k_reaction_friend;
            }
        }
    };
    look(a, b);
    look(b, a);
    if (friendly_entry) {
        return Reaction::friend_;
    }
    return hostile ? Reaction::enemy : Reaction::neutral;
}

bool starts_combat(std::uint8_t aggression, Reaction r) noexcept {
    switch (aggression) {
    case 0: return false;
    case 1: return r == Reaction::enemy;
    case 2: return r == Reaction::enemy || r == Reaction::neutral;
    default: return true;
    }
}

bool assists(std::uint8_t assistance, Reaction r) noexcept {
    switch (assistance) {
    case 0: return false;
    case 1: return r == Reaction::ally;
    default: return friendly(r);
    }
}

float flee_fraction(std::uint8_t confidence, const FleeSettings& s) noexcept {
    switch (confidence) {
    case 0: return 2.0F;
    case 1: return s.cautious;
    case 2: return s.average;
    case 3: return s.brave;
    default: return -1.0F;
    }
}

MeleeStep melee_step(const MeleeInputs& in) noexcept {
    if (in.distance > in.reach) {
        return MeleeStep::approach;
    }
    if (in.attack_ready) {
        return in.roll < static_cast<double>(in.style.special_attack_mult) ? MeleeStep::power_attack
                                                                         : MeleeStep::attack;
    }
    const double fallback = static_cast<double>(in.style.fallback_mult) * k_wait_weight;
    const double circle = static_cast<double>(in.style.circle_mult) * k_wait_weight;
    if (in.roll < fallback) {
        return MeleeStep::fallback;
    }
    if (in.roll < fallback + circle) {
        return MeleeStep::circle;
    }
    return MeleeStep::hold;
}

RangedStep ranged_step(const RangedInputs& in) noexcept {
    if (in.has_melee && in.distance < in.melee_distance) {
        return RangedStep::switch_to_melee;
    }
    if (in.distance < in.min_distance) {
        return RangedStep::keep_distance;
    }
    if (in.distance > in.max_distance || !in.line_of_sight) {
        return RangedStep::approach;
    }
    return RangedStep::shoot;
}

} // namespace skydot::combat_ai
