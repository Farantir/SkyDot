// SPDX-License-Identifier: GPL-3.0-or-later
#include "combat/damage.hpp"

#include <algorithm>
#include <cmath>

namespace skydot::combat {
namespace {

double lerp(double a, double b, double t) { return a + (b - a) * std::clamp(t, 0.0, 1.0); }

} // namespace

HitResult compute_hit(const HitInputs& in, const CombatSettings& s) {
    HitResult r;
    r.base = in.weapon_damage * in.temper_health + in.ammo_damage;
    r.skill = in.attacker_is_player
                  ? static_cast<float>(lerp(static_cast<double>(s.damage_pc_skill_min),
                                            static_cast<double>(s.damage_pc_skill_max), in.weapon_skill / 100.0))
                  : 1.0F;
    r.attack = in.attack_mult;
    r.difficulty = in.difficulty;
    r.sneak = in.sneak;
    r.weapon = r.base * r.skill * r.attack * r.difficulty * r.sneak;

    double rating = 0.0;
    int pieces = 0;
    for (const ArmorPiece& piece : in.armor) {
        if (piece.armor_type == 2) {
            // Clothing: its rating at fClothingArmorScale, no skill and no
            // hidden per-piece bonus.
            rating += static_cast<double>(piece.rating) * static_cast<double>(s.clothing_armor_scale);
            continue;
        }
        const double skill = piece.armor_type == 1 ? in.heavy_armor_skill : in.light_armor_skill;
        const double top = static_cast<double>(in.target_is_player ? s.armor_rating_pc_max : s.armor_rating_max);
        const double factor = lerp(static_cast<double>(s.armor_rating_base), top, skill / 100.0);
        rating += std::ceil(static_cast<double>(piece.rating) * factor);
        ++pieces;
    }
    r.armor_rating = static_cast<float>(rating);
    const double percent = rating * static_cast<double>(s.armor_scaling_factor) +
                           static_cast<double>(s.armor_base_factor) * 100.0 * pieces;
    r.armor_percent = static_cast<float>(std::min(static_cast<double>(s.max_armor_rating), percent));
    r.final_damage = r.weapon * (1.0F - r.armor_percent / 100.0F);
    return r;
}

} // namespace skydot::combat
