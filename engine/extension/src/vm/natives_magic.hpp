// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Papyrus natives of magic: Spell, MagicEffect, ActiveMagicEffect and the spell-related
// Actor functions. They call the game through `MagicApi`, which SkydotPapyrus implements over
// magic/ (vm/papyrus_magic.cpp) and tests implement with a fake. Nothing here touches Godot.
#pragma once

#include "vm/vm.hpp"

#include "world_generated.h"

#include <cstdint>
#include <optional>

namespace skydot::vm {

class MagicApi {
public:
    virtual ~MagicApi() = default;

    // ---- spells
    /// Cast `spell` from `caster` (blamed: `blame`, 0 for the caster) on `target`, at no cost, as a
    /// script does (Spell.Cast, RemoteCast, DoCombatSpellApply). A target of 0 is the caster. False
    /// if there is no such spell or no one to cast it.
    virtual bool cast(std::uint32_t spell, std::uint32_t caster, std::uint32_t blame, std::uint32_t target) = 0;
    /// The magicka cost of `spell`: the stored cost with `caster` 0, else what it costs that caster.
    virtual double spell_cost(std::uint32_t spell, std::uint32_t caster) = 0;

    // ---- actors
    virtual bool add_spell(std::uint32_t actor, std::uint32_t spell, bool verbose) = 0;
    virtual bool remove_spell(std::uint32_t actor, std::uint32_t spell) = 0;
    virtual bool has_spell(std::uint32_t actor, std::uint32_t spell) = 0;
    /// End what `spell` put on `actor`; whether anything ended.
    virtual bool dispel_spell(std::uint32_t actor, std::uint32_t spell) = 0;
    virtual void dispel_all_spells(std::uint32_t actor) = 0;
    virtual bool has_magic_effect(std::uint32_t actor, std::uint32_t mgef) = 0;
    virtual bool has_magic_effect_keyword(std::uint32_t actor, std::uint32_t keyword) = 0;
    /// `hand`: 0 left, 1 right, 2 the power (Papyrus' aiSource 0, 1 and 2).
    virtual bool equip_spell(std::uint32_t actor, std::uint32_t spell, int hand) = 0;
    virtual void unequip_spell(std::uint32_t actor, int hand) = 0;
    [[nodiscard]] virtual std::uint32_t equipped_spell(std::uint32_t actor, int hand) = 0;

    // ---- the object of one running effect (its handle is the instance id)
    struct Effect {
        std::uint32_t mgef = 0;
        std::uint32_t target = 0;
        std::uint32_t caster = 0;
        double magnitude = 0.0;
        double duration = 0.0;
        double elapsed = 0.0;
    };
    /// Running or just finished (its OnEffectFinish is being sent).
    [[nodiscard]] virtual std::optional<Effect> effect(std::uint32_t handle) = 0;
    virtual void dispel_effect(std::uint32_t handle) = 0;
};

/// Spell.Cast, RemoteCast, GetMagickaCost, GetEffectiveMagickaCost, GetCastTime, IsHostile,
/// GetNumEffects, GetNthEffectMagicEffect, GetNthEffectMagnitude, GetNthEffectArea,
/// GetNthEffectDuration, GetPerk.
void bind_spell_natives(Vm& vm, const bethconv::pack::wfb::World& world, MagicApi& api);
/// MagicEffect.GetAssociatedSkill, GetResistance, GetBaseCost, GetCastTime, GetCastingType,
/// GetDeliveryType, GetSkillLevel, GetSkillUsageMult, GetPerk, GetProjectile, GetExplosion,
/// GetLight, GetCastingArt, GetHitEffectArt, GetEnchantArt, GetEquipAbility, GetImpactDataSet,
/// GetHitShader, GetEnchantShader, GetNumCounterEffects, GetNthCounterEffect, IsEffectFlagSet.
void bind_magiceffect_natives(Vm& vm, const bethconv::pack::wfb::World& world);
/// ActiveMagicEffect.GetTargetActor, GetCasterActor, GetBaseObject, Dispel, GetMagnitude,
/// GetDuration, GetTimeElapsed.
void bind_activemagiceffect_natives(Vm& vm, MagicApi& api);
/// Actor.AddSpell, RemoveSpell, HasSpell, DispelSpell, DispelAllSpells, HasMagicEffect,
/// HasMagicEffectWithKeyword, DoCombatSpellApply, EquipSpell, UnequipSpell, GetEquippedSpell.
void bind_actor_magic_natives(Vm& vm, MagicApi& api);

} // namespace skydot::vm
