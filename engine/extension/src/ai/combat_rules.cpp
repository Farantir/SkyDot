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

void apply_scripted_memberships(std::vector<Membership>& out,
                                const std::function<std::optional<std::int32_t>(std::uint32_t faction)>& overridden,
                                const std::vector<std::uint32_t>& added) {
    for (Membership& m : out) {
        if (const auto set = overridden(m.faction)) {
            m.rank = *set;
        }
    }
    for (const std::uint32_t faction : added) {
        if (!std::ranges::any_of(out, [faction](const Membership& m) { return m.faction == faction; })) {
            out.push_back({faction, overridden(faction).value_or(0)});
        }
    }
}

std::vector<FactionRelation> with_scripted_relations(
    std::vector<FactionRelation> data, const std::vector<std::pair<std::uint32_t, std::uint8_t>>& scripted) {
    if (scripted.empty()) {
        return data;
    }
    std::erase_if(data, [&](const FactionRelation& r) {
        return std::ranges::any_of(scripted, [&](const auto& e) { return e.first == r.faction; });
    });
    for (const auto& [faction, reaction] : scripted) {
        data.push_back({faction, 0, reaction});
    }
    return data;
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
    if (in.target_airborne) {
        return in.distance > in.max_distance || !in.line_of_sight ? RangedStep::approach : RangedStep::shoot;
    }
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

MagicMode magic_mode(const MagicState& state) noexcept {
    if (state.paralyzed) {
        return MagicMode::frozen;
    }
    if (state.fear_from != 0) {
        return MagicMode::afraid;
    }
    return state.calm ? MagicMode::calmed : MagicMode::free;
}

std::uint8_t aggression_under(std::uint8_t base, const MagicState& state) noexcept {
    if (state.frenzy) {
        return 3;
    }
    return state.commanded_by != 0 ? 1 : base;
}

Reaction reaction_under(const MagicState& state, Reaction r) noexcept {
    return state.frenzy ? Reaction::enemy : r;
}

Reaction commanded_reaction(std::uint32_t a, std::uint32_t b, const CommanderOf& commander_of,
                            const ReactionOf& base) {
    const std::uint32_t ca = commander_of(a);
    const std::uint32_t cb = commander_of(b);
    const std::uint32_t pa = ca != 0 ? ca : a;
    const std::uint32_t pb = cb != 0 ? cb : b;
    if (pa == pb) {
        return Reaction::ally;
    }
    return base(pa, pb);
}

FollowStep follow_step(double distance, double near, double far) noexcept {
    if (distance > far) {
        return FollowStep::run;
    }
    return distance > near ? FollowStep::walk : FollowStep::stay;
}

std::string stagger_clip(const std::vector<std::string>& names, bool large) {
    const auto has = [](const std::string& n, const char* part) { return n.find(part) != std::string::npos; };
    std::vector<const std::string*> found;
    for (const auto& n : names) {
        if (has(n, "stagger") && !has(n, "bash")) {
            found.push_back(&n);
        }
    }
    if (found.empty()) {
        for (const auto& n : names) {
            if (has(n, "recoil")) {
                found.push_back(&n);
            }
        }
    }
    const auto order = [](const std::string* x, const std::string* y) {
        return x->size() != y->size() ? x->size() < y->size() : *x < *y;
    };
    std::ranges::sort(found, order);
    for (const auto* n : found) {
        const bool big = has(*n, "large") || has(*n, "heavy") || has(*n, "back");
        const bool small = has(*n, "small") || has(*n, "light");
        if (large ? big : small) {
            return *n;
        }
    }
    // None says its size: the plainest name for a small one, the longest for a large one.
    if (found.empty()) {
        return {};
    }
    return large ? *found.back() : *found.front();
}

} // namespace skydot::combat_ai
