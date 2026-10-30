// SPDX-License-Identifier: GPL-3.0-or-later
//
// The combat AI rules (ai/combat_rules.hpp): reactions from relationships and
// factions, who starts a fight and who joins, fleeing, the melee and ranged
// steps. The facts are those of Skyrim.esm: a bandit (BanditFaction 0x1BCC0,
// aggression 2, confidence 4) has no XNAM entry towards PlayerFaction 0xDB1.
#include "ai/combat_rules.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>

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

TEST_CASE("the ranged step against an airborne target only approaches or shoots", "[combat][ai]") {
    RangedInputs in;
    in.target_airborne = true;
    in.has_melee = true;
    in.line_of_sight = true;
    in.distance = 100.0;
    CHECK(ranged_step(in) == RangedStep::shoot);  // no melee, no backing off
    in.distance = 1500.0;
    CHECK(ranged_step(in) == RangedStep::shoot);
    in.distance = 2500.0;
    CHECK(ranged_step(in) == RangedStep::approach);
    in.distance = 1000.0;
    in.line_of_sight = false;
    CHECK(ranged_step(in) == RangedStep::approach);
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

// ---- magic ----

TEST_CASE("held magic picks the mode: paralysis, then fear, then calm", "[combat][ai][magic]") {
    MagicState s;
    CHECK(magic_mode(s) == MagicMode::free);
    s.calm = true;
    CHECK(magic_mode(s) == MagicMode::calmed);
    s.fear_from = 0x14;
    CHECK(magic_mode(s) == MagicMode::afraid);
    s.paralyzed = true;
    CHECK(magic_mode(s) == MagicMode::frozen);
    // Frenzy alone leaves it free: it only changes whom it fights.
    MagicState f;
    f.frenzy = true;
    CHECK(magic_mode(f) == MagicMode::free);
}

TEST_CASE("frenzy fights anyone, a commanded actor its master's enemies", "[combat][ai][magic]") {
    MagicState f;
    f.frenzy = true;
    CHECK(aggression_under(0, f) == 3);
    CHECK(reaction_under(f, Reaction::friend_) == Reaction::enemy);
    CHECK(starts_combat(aggression_under(0, f), reaction_under(f, Reaction::ally)));
    MagicState c;
    c.commanded_by = 0x14;
    CHECK(aggression_under(0, c) == 1);
    CHECK(aggression_under(3, c) == 1);
    CHECK(reaction_under(c, Reaction::neutral) == Reaction::neutral);
    CHECK(aggression_under(2, MagicState{}) == 2);
    // The frenzy over: an ally is not fought any more, a foe is.
    CHECK_FALSE(fights_after_frenzy(1, Reaction::ally));
    CHECK(fights_after_frenzy(1, Reaction::enemy));
}

TEST_CASE("a summon is its commander's ally and takes its reactions", "[combat][ai][magic]") {
    constexpr std::uint32_t player = 0x14, summon = 0xFF000001, other = 0xFF000002, wolf = 0x100, bandit = 0x200;
    const CommanderOf commander = [&](std::uint32_t r) { return r == summon || r == other ? player : 0u; };
    const ReactionOf base = [&](std::uint32_t a, std::uint32_t b) {
        if (a == player && b == bandit) return Reaction::enemy;
        if (a == bandit && b == player) return Reaction::enemy;
        if (a == player && b == wolf) return Reaction::neutral;
        return Reaction::neutral;
    };
    CHECK(commanded_reaction(summon, player, commander, base) == Reaction::ally);
    CHECK(commanded_reaction(player, summon, commander, base) == Reaction::ally);
    CHECK(commanded_reaction(summon, other, commander, base) == Reaction::ally);
    CHECK(commanded_reaction(summon, bandit, commander, base) == Reaction::enemy);
    CHECK(commanded_reaction(bandit, summon, commander, base) == Reaction::enemy);
    CHECK(commanded_reaction(summon, wolf, commander, base) == Reaction::neutral);
    CHECK(commanded_reaction(bandit, wolf, commander, base) == Reaction::neutral);
}

TEST_CASE("a commanded actor follows: stay, walk, run", "[combat][ai][magic]") {
    CHECK(follow_step(100.0) == FollowStep::stay);
    CHECK(follow_step(400.0) == FollowStep::walk);
    CHECK(follow_step(900.0) == FollowStep::run);
}

TEST_CASE("the stagger clip is found by name and size", "[combat][ai][magic]") {
    const std::vector<std::string> human{"idle", "staggerstart", "staggerbackwards", "staggerbash", "recoillargestart"};
    CHECK(stagger_clip(human, true) == "staggerbackwards");
    CHECK(stagger_clip(human, false) == "staggerstart");
    const std::vector<std::string> sized{"staggersmall", "staggerlarge", "staggerx"};
    CHECK(stagger_clip(sized, true) == "staggerlarge");
    CHECK(stagger_clip(sized, false) == "staggersmall");
    CHECK(stagger_clip({"idle", "recoilstart"}, false) == "recoilstart");
    CHECK(stagger_clip({"idle", "walk"}, true).empty());
}

TEST_CASE("scripted memberships replace, remove and add to the NPC_'s", "[combat_ai]") {
    std::vector<Membership> m = {{0x10, 1}, {0x20, 0}};
    const std::map<std::uint32_t, std::int32_t> set = {{0x10, 5}, {0x20, -1}, {0x40, 2}};
    apply_scripted_memberships(
        m,
        [&](std::uint32_t f) {
            const auto it = set.find(f);
            return it != set.end() ? std::optional<std::int32_t>(it->second) : std::nullopt;
        },
        {0x40});
    REQUIRE(m.size() == 3);
    CHECK(m[0].rank == 5);
    CHECK(m[1].rank == -1); // removed
    CHECK(m[2].faction == 0x40);
    CHECK(m[2].rank == 2);
}

TEST_CASE("SetEnemy makes a dragon and the player's side enemies, SetAlly undoes it", "[combat_ai]") {
    constexpr std::uint32_t dragon_faction = 0x100, player_faction = 0x200, guard_faction = 0x300;
    const std::vector<Membership> dragon = {{dragon_faction, 0}};
    const std::vector<Membership> player = {{player_faction, 0}};
    const std::vector<Membership> guard = {{guard_faction, 0}};
    // The data: guards are the player's friends (the guard faction lists the player's); the dragon knows nobody.
    const auto data = [&](std::uint32_t f) {
        std::vector<FactionRelation> out;
        if (f == guard_faction) {
            out.push_back({player_faction, 0, 3});
        }
        return out;
    };
    CHECK(reaction(dragon, player, std::nullopt, data) == Reaction::neutral);
    // SetEnemy(PlayerFaction): dragon faction sees the player's as enemy, and the other way round.
    std::vector<std::pair<std::uint32_t, std::uint8_t>> dragon_set = {{player_faction, 1}};
    std::vector<std::pair<std::uint32_t, std::uint8_t>> player_set = {{dragon_faction, 1}};
    const auto scripted = [&](std::uint32_t f) {
        if (f == dragon_faction) {
            return with_scripted_relations(data(f), dragon_set);
        }
        if (f == player_faction) {
            return with_scripted_relations(data(f), player_set);
        }
        return data(f);
    };
    CHECK(reaction(dragon, player, std::nullopt, scripted) == Reaction::enemy);
    CHECK(reaction(player, dragon, std::nullopt, scripted) == Reaction::enemy);
    CHECK(reaction(guard, player, std::nullopt, scripted) == Reaction::friend_); // the data's stays
    // A scripted entry replaces the data's for that faction: SetAlly over a data enemy.
    const auto hostile_data = [](std::uint32_t) { return std::vector<FactionRelation>{{0x200, 0, 1}}; };
    const std::vector<std::pair<std::uint32_t, std::uint8_t>> ally = {{0x200, 2}};
    const auto merged = with_scripted_relations(hostile_data(0x100), ally);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].reaction == 2);
    CHECK(with_scripted_relations(hostile_data(0x100), {}).size() == 1);
}

TEST_CASE("a relationship rank a script set becomes the RELA rank the reaction reads", "[combat_ai]") {
    CHECK(rela_rank_of(4) == 0);   // lover
    CHECK(rela_rank_of(3) == 1);   // ally
    CHECK(rela_rank_of(2) == 2);   // confidant
    CHECK(rela_rank_of(0) == 4);   // acquaintance
    CHECK(rela_rank_of(-3) == 7);  // enemy
    CHECK(rela_rank_of(-9) == 8);  // clamped to archnemesis
    const std::vector<Membership> a = {{0x1, 0}};
    const std::vector<Membership> b = {{0x2, 0}};
    const auto none = [](std::uint32_t) { return std::vector<FactionRelation>{}; };
    CHECK(reaction(a, b, rela_rank_of(3), none) == Reaction::ally);
    CHECK(reaction(a, b, rela_rank_of(-3), none) == Reaction::enemy);
    CHECK(reaction(a, b, rela_rank_of(0), none) == Reaction::neutral);
}
