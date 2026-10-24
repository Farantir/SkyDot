// SPDX-License-Identifier: GPL-3.0-or-later
//
// Casting (GAMEPLAY-PLAN 7a-3, docs/adr/0008): what the game layer needs to cast a spell or a
// scroll (`cast_info`), paying a spell apart from landing its effects (`pay_spell`, `land_form`:
// an aimed spell is paid at the release and lands at the impact, an area spell is paid once and
// lands on every actor), and the state of a hand that holds a spell (`HandCast`). Sources: UESP
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

// ---- the hand ---------------------------------------------------------------------------------

/// What a hand holds, as `HandCast` needs it.
struct HandSpell {
    std::uint32_t casting_type = casting::fire_and_forget;
    double charge_time = 0.0;
};

/// One step of a hand: what happened.
struct HandEvent {
    enum class Kind : std::uint8_t {
        none,    ///< Nothing (idle, charging, released too early).
        charged, ///< Fire and forget: the casting time is full; releasing now casts.
        cast,    ///< Fire and forget released after the time: pay and land the spell.
        tick,    ///< Concentration: `seconds` were paid for; land the effects again.
        stop,    ///< Concentration ended (released, or the payer had no magicka).
    };
    Kind kind = Kind::none;
    double seconds = 0.0; ///< tick: the seconds paid; cast: the seconds held.
    [[nodiscard]] const char* name() const noexcept;
};

/// The attack button on one hand that holds a spell: idle -> charging (fire and forget; `charge_time`
/// seconds while held) -> released (event cast) or idle (released early: nothing, no cost);
/// concentration: active from the press, each `step` pays for the time and ticks, until released or
/// the payer says no.
class HandCast {
public:
    enum class Phase : std::uint8_t { idle, charging, concentrating };

    /// The payer: pays for `seconds` of concentration, false if the caster cannot.
    using Payer = std::function<bool(double seconds)>;

    void press(const HandSpell& spell);
    HandEvent step(double dt, const Payer& pay);
    HandEvent release();
    /// Back to idle without an event (the spell was put away, the caster died).
    void cancel() { phase_ = Phase::idle; held_ = 0.0; }

    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] double held() const noexcept { return held_; }
    /// Fire and forget: how much of the casting time has passed, 0..1 (1 when it is 0 seconds).
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] bool charged() const noexcept { return phase_ == Phase::charging && held_ >= spell_.charge_time; }

private:
    Phase phase_ = Phase::idle;
    HandSpell spell_;
    double held_ = 0.0;
    bool announced_ = false;
};

} // namespace skydot::magic
