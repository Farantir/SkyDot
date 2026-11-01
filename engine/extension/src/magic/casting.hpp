// SPDX-License-Identifier: GPL-3.0-or-later
//
// Casting (GAMEPLAY-PLAN 7a-3, docs/adr/0008): what the game layer needs to cast a spell or a
// scroll (`cast_info`), paying a spell apart from landing its effects (`pay_spell`, `land_form`:
// an aimed spell is paid at the release and lands at the impact, an area spell is paid once and
// lands on every actor). The state of a hand is Casting's (game/casting.gd). Sources: UESP
// "Skyrim:Magic Overview" (fire and forget: held for the casting time, released to cast;
// concentration: magicka per second while held; scrolls are fire and forget and cost no magicka).
// Nothing here touches Godot.
#pragma once

#include "magic/active_effects.hpp"
#include "magic/magic_rules.hpp"
#include "magic/spell_lists.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skydot::magic {

/// What the game layer needs to cast `form` (a SPEL or a scroll item).
struct CastInfo {
    bool found = false;
    bool is_scroll = false;
    std::uint32_t form = 0;
    std::string name;
    std::uint32_t type = 0;         ///< SPIT type (a scroll: 0).
    std::uint32_t casting_type = 0; ///< A scroll or a SPEL of type scroll: fire and forget.
    std::uint32_t delivery = 0;     ///< A scroll: its primary effect's MGEF.
    float charge_time = 0.0F;       ///< Seconds held before a fire-and-forget spell can be released (a scroll: 0).
    float cast_duration = 0.0F;
    float range = 0.0F;
    std::size_t effects = 0;
    /// The costliest ("primary") effect and its MGEF; mgef 0 if the spell has no known effect.
    std::uint32_t mgef = 0;
    float magnitude = 0.0F;
    std::uint32_t duration = 0;
    std::uint32_t area_feet = 0;
    double area_units = 0.0;
    std::uint32_t projectile = 0;
    std::uint32_t explosion = 0;
    std::uint32_t casting_art = 0;
    std::uint32_t hit_art = 0;
    std::uint32_t casting_light = 0;
    bool hostile = false;
    bool recover = false;           ///< MGEF flag Recover of the costliest effect: its change is undone when it ends (Fortify...).
    std::uint32_t archetype = 0;    ///< Of the costliest effect (archetype::).
    int actor_value = -1;           ///< Its actor value, -1 if none.
    int school = -1; ///< The actor value of the magic skill, -1 if none.
};

[[nodiscard]] CastInfo cast_info(const wfb::World& world, std::uint32_t form, const MagicSettings& s = {});

/// The effects of `form` (a SPEL, or the scroll item) as inputs to `ActiveEffects::apply`, with the
/// kind of source they have; false if `form` is neither.
bool effects_of_castable(const wfb::World& world, std::uint32_t form, std::vector<EffectInput>& inputs,
                         SourceKind& kind);

/// What `caster` pays for `spell` once: the stored cost x the skill multiplier, then the caster's
/// Mod Spell Cost perks (38; subjects [caster, spell]), at least 0. A concentration spell's is per second.
[[nodiscard]] double perk_spell_cost(const wfb::World& world, const MagicHost& host, std::uint32_t caster,
                                     const wfb::Spell& spell, const MagicSettings& s = {});

struct Payment {
    bool ok = false;
    std::string reason; ///< "", "no such spell", "cooldown", "not enough magicka"
    double cost = 0.0;
};

/// Pay for casting `spell` as `caster` (`seconds` of a concentration spell, one second if 0): the
/// cooldown of a power, then the magicka (damaged through the host so the regeneration waits), then
/// the power's use is noted at `game_days`. A SPEL only; scrolls cost nothing and are not paid.
Payment pay_spell(const wfb::World& world, MagicHost& host, SpellBook& book, std::uint32_t caster,
                  std::uint32_t spell, double seconds, double game_days, const MagicSettings& s = {});

struct Landing {
    bool ok = false;
    std::string reason; ///< "", "no such spell"
    std::vector<std::uint32_t> instances;
};

/// Put the effects of `form` (a SPEL or a scroll item) on `target` from `caster`, without cost.
/// Target 0 and spells of self delivery land on the caster.
Landing land_form(const wfb::World& world, ActiveEffects& effects, std::uint32_t caster, std::uint32_t form,
                  std::uint32_t target);

} // namespace skydot::magic
