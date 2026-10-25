// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a mage casts (ai/magic_choice.hpp): classifying spells, the weapons-or-spells decision from the
// combat style, healing when hurt, the buffs once, an attack spell in reach, holding a concentration
// spell. The spells are those of Skyrim.esm in shape (Flames: concentration, aimed, short reach;
// Firebolt: fire and forget, aimed, far; Healing: concentration, self, restores Health).
#include "ai/magic_choice.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::combat_ai;

namespace {

constexpr int k_health = 24;
constexpr int k_armor = 22;

SpellFacts flames() {
    SpellFacts s;
    s.form = 0x12FCD;
    s.cost = 8.0;
    s.concentration = true;
    s.delivery = 2;
    s.reach = 500.0;
    s.hostile = true;
    return s;
}
SpellFacts firebolt() {
    SpellFacts s;
    s.form = 0x12FD0;
    s.cost = 25.0;
    s.delivery = 2;
    s.reach = 3000.0;
    s.hostile = true;
    return s;
}
SpellFacts healing() {
    SpellFacts s;
    s.form = 0x12FCC;
    s.cost = 12.0;
    s.concentration = true;
    s.delivery = 0;
    s.recover = false; // Restore Health stays; Fortify Health is undone at the end
    s.archetype = 0;
    s.actor_value = k_health;
    return s;
}
SpellFacts familiar() {
    SpellFacts s;
    s.form = 0x640B6;
    s.cost = 60.0;
    s.delivery = 0;
    s.archetype = 18;
    return s;
}
SpellFacts oakflesh() {
    SpellFacts s;
    s.form = 0x5AD5E;
    s.cost = 40.0;
    s.delivery = 0;
    s.archetype = 34;
    s.recover = true;
    s.actor_value = k_armor;
    return s;
}
SpellFacts cloak() {
    SpellFacts s;
    s.form = 0x1A4CC;
    s.cost = 90.0;
    s.delivery = 0;
    s.archetype = 35;
    return s;
}

MageInputs mage(std::vector<SpellFacts> spells) {
    MageInputs in;
    in.spells = std::move(spells);
    in.magicka = 200.0;
    in.magicka_max = 200.0;
    in.distance = 800.0;
    in.style = {1.0, 0.2, 0.2, 0.2};
    return in;
}

} // namespace

TEST_CASE("spells are classified by what they do to whom", "[magic][ai]") {
    CHECK(classify(flames()) == SpellRole::attack);
    CHECK(classify(firebolt()) == SpellRole::attack);
    CHECK(classify(healing()) == SpellRole::heal);
    CHECK(classify(familiar()) == SpellRole::summon);
    CHECK(classify(oakflesh()) == SpellRole::fortify);
    CHECK(classify(cloak()) == SpellRole::cloak);
    SpellFacts fortify_health = healing();
    fortify_health.recover = true;
    CHECK(classify(fortify_health) == SpellRole::fortify);
    SpellFacts power = firebolt();
    power.plain = false;
    CHECK(classify(power) == SpellRole::other);
    SpellFacts rune = firebolt();
    rune.delivery = 4;
    CHECK(classify(rune) == SpellRole::other);
    SpellFacts conjure = familiar();
    conjure.delivery = 4; // Conjure Familiar is delivered to a target location
    CHECK(classify(conjure) == SpellRole::summon);
    SpellFacts aimed_buff = oakflesh();
    aimed_buff.delivery = 2;
    CHECK(classify(aimed_buff) == SpellRole::other);
}

TEST_CASE("the style's multipliers decide between spells and weapons", "[magic][ai]") {
    MageInputs in = mage({firebolt()});
    in.has_melee = true;
    CHECK(prefers_magic(in));
    in.style = {0.2, 1.0, 0.2, 0.2}; // a warrior with a spell
    CHECK_FALSE(prefers_magic(in));
    in.style = {0.5, 0.5, 0.5, 0.5}; // a tie goes to the weapons
    CHECK_FALSE(prefers_magic(in));
    in.has_melee = false;
    CHECK_FALSE(prefers_magic(in)); // the fists tie too (a spider that spits and bites)
    in.style = {4.05, 0.76, 1.0, 1.0}; // csHumanMagic
    CHECK(prefers_magic(in));
    in.style.magic = 0.0;
    CHECK_FALSE(prefers_magic(in)); // the style forbids it
    in = mage({firebolt()});
    in.has_bow = true;
    in.style = {0.4, 0.1, 0.9, 0.1};
    CHECK_FALSE(prefers_magic(in));
}

TEST_CASE("a mage casts an attack spell in reach and approaches one out of reach", "[magic][ai]") {
    MageInputs in = mage({flames(), firebolt()});
    in.distance = 1500.0; // beyond Flames, within Firebolt
    auto d = decide(in);
    CHECK(d.action == MageAction::cast);
    CHECK(d.spell == 0x12FD0);
    CHECK(d.hold == 0.0);
    in.distance = 300.0; // both reach; the die picks
    in.roll = 0.0;
    d = decide(in);
    CHECK(d.action == MageAction::cast);
    CHECK(d.spell == 0x12FCD);
    CHECK(d.hold >= k_hold_min);
    CHECK(d.hold <= k_hold_max);
    in.roll = 0.9;
    d = decide(in);
    CHECK(d.spell == 0x12FD0);
    in = mage({flames()});
    in.distance = 1200.0; // only Flames and it is far
    d = decide(in);
    CHECK(d.action == MageAction::approach);
    CHECK(d.spell == 0x12FCD);
    in.distance = 300.0;
    in.line_of_sight = false;
    CHECK(decide(in).action == MageAction::approach); // a wall between: closer
}

TEST_CASE("without Magicka for any attack, or in a warrior's style, the weapons go on", "[magic][ai]") {
    MageInputs in = mage({firebolt()});
    in.magicka = 10.0;
    CHECK(decide(in).action == MageAction::weapons);
    in = mage({firebolt()});
    in.has_melee = true;
    in.style = {0.2, 1.0, 0.2};
    CHECK(decide(in).action == MageAction::weapons);
    CHECK(decide(mage({})).action == MageAction::weapons);
}

TEST_CASE("a hurt mage heals itself with a healing spell it can afford", "[magic][ai]") {
    MageInputs in = mage({firebolt(), healing()});
    in.distance = 800.0;
    in.health_fraction = 0.6;
    CHECK(decide(in).spell == 0x12FD0);
    in.health_fraction = 0.4;
    auto d = decide(in);
    CHECK(d.action == MageAction::cast);
    CHECK(d.role == SpellRole::heal);
    CHECK(d.spell == 0x12FCC);
    CHECK(d.hold > 0.0);
    in.magicka = 5.0; // cannot pay it
    CHECK(decide(in).action == MageAction::weapons);
    in = mage({firebolt()});
    in.health_fraction = 0.1; // no healing spell known
    CHECK(decide(in).spell == 0x12FD0);
}

TEST_CASE("buffs are cast once, while the Magicka allows", "[magic][ai]") {
    MageInputs in = mage({firebolt(), familiar(), oakflesh(), cloak()});
    in.distance = 1000.0;
    auto d = decide(in);
    CHECK(d.role == SpellRole::summon);
    in.spells[1].active = true;
    d = decide(in);
    CHECK(d.role == SpellRole::fortify);
    in.spells[2].active = true;
    d = decide(in);
    CHECK(d.role == SpellRole::attack); // the cloak waits for the enemy to come near
    in.distance = 400.0;
    d = decide(in);
    CHECK(d.role == SpellRole::cloak);
    in.spells[3].active = true;
    CHECK(decide(in).role == SpellRole::attack);
    // Not when the buff would leave less than a quarter of the Magicka.
    in = mage({firebolt(), familiar()});
    in.magicka = 100.0; // 100 - 60 < 50
    CHECK(decide(in).role == SpellRole::attack);
    in.magicka = 140.0;
    CHECK(decide(in).role == SpellRole::summon);
}

TEST_CASE("a mage recovering from a cast waits, or backs off from a long spell", "[magic][ai]") {
    MageInputs in = mage({firebolt()});
    in.ready = false;
    in.distance = 1000.0;
    CHECK(decide(in).action == MageAction::wait);
    in.distance = 200.0;
    CHECK(decide(in).action == MageAction::keep_distance);
    in = mage({flames()});
    in.ready = false;
    in.distance = 200.0;
    CHECK(decide(in).action == MageAction::wait); // Flames is a short spell: no backing off
    in = mage({});
    in.ready = false;
    CHECK(decide(in).action == MageAction::weapons);
}

TEST_CASE("a concentration spell is held between one and three seconds, as long as the Magicka lasts", "[magic][ai]") {
    CHECK(hold_seconds(0.0, 8.0, 200.0) == k_hold_min);
    CHECK(hold_seconds(1.0, 8.0, 200.0) == k_hold_max);
    CHECK(hold_seconds(0.5, 8.0, 200.0) == 2.0);
    CHECK(hold_seconds(1.0, 8.0, 12.0) == 1.5);
    CHECK(hold_seconds(1.0, 8.0, 0.0) == 0.1);
}

namespace {
ShoutInputs shouter() {
    ShoutInputs in;
    // Unrelenting Force (draugr): a cone a few hundred units long.
    in.shouts = {{0x9DE93, 0x13F3A, 700.0, 15.0, true}};
    in.aimed = true;
    in.distance = 300.0;
    in.shout_mult = 1.0;
    in.roll = 0.3;
    return in;
}
} // namespace

TEST_CASE("an NPC shouts when it is aimed, in sight, in reach, not recovering and its style likes it", "[magic][ai][shout]") {
    auto in = shouter();
    CHECK(decide_shout(in) == 0x9DE93);
    in.recovering = true;
    CHECK(decide_shout(in) == 0);
    in = shouter();
    in.aimed = false;
    CHECK(decide_shout(in) == 0);
    in = shouter();
    in.line_of_sight = false;
    CHECK(decide_shout(in) == 0);
    in = shouter();
    in.distance = 700.0 * k_reach_fraction + 1.0;
    CHECK(decide_shout(in) == 0);
    in.distance = 700.0 * k_reach_fraction;
    CHECK(decide_shout(in) == 0x9DE93);
    in = shouter();
    in.shout_mult = 0.0;
    CHECK(decide_shout(in) == 0); // a style without the mult (or none) never shouts
    in.shout_mult = 0.5;
    in.roll = 0.6;
    CHECK(decide_shout(in) == 0); // the die is the mult
    in.roll = 0.4;
    CHECK(decide_shout(in) == 0x9DE93);
}

TEST_CASE("the die picks among the shouts in reach; a friendly shout is not said at an enemy", "[magic][ai][shout]") {
    auto in = shouter();
    in.shouts.push_back({0x1111, 0x2222, 500.0, 8.0, true});
    in.shouts.push_back({0x3333, 0x4444, 500.0, 8.0, false});
    in.roll = 0.1;
    CHECK(decide_shout(in) == 0x9DE93);
    in.roll = 0.9;
    CHECK(decide_shout(in) == 0x1111);
    in.distance = 450.0; // out of reach of the 500 unit one
    in.roll = 0.9;
    CHECK(decide_shout(in) == 0x9DE93);
}

TEST_CASE("aiming takes the minimum seconds", "[magic][ai][shout]") {
    CHECK_FALSE(shout_aimed(1.9, 2.0));
    CHECK(shout_aimed(2.0, 2.0));
}
