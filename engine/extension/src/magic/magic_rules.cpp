// SPDX-License-Identifier: GPL-3.0-or-later
#include "magic/magic_rules.hpp"

#include "data/fb_search.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace skydot::magic {

namespace {

constexpr std::array<std::string_view, archetype::count> k_names = {
    "ValueModifier", "Script", "Dispel", "CureDisease", "Absorb", "DualValueModifier", "Calm", "Demoralize",
    "Frenzy", "Disarm", "CommandSummoned", "Invisibility", "Light", "Unknown13", "Unknown14", "Lock", "Open",
    "BoundWeapon", "SummonCreature", "DetectLife", "Telekinesis", "Paralysis", "Reanimate", "SoulTrap",
    "TurnUndead", "Guide", "WerewolfFeed", "CureParalysis", "CureAddiction", "CurePoison", "Concussion",
    "ValueAndParts", "AccumulateMagnitude", "Stagger", "PeakValueModifier", "Cloak", "Werewolf", "SlowTime",
    "Rally", "EnhanceWeapon", "SpawnHazard", "Etherealize", "Banish", "SpawnScriptedRef", "Disguise",
    "GrabActor", "VampireLord"};

} // namespace

std::string archetype_name(std::uint32_t archetype) {
    if (archetype < k_names.size()) {
        return std::string(k_names[archetype]);
    }
    return "Unknown" + std::to_string(archetype);
}

double skill_cost_multiplier(double skill, const MagicSettings& s) {
    const double ratio = std::clamp(skill, 0.0, 1000.0) / s.skill_cost_divisor;
    return std::clamp(1.0 - std::pow(ratio, s.skill_cost_exponent), 0.0, 1.0);
}

double effect_cost(double base_cost, double magnitude, double duration, std::uint32_t casting_type, double area,
                   const MagicSettings& s) {
    const double mag = std::max(magnitude, 1.0);
    const double dur_factor =
        (duration > 0.0 && casting_type != casting::concentration) ? duration / 10.0 : 1.0;
    return base_cost * std::pow(mag * dur_factor, 1.1) * (1.0 + area * s.area_cost_mult);
}

double auto_calc_cost(const wfb::World& world, const wfb::Spell& spell, const MagicSettings& s) {
    double sum = 0.0;
    if (spell.effects() == nullptr) {
        return sum;
    }
    for (const auto* e : *spell.effects()) {
        const auto* m = lookup(world.magic_effects(), e->effect());
        if (m == nullptr) {
            continue;
        }
        sum += effect_cost(m->base_cost(), e->magnitude(), e->duration(), spell.casting_type(), e->area(), s);
    }
    return sum;
}

int spell_school(const wfb::World& world, const wfb::Spell& spell) {
    int school = -1;
    double best = -1.0;
    if (spell.effects() == nullptr) {
        return school;
    }
    for (const auto* e : *spell.effects()) {
        const auto* m = lookup(world.magic_effects(), e->effect());
        if (m == nullptr) {
            continue;
        }
        const double cost = effect_cost(m->base_cost(), e->magnitude(), e->duration(), spell.casting_type(), 0.0);
        if (cost > best && m->magic_skill() >= 0) {
            best = cost;
            school = m->magic_skill();
        }
    }
    return school;
}

double spell_cost(const wfb::World& world, const wfb::Spell& spell, double skill, const MagicSettings& s) {
    // The editor stores the calculated cost in SPIT also for spells without the manual flag, and a
    // later plugin that changes an effect's base cost does not rewrite it (Dragonborn raises Flames'
    // fire effect from 1.5 to 2.5; Flames stays at the stored 14, as UESP lists it).
    double stored = static_cast<double>(spell.cost());
    if (stored == 0.0 && (spell.flags() & k_spell_manual_cost) == 0) {
        stored = std::floor(auto_calc_cost(world, spell, s)); // a record that stored nothing
    }
    return stored * skill_cost_multiplier(skill, s) * k_placeholder_perk_multiplier;
}

double resisted_magnitude(double magnitude, double resist, const MagicSettings& s) {
    const double r = std::clamp(resist, 0.0, s.max_resistance);
    return magnitude * (1.0 - r / 100.0);
}

} // namespace skydot::magic
