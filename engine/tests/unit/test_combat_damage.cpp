// SPDX-License-Identifier: GPL-3.0-or-later
//
// The damage formula (combat/damage.hpp): the Iron Sword at OneHanded 15
// against an iron armor set, the power attack, the armor cap, unarmed.
#include "combat/damage.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace skydot::combat;
using Catch::Approx;

namespace {

HitInputs iron_sword_vs_iron_armor() {
    HitInputs in;
    in.weapon_damage = 7.0F;
    in.weapon_skill = 15.0;
    in.attacker_is_player = true;
    in.target_is_player = true;
    in.heavy_armor_skill = 15.0;
    // Cuirass 25, helmet 15, gauntlets 10, boots 10 (heavy).
    for (const float rating : {25.0F, 15.0F, 10.0F, 10.0F}) {
        in.armor.push_back({rating, 1});
    }
    return in;
}

} // namespace

TEST_CASE("an Iron Sword at OneHanded 15 against a heavy iron set", "[combat]") {
    const CombatSettings settings;
    const HitResult r = compute_hit(iron_sword_vs_iron_armor(), settings);
    CHECK(r.skill == Approx(1.075));             // lerp(1, 1.5, 0.15)
    CHECK(r.weapon == Approx(7.525));
    // factor lerp(1, 1.4, 0.15) = 1.06: 27 + 16 + 11 + 11 = 65.
    CHECK(r.armor_rating == Approx(65.0));
    CHECK(r.armor_percent == Approx(65.0 * 0.12 + 4 * 3.0)); // 19.8
    CHECK(r.final_damage == Approx(7.525 * (1.0 - 0.198)));
}

TEST_CASE("a power attack doubles the damage, an NPC attacker has no skill factor", "[combat]") {
    HitInputs in = iron_sword_vs_iron_armor();
    in.armor.clear();
    in.attack_mult = 2.0F;
    const HitResult player = compute_hit(in, CombatSettings{});
    CHECK(player.final_damage == Approx(7.0 * 1.075 * 2.0));
    in.attacker_is_player = false;
    CHECK(compute_hit(in, CombatSettings{}).final_damage == Approx(14.0));
}

TEST_CASE("armor reduction stops at the cap and NPCs scale armor by fArmorRatingMax", "[combat]") {
    HitInputs in;
    in.weapon_damage = 10.0F;
    in.target_is_player = false;
    in.heavy_armor_skill = 100.0;
    for (int i = 0; i < 5; ++i) {
        in.armor.push_back({100.0F, 1}); // 250 each: far past the cap
    }
    const HitResult r = compute_hit(in, CombatSettings{});
    CHECK(r.armor_percent == Approx(80.0));
    CHECK(r.final_damage == Approx(2.0));
    in.armor.assign(1, {10.0F, 1});
    CHECK(compute_hit(in, CombatSettings{}).armor_rating == Approx(25.0)); // ceil(10 x 2.5)
}

TEST_CASE("unarmed damage, tempering and clothing", "[combat]") {
    HitInputs in;
    in.weapon_damage = 4.0F; // the race's unarmed damage
    in.attacker_is_player = false;
    CHECK(compute_hit(in, CombatSettings{}).final_damage == Approx(4.0));
    in.weapon_damage = 7.0F;
    in.temper_quality = 6; // Legendary: +20 x 0.5 for a weapon
    CHECK(compute_hit(in, CombatSettings{}).final_damage == Approx(17.0));
    in.armor.push_back({10.0F, 2}); // robes: rating counts, no piece bonus
    const HitResult r = compute_hit(in, CombatSettings{});
    CHECK(r.armor_percent == Approx(1.2));
}

TEST_CASE("tempering adds a bonus: weapons round to nearest, armor up, chest armor counts double", "[combat]") {
    CHECK(temper_bonus(1, 0.5F, false) == Approx(1.0F));
    CHECK(temper_bonus(2, 0.5F, false) == Approx(3.0F));
    CHECK(temper_bonus(6, 1.0F, true) == Approx(20.0F));
    CHECK(temper_bonus(1, 0.5F, true) == Approx(1.0F));
    CHECK(temper_bonus(2, 0.5F, true) == Approx(3.0F));
    CHECK(temper_bonus(0, 1.0F, true) == Approx(0.0F));
    HitInputs in;
    in.weapon_damage = 10.0F;
    in.armor.push_back({10.0F, 0, 6, true});
    in.light_armor_skill = 0.0;
    CHECK(compute_hit(in, CombatSettings{}).armor_rating == Approx(std::ceil(30.0 * static_cast<double>(CombatSettings{}.armor_rating_base))));
}

TEST_CASE("an arrow adds its damage to the bow's before the other factors", "[combat][damage]") {
    // A Hunting Bow (damage 7) with an Iron Arrow (damage 8), an NPC shooter and an unarmored target.
    HitInputs in;
    in.weapon_damage = 7.0F;
    in.ammo_damage = 8.0F;
    const auto r = compute_hit(in, CombatSettings{});
    CHECK(r.base == Approx(15.0F));
    CHECK(r.final_damage == Approx(15.0F));
    // The bow's tempering counts for the bow only.
    in.temper_quality = 5; // Epic: (3.6 x 5 - 1.6) x 0.5 = 8.2, rounded to 8
    CHECK(compute_hit(in, CombatSettings{}).base == Approx(7.0F + 8.0F + 8.0F));
    // The player's skill and the attack multiplier scale the sum.
    in.temper_quality = 0;
    in.attacker_is_player = true;
    in.weapon_skill = 100.0;
    in.attack_mult = 2.0F;
    CHECK(compute_hit(in, CombatSettings{}).weapon == Approx(15.0F * 1.5F * 2.0F));
}
