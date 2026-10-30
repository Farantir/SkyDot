// SPDX-License-Identifier: GPL-3.0-or-later
//
// The damage of one melee hit, from UESP Skyrim:Damage, Skyrim:Armor and
// Skyrim:Smithing and the game settings; every factor is returned. Nothing
// here touches Godot or the pack: the caller passes the settings and the
// facts in.
//
//   base   = weapon x tempering + ammo (arrows)
//   weapon = base x skill x skill x attack x difficulty x sneak
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

#include <cmath>
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

/// Tempering adds, it does not multiply (UESP Skyrim:Smithing): quality level 1 (Fine) to 6
/// (Legendary) gives 3.6 x ql - 1.6, times 1 for chest armor and 0.5 for weapons and other armor.
/// Weapons round to the nearest, armor up.
[[nodiscard]] inline float temper_bonus(int quality, float factor, bool round_up) {
    if (quality <= 0) {
        return 0.0F;
    }
    const float bonus = (3.6F * static_cast<float>(quality) - 1.6F) * factor;
    return round_up ? std::ceil(bonus - 0.001F) : std::round(bonus);
}

/// One worn piece of the target.
struct ArmorPiece {
    float rating = 0.0F;
    /// ARMO armor type: 0 light, 1 heavy, 2 clothing.
    std::uint8_t armor_type = 0;
    /// Its tempering quality level (0 untempered) and whether it is chest armor (body slot 32).
    int quality = 0;
    bool chest = false;
};

struct HitInputs {
    /// The weapon's damage; for an unarmed hit the race's unarmed damage.
    float weapon_damage = 0.0F;
    /// An arrow's or bolt's own damage, added to the bow's (UESP Skyrim:Damage: "arrow damage =
    /// bow damage + arrow damage"; the bow's tempering counts for the bow only); 0 for melee.
    float ammo_damage = 0.0F;
    /// Tempering quality level of the weapon, 0 untempered up to 6 (Legendary).
    int temper_quality = 0;
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
    float base = 0.0F;      ///< weapon_damage + tempering + ammo
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
