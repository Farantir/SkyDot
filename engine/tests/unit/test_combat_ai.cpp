// SPDX-License-Identifier: GPL-3.0-or-later
//
// The combat AI rules (ai/combat_rules.hpp): reactions from relationships and
// factions, who starts a fight and who joins, fleeing, the melee and ranged
// steps. The facts are those of Skyrim.esm: a bandit (BanditFaction 0x1BCC0,
// aggression 2, confidence 4) has no XNAM entry towards PlayerFaction 0xDB1.
#include "ai/combat_rules.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::combat_ai;

namespace {

constexpr std::uint32_t k_bandit = 0x1BCC0;
constexpr std::uint32_t k_player = 0xDB1;
constexpr std::uint32_t k_wolf = 0x3E691;
constexpr std::uint32_t k_white_river_wolves = 0x86DDB;
constexpr std::uint32_t k_skeever = 0x330B1;

std::vector<FactionRelation> table(std::uint32_t faction) {
    switch (faction) {
    case k_bandit:
        return {{k_bandit, 0, 2}, {k_white_river_wolves, 0, 1}, {k_skeever, 0, 0}};
    case k_wolf:
        return {{k_wolf, 0, 2}};
    default: return {};
    }
}

const RelationsOf relations = table;

} // namespace

TEST_CASE("a bandit sees the player as neutral and, very aggressive, starts the fight", "[combat][ai]") {
    const std::vector<Membership> bandit{{k_bandit, 0}};
    const std::vector<Membership> player{{k_player, 0}};
    const auto r = reaction(bandit, player, std::nullopt, relations);
    CHECK(r == Reaction::neutral);
    CHECK(starts_combat(2, r));
    CHECK_FALSE(starts_combat(1, r)); // an aggressive one waits for an enemy
    CHECK_FALSE(starts_combat(0, r));
    // The player's own faction knows no bandits, so the other direction agrees.
    CHECK(reaction(player, bandit, std::nullopt, relations) == Reaction::neutral);
}

TEST_CASE("a wolf and a bandit are neutral, bandits of one faction are allies", "[combat][ai]") {
    const std::vector<Membership> bandit{{k_bandit, 0}};
    const std::vector<Membership> bandit2{{k_bandit, 1}};
    const std::vector<Membership> wolf{{k_wolf, 0}};
    const auto wolf_sees_bandit = reaction(wolf, bandit, std::nullopt, relations);
    CHECK(wolf_sees_bandit == Reaction::neutral);
    CHECK(starts_combat(2, wolf_sees_bandit)); // a very aggressive wolf attacks
    CHECK_FALSE(starts_combat(1, wolf_sees_bandit));
    const auto allies = reaction(bandit, bandit2, std::nullopt, relations);
    CHECK(allies == Reaction::ally);
    CHECK_FALSE(starts_combat(2, allies));
    CHECK(starts_combat(3, allies)); // frenzied: anyone
    // The bandits' enemy entry makes the White River Watch wolves hostile.
    const std::vector<Membership> watch_wolf{{k_white_river_wolves, 0}};
    const auto hostile = reaction(bandit, watch_wolf, std::nullopt, relations);
    CHECK(hostile == Reaction::enemy);
    CHECK(starts_combat(1, hostile));
    // The enemy entry holds from the other side too.
    CHECK(reaction(watch_wolf, bandit, std::nullopt, relations) == Reaction::enemy);
}

TEST_CASE("a rank of -1 is no membership; a relationship decides first", "[combat][ai]") {
    const std::vector<Membership> ex_bandit{{k_bandit, -1}};
    const std::vector<Membership> bandit{{k_bandit, 0}};
    CHECK(reaction(ex_bandit, bandit, std::nullopt, relations) == Reaction::neutral);
    CHECK(reaction(bandit, bandit, 7u, relations) == Reaction::enemy);   // enemy beats shared faction
    CHECK(reaction(bandit, bandit, 6u, relations) == Reaction::enemy);   // foe
    CHECK(reaction(bandit, {}, 3u, relations) == Reaction::friend_);     // friend
    CHECK(reaction(bandit, {}, 1u, relations) == Reaction::ally);        // ally
    CHECK(reaction(bandit, {}, 4u, relations) == Reaction::neutral);     // acquaintance says nothing
    CHECK(reaction(bandit, {}, 5u, relations) == Reaction::neutral);     // rival too
}

TEST_CASE("assistance joins allies, or friends and allies", "[combat][ai]") {
    CHECK_FALSE(assists(0, Reaction::ally));
    CHECK(assists(1, Reaction::ally));
    CHECK_FALSE(assists(1, Reaction::friend_));
    CHECK(assists(2, Reaction::friend_));
    CHECK(assists(2, Reaction::ally));
    CHECK_FALSE(assists(2, Reaction::neutral));
    CHECK_FALSE(assists(2, Reaction::enemy));
}

TEST_CASE("confidence sets the health below which an actor flees", "[combat][ai]") {
    const FleeSettings s;
    CHECK(flee_fraction(0, s) > 1.0F);             // cowardly: at once
    CHECK(flee_fraction(1, s) == 0.375F);
    CHECK(flee_fraction(2, s) == 0.15F);
    CHECK(flee_fraction(3, s) == 0.0375F);
    CHECK(flee_fraction(4, s) < 0.0F);             // foolhardy (the bandit's 4): never
}

TEST_CASE("the melee step approaches, attacks, and otherwise circles or falls back", "[combat][ai]") {
    MeleeInputs in;
    in.reach = 140.0;
    in.style = {.offensive_mult = 0.32F, .circle_mult = 0.73F, .fallback_mult = 0.42F, .special_attack_mult = 0.1F};
    in.distance = 300.0;
    in.attack_ready = true;
    CHECK(melee_step(in) == MeleeStep::approach);
    in.distance = 100.0;
    in.roll = 0.5;
    CHECK(melee_step(in) == MeleeStep::attack);
    in.roll = 0.05;
    CHECK(melee_step(in) == MeleeStep::power_attack);
    in.attack_ready = false;
    in.roll = 0.1;
    CHECK(melee_step(in) == MeleeStep::fallback);   // under 0.42 x 0.5
    in.roll = 0.4;
    CHECK(melee_step(in) == MeleeStep::circle);     // under 0.21 + 0.365
    in.roll = 0.9;
    CHECK(melee_step(in) == MeleeStep::hold);
}

TEST_CASE("the ranged step keeps its distance, shoots in sight and closes in otherwise", "[combat][ai]") {
    RangedInputs in;
    in.distance = 1000.0;
    in.line_of_sight = true;
    CHECK(ranged_step(in) == RangedStep::shoot);
    in.line_of_sight = false;
    CHECK(ranged_step(in) == RangedStep::approach);
    in.line_of_sight = true;
    in.distance = 2500.0;
    CHECK(ranged_step(in) == RangedStep::approach);
    in.distance = 400.0;
    CHECK(ranged_step(in) == RangedStep::keep_distance);
    in.distance = 200.0;
    CHECK(ranged_step(in) == RangedStep::keep_distance);  // no melee weapon: back off
    in.has_melee = true;
    CHECK(ranged_step(in) == RangedStep::switch_to_melee);
}
