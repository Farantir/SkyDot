// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a mage does in a fight (GAMEPLAY-PLAN.md, 7a-5), without Godot, the pack or the clock: which
// of the spells it knows to cast, or whether to fight with its weapons. SkydotAi gathers the facts
// (its spells' cast info, its Magicka and Health, the distance, what it already has on itself) and
// acts on the answer.
//
// Sources: UESP "Skyrim:Combat" and the CK's combat style (CSTY: the magic, melee and ranged
// equipment score multipliers say what an NPC prefers to fight with). Everything the sources do not
// give is a PLACEHOLDER, named below.
#pragma once

#include <cstdint>
#include <vector>

namespace skydot::combat_ai {

/// What a spell is for, as an NPC uses it.
enum class SpellRole : std::uint8_t {
    other,   ///< Not used by the AI (powers, abilities, runes, a spell without a known effect...).
    attack,  ///< Hostile, touch or aimed or at an actor: cast at the enemy.
    heal,    ///< Restore Health on itself (a Health value modifier that is not undone at the end).
    summon,  ///< Summon creature on itself.
    cloak,   ///< Cloak archetype on itself.
    fortify, ///< A beneficial value modifier on itself that is no healing (armor spells, Fortify ...).
};

/// What the rules need to know of one known spell.
struct SpellFacts {
    std::uint32_t form{};
    bool plain = true;            ///< A spell (SPIT type 0), not a power, ability, poison, ...
    double cost{};                ///< Magicka: a fire-and-forget spell once, a concentration spell per second.
    bool concentration{};
    std::uint32_t delivery{};     ///< 0 self, 1 touch, 2 aimed, 3 target actor, 4 target location.
    double reach{};               ///< Game units it can hit from (touch, aimed: the projectile's range).
    bool hostile{};
    bool recover{};               ///< MGEF flag Recover: the change is undone when the effect ends (Fortify...); clear for Restore Health.
    std::uint32_t archetype{};    ///< Of the costliest effect (magic_rules.hpp `archetype`).
    int actor_value{-1};          ///< Of the costliest effect, -1 if none (vm/actor_values.hpp).
    bool active{};                ///< Its effects run on the caster now.
};

/// The role of a spell: attack for hostile touch, aimed or target-actor delivery; heal for a
/// non-hostile self value modifier of Health without the Recover flag (Restore Health; with the flag it is Fortify Health); summon (archetype 18; self or target location delivery), cloak (35) and fortify (a
/// non-hostile self value modifier of another actor value, or of Health with the Recover flag)
/// on the caster. Wards have no archetype of their own in the engine yet: they are `other`.
[[nodiscard]] SpellRole classify(const SpellFacts& spell) noexcept;

/// The combat style's score multipliers for what the NPC fights with (CSTY CSGD).
struct WeaponStyle {
    double magic = 0.5;
    double melee = 0.5;
    double ranged = 0.5;
    double unarmed = 0.5; ///< Fists and a creature's own attacks: always there to fight with.
    double shout = 0.0;   ///< CSTY shout score mult: how often it shouts; 0 (no combat style too) never.
};

/// PLACEHOLDERS (UESP gives no numbers).
/// It heals itself below this fraction of its Health (50 %).
inline constexpr double k_heal_below = 0.5;
/// A concentration healing spell is kept until the Health is above this fraction (90 %).
inline constexpr double k_healed_above = 0.9;
/// A concentration attack spell is held for this long, drawn from the die (seconds).
inline constexpr double k_hold_min = 1.0;
inline constexpr double k_hold_max = 3.0;
/// A buff (summon, fortify, cloak) is cast only when it leaves this fraction of the maximum Magicka.
inline constexpr double k_buff_reserve = 0.25;
/// A cloak is cast when the enemy is within this many game units.
inline constexpr double k_cloak_distance = 600.0;
/// An aimed spell is cast from where the enemy is at least this far away; closer, it backs off while
/// it waits for the next cast (as a bow does, combat_rules.hpp RangedInputs::min_distance).
inline constexpr double k_mage_min_distance = 400.0;
/// It closes in to this fraction of a spell's reach before casting it.
inline constexpr double k_reach_fraction = 0.8;

struct MageInputs {
    std::vector<SpellFacts> spells; ///< The spells it knows, in order.
    double magicka{};
    double magicka_max{};
    double health_fraction = 1.0;
    double distance{};              ///< To the target, game units.
    bool line_of_sight = true;
    bool ready = true;              ///< The last cast is over.
    bool has_melee{};               ///< It carries a melee weapon.
    bool has_bow{};
    WeaponStyle style;
    double roll{};                  ///< A die in [0, 1).
};

enum class MageAction : std::uint8_t {
    weapons,       ///< Fight with the weapons (the melee and ranged rules).
    cast,          ///< Cast `spell` now.
    approach,      ///< Close in to cast `spell`.
    keep_distance, ///< Back off while waiting for the next cast.
    wait,          ///< Stand and face the target while the last cast recovers.
};

struct MageDecision {
    MageAction action = MageAction::weapons;
    SpellRole role = SpellRole::other;
    std::uint32_t spell{};
    double hold{}; ///< A concentration spell is held for this many seconds (0: a fire-and-forget spell).
};

/// Whether the fight is fought with spells: the style's magic multiplier is above 0 and above those of
/// the unarmed attacks and of the weapons it carries (a tie goes to the weapons).
[[nodiscard]] bool prefers_magic(const MageInputs& in) noexcept;

/// What to do now: heal below k_heal_below (the first affordable healing spell), else cast a buff it
/// does not have on (summon, fortify, cloak when the enemy is near) while it keeps k_buff_reserve of
/// its Magicka, else, if it prefers magic, an attack spell chosen with the die from those in reach
/// it can afford (approach the longest reach if none is in reach), else the weapons. Not ready: it
/// waits (or keeps its distance), only the weapons go on.
[[nodiscard]] MageDecision decide(const MageInputs& in) noexcept;

/// How long a concentration spell is held: the die between k_hold_min and k_hold_max, no longer than
/// the Magicka lasts (at least 0.1 s).
[[nodiscard]] double hold_seconds(double roll, double cost_per_second, double magicka) noexcept;

// ---- shouts (7b-4) ------------------------------------------------------------------------

/// What the rules need to know of one shout an NPC knows: it says all three words (7b-2), so the
/// spell is that of the last word and the recovery the whole shout's.
struct ShoutFacts {
    std::uint32_t form{};      ///< The SHOU.
    std::uint32_t spell{};     ///< Of the last word.
    double reach{};            ///< Game units the spell can hit from (its projectile's range, a cone's).
    double recovery{};         ///< Seconds the voice recovers after it.
    bool hostile{};
};

/// PLACEHOLDER (the CK documents the shout mult as a score multiplier, not a rate): the chance, per
/// decision, that an NPC that can shout does is its style's shout mult (a mult of 1 or more: always).
/// A shout is only said at a target within this fraction of its reach (k_reach_fraction).
struct ShoutInputs {
    std::vector<ShoutFacts> shouts;
    bool recovering{};       ///< The voice still recovers from the last shout.
    bool aimed{};            ///< It has faced the target for fAIShoutMinAimSeconds within fAIShoutToleranceDegrees.
    bool line_of_sight = true;
    double distance{};       ///< To the target, game units.
    double shout_mult{};     ///< The combat style's shout score mult.
    double roll{};           ///< A die in [0, 1).
};

/// Whether the NPC has faced its target long enough: `aimed_seconds` is how long the angle between
/// where it looks and the target has stayed within the tolerance (the caller resets it when it leaves
/// the tolerance).
[[nodiscard]] bool shout_aimed(double aimed_seconds, double min_aim_seconds) noexcept;

/// The shout to say now, 0 for none: not recovering, aimed, in sight, the style's mult above 0, the
/// die under the mult, and a hostile shout whose reach holds the target (k_reach_fraction of it); the
/// same die, rescaled, picks among several.
[[nodiscard]] std::uint32_t decide_shout(const ShoutInputs& in) noexcept;

} // namespace skydot::combat_ai
