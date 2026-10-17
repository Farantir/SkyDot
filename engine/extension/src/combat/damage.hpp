// SPDX-License-Identifier: GPL-3.0-or-later
//
// The damage of one melee hit, from UESP Skyrim:Damage, Skyrim:Armor and
// Skyrim:Smithing and the game settings; every factor is returned. Nothing
// here touches Godot or the pack: the caller passes the settings and the
// facts in.
//
//   weapon = base x tempering x skill x attack x difficulty x sneak
//   skill  = player with a weapon skill: lerp(fDamagePCSkillMin,
//            fDamagePCSkillMax, skill / 100); otherwise 1
//   armor  = min(fMaxArmorRating, sum(ceil(rating x armor skill factor)) x
//            fArmorScalingFactor + fArmorBaseFactor x 100 x pieces)
//   final  = weapon x (1 - armor / 100)
//
// UESP Skyrim:Armor: "damage reduction % = displayed armor rating x 0.12 +
// 3 x pieces worn" (each worn piece but clothing has a hidden 25 rating =
// 3%), capped at 80%; an item's rating is ceil((base + quality) x (1 + 0.4 x
// skill / 100)) for the player and 1.5 instead of 0.4 for NPCs, which the
// GMSTs state as lerp(fArmorRatingBase 1, fArmorRatingPCMax 1.4 | fArmorRatingMax
// 2.5, skill / 100).
#pragma once

#include <cstdint>
#include <vector>

namespace skydot::combat {

/// The game settings the formula reads, with their values in Skyrim.esm.
struct CombatSettings {
    /// No GMST holds the minimum; 1 makes UESP's 1 + skill / 200.
    float damage_pc_skill_min = 1.0F;
    float damage_pc_skill_max = 1.5F;   ///< fDamagePCSkillMax.
    float armor_scaling_factor = 0.12F; ///< fArmorScalingFactor.
    float max_armor_rating = 80.0F;     ///< fMaxArmorRating.
    float armor_base_factor = 0.03F;    ///< fArmorBaseFactor.
    float armor_rating_base = 1.0F;     ///< fArmorRatingBase.
    float armor_rating_pc_max = 1.4F;   ///< fArmorRatingPCMax.
    float armor_rating_max = 2.5F;      ///< fArmorRatingMax.
    float clothing_armor_scale = 1.0F;  ///< fClothingArmorScale.
};

/// PLACEHOLDER (difficulty setting, P-later): Adept, no GMST holds it.
inline constexpr float k_placeholder_difficulty = 1.0F;
/// PLACEHOLDER (A8, sneaking): no sneak attack bonus yet.
inline constexpr float k_placeholder_sneak = 1.0F;

/// One worn piece of the target.
struct ArmorPiece {
    float rating = 0.0F;
    /// ARMO armor type: 0 light, 1 heavy, 2 clothing.
    std::uint8_t armor_type = 0;
};

struct HitInputs {
    /// The weapon's damage; for an unarmed hit the race's unarmed damage.
    float weapon_damage = 0.0F;
    /// Item health of the weapon (tempering), 1 untouched, up to 1.6.
    float temper_health = 1.0F;
    /// The attacker's skill with the weapon (0 to 100) and whether the player
    /// swings it (NPCs have no skill factor).
    double weapon_skill = 0.0;
    bool attacker_is_player = false;
    /// The ATKD damage mult of the attack event.
    float attack_mult = 1.0F;
    float difficulty = k_placeholder_difficulty;
    float sneak = k_placeholder_sneak;

    std::vector<ArmorPiece> armor;
    /// The target's LightArmor and HeavyArmor skills.
    double light_armor_skill = 0.0;
    double heavy_armor_skill = 0.0;
    bool target_is_player = false;
};

struct HitResult {
    float base = 0.0F;      ///< weapon_damage x temper_health
    float skill = 1.0F;
    float attack = 1.0F;
    float difficulty = 1.0F;
    float sneak = 1.0F;
    float weapon = 0.0F;    ///< Before armor.
    float armor_rating = 0.0F;
    float armor_percent = 0.0F;
    float final_damage = 0.0F;
};

[[nodiscard]] HitResult compute_hit(const HitInputs& in, const CombatSettings& settings);

} // namespace skydot::combat
