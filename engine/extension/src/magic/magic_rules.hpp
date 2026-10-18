// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pure rules of magic: what a spell costs, what a resistance takes off, how big an area is.
// Sources: UESP "Skyrim:Magic Overview" (Spell Cost: cost = base cost x skill multiplier x perk
// multiplier x equipment multiplier x dual cast multiplier; skill multiplier = 1 - (skill/400)^0.65;
// total Magic Resistance is capped at 85 %), the game settings the pack carries (fMagicPCSkillCostScale
// 0.65, fMagicCasterPCSkillCostBase 0.0025, fPlayerMaxResistance 85, fMagicAreaBaseCostMult 0) and the
// magicka costs stored in Skyrim.esm (fitted: see docs/adr/0008). Nothing here touches Godot.
#pragma once

#include "world_generated.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace skydot::magic {

namespace wfb = bethconv::pack::wfb;

/// The MGEF archetypes (DATA offset 64; UESP "Skyrim:Magic Effects" names).
namespace archetype {
inline constexpr std::uint32_t value_modifier = 0;
inline constexpr std::uint32_t script = 1;
inline constexpr std::uint32_t absorb = 4;
inline constexpr std::uint32_t dual_value_modifier = 5;
inline constexpr std::uint32_t peak_value_modifier = 34;
inline constexpr std::uint32_t count = 47;
} // namespace archetype

/// "ValueModifier" ... "VampireLord"; "Unknown<N>" beyond the table.
[[nodiscard]] std::string archetype_name(std::uint32_t archetype);

/// MGEF DATA flags (xEdit / UESP).
namespace mgef_flag {
inline constexpr std::uint32_t hostile = 0x1;
inline constexpr std::uint32_t recover = 0x2;
inline constexpr std::uint32_t detrimental = 0x4;
inline constexpr std::uint32_t no_duration = 0x200;
inline constexpr std::uint32_t no_death_dispel = 0x10000000;
} // namespace mgef_flag

/// MGEF / SPEL / ENCH casting types.
namespace casting {
inline constexpr std::uint32_t constant = 0;
inline constexpr std::uint32_t fire_and_forget = 1;
inline constexpr std::uint32_t concentration = 2;
inline constexpr std::uint32_t scroll = 3;
} // namespace casting

/// SPEL SPIT.
namespace spell_type {
inline constexpr std::uint32_t spell = 0;
inline constexpr std::uint32_t disease = 1;
inline constexpr std::uint32_t power = 2;
inline constexpr std::uint32_t lesser_power = 3;
inline constexpr std::uint32_t ability = 4;
inline constexpr std::uint32_t poison = 5;
inline constexpr std::uint32_t addiction = 10;
inline constexpr std::uint32_t voice = 11;
} // namespace spell_type
/// SPIT flag 0x1: the cost is the stored one (clear: the editor calculated it from the effects).
inline constexpr std::uint32_t k_spell_manual_cost = 0x1;

/// A game unit is 1.428 cm (k_metres_per_unit); the area of an effect is in feet.
inline constexpr double k_metres_per_foot = 0.3048;
inline constexpr double k_metres_per_unit = 0.0142875;
[[nodiscard]] constexpr double area_units(double feet) noexcept { return feet * (k_metres_per_foot / k_metres_per_unit); }

/// The game settings the rules read, with Skyrim.esm's values as defaults.
struct MagicSettings {
    double max_resistance = 85.0;       ///< fPlayerMaxResistance
    double skill_cost_exponent = 0.65;  ///< fMagicPCSkillCostScale
    double skill_cost_divisor = 400.0;  ///< 1 / fMagicCasterPCSkillCostBase
    double area_cost_mult = 0.0;        ///< fMagicAreaBaseCostMult
    double regen_delay = 0.5;
};

/// 1 - (skill/400)^0.65, from 1 (skill 0) towards 0.594 (skill 100), clamped to 0..1.
[[nodiscard]] double skill_cost_multiplier(double skill, const MagicSettings& s = {});

/// PLACEHOLDERS until P6 (perks): the perk, equipment and dual cast multipliers are 1.
inline constexpr double k_placeholder_perk_multiplier = 1.0;
inline constexpr double k_placeholder_dual_cast_scale = 1.0;

/// The cost the editor stores for one effect: base cost x (magnitude x duration factor)^1.1, with
/// the magnitude at least 1, the duration factor duration/10 for a timed fire-and-forget effect and 1
/// otherwise (instant, concentration: the cost is per second), times (1 + area x fMagicAreaBaseCostMult).
[[nodiscard]] double effect_cost(double base_cost, double magnitude, double duration, std::uint32_t casting_type,
                                 double area, const MagicSettings& s = {});

/// The cost a spell costs a caster with `skill` in its school: the stored SPIT cost (see adr/0008 for
/// why it is used also when the editor calculated it), x the skill multiplier x the placeholders.
[[nodiscard]] double spell_cost(const wfb::World& world, const wfb::Spell& spell, double skill,
                                const MagicSettings& s = {});
/// The cost summed from the effects (what the editor would store); for tests and the census.
[[nodiscard]] double auto_calc_cost(const wfb::World& world, const wfb::Spell& spell, const MagicSettings& s = {});
/// The actor value of the school a spell is cast with: the magic skill of its costliest effect, -1 if none.
[[nodiscard]] int spell_school(const wfb::World& world, const wfb::Spell& spell);

/// magnitude x (1 - resist/100), `resist` capped at `s.max_resistance` (and at 0 below).
[[nodiscard]] double resisted_magnitude(double magnitude, double resist, const MagicSettings& s = {});

} // namespace skydot::magic
