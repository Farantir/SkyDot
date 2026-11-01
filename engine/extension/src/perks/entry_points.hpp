// SPDX-License-Identifier: GPL-3.0-or-later
//
// The perk entry point engine (ARCHITECTURE.md section 7, "Perks"): it applies the entry-point effects
// that an actor's perks carry, the way the condition evaluator answers a condition list. A perk effect
// (PERK PRKE block, world.fb 38) names an entry point (a value the game computes: damage, a spell's cost,
// a price), a function (set, add, multiply ... the value) and condition tabs; the game asks `apply` at the
// entry point with the value it computed and gets the changed value. Nothing here touches Godot.
//
// - The effects of the owner's perks (`PerksOf`: perk, rank) at the entry point are collected when the
//   effect's rank is the perk's rank (an effect of rank 0 belongs to rank 1; ranks chained as separate PERK
//   records each have effects of rank 0) and every condition tab passes.
// - Tab t is evaluated with `EntryContext::subjects[t]` as the condition subject: tab 0 is the owner, the
//   next tabs the weapon base, target reference, spell, item ... the entry point's caller names.
//   A subject can be a base form; HasKeyword and the EP* functions answer for it (EntryOverrides).
// - The collected effects apply in ascending priority; ties by perk id, then record order.
// - Add Leveled List and Add Activate Choice are listed (`matching`) but nothing uses them yet.
#pragma once

#include "conditions/conditions.hpp"
#include "world_generated.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skydot::perks {

namespace wfb = bethconv::pack::wfb;

/// xEdit's wbEntryPointsEnum (the CK wiki's list), checked against PerkSkillBoosts.
enum class EntryPoint : std::uint8_t {
    CalculateWeaponDamage = 0,
    CalcMyCritChance = 1,
    CalcMyCritDamage = 2,
    MineExplodeChance = 3,
    AdjustLimbDamage = 4,
    AdjustBookSkillPoints = 5,
    ModRecoveredHealth = 6,
    GetShouldAttack = 7,
    ModBuyPrices = 8,
    AddLeveledListOnDeath = 9,
    GetMaxCarryWeight = 10,
    AddictionChance = 11,
    AddictionDuration = 12,
    PositiveChemDuration = 13,
    Activate = 14,
    IgnoreRunningDuringDetection = 15,
    IgnoreBrokenLock = 16,
    ModEnemyCritChance = 17,
    ModSneakAttackMult = 18,
    MaxPlaceableMines = 19,
    BowZoom = 20,
    RecoverArrowChance = 21,
    ModSkillUse = 22,
    TkDistance = 23,
    TkDamageMult = 24,
    TkDamage = 25,
    ModBashingDamage = 26,
    ModPowerAttackStamina = 27,
    ModPowerAttackDamage = 28,
    ModSpellMagnitude = 29,
    ModSpellDuration = 30,
    SecondaryValueWeight = 31,
    ModArmorWeight = 32,
    ModIncomingStagger = 33,
    ModTargetStagger = 34,
    ModAttackDamage = 35,
    ModIncomingDamage = 36,
    ModTargetDamageResistance = 37,
    ModSpellCost = 38,
    ModPercentBlocked = 39,
    ShieldDeflectArrow = 40,
    ModIncomingSpellMagnitude = 41,
    ModIncomingSpellDuration = 42,
    Intimidation = 43,
    Reputation = 44,
    FavorPoints = 45,
    BribeAmount = 46,
    ModDetectionLight = 47,
    ModDetectionMovement = 48,
    ModSoulGemRecharge = 49,
    SetSweepAttack = 50,
    ApplyCombatHitSpell = 51,
    ApplyBashingSpell = 52,
    ApplyReanimateSpell = 53,
    SetBooleanGraphVariable = 54,
    SpellCastingSoundEvent = 55,
    ModPickpocketChance = 56,
    ModDetectionSneakSkill = 57,
    FallingDamage = 58,
    ModLockpickSweetSpot = 59,
    ModSellPrices = 60,
    CanPickpocketEquippedItem = 61,
    LockpickLevelAllowed = 62,
    LockpickStartingArc = 63,
    SetProgressionPicking = 64,
    LockpicksUnbreakable = 65,
    ModAlchemyEffectiveness = 66,
    ApplyWeaponSwingSpell = 67,
    CommandedActorLimit = 68,
    ApplySneakingSpell = 69,
    PlayerMagicSlowdown = 70,
    WardMagickaAbsorption = 71,
    InitialIngredientEffectsLearned = 72,
    PurifyAlchemyIngredients = 73,
    FilterActivation = 74,
    CanDualCastSpell = 75,
    ModTemperingHealth = 76,
    ModEnchantmentPower = 77,
    SoulPctCapturedToWeapon = 78,
    ModSoulGemEnchanting = 79,
    ModAppliedEnchantmentsAllowed = 80,
    SetActivateLabel = 81,
    ModShoutOk = 82,
    ModPoisonDoseCount = 83,
    ShouldApplyPlacedItem = 84,
    ModArmorRating = 85,
    LockpickingCrimeChance = 86,
    IngredientsHarvested = 87,
    SpellRangeTargetLoc = 88,
    PotionsCreated = 89,
    LockpickingKeyRewardChance = 90,
    AllowMountActor = 91,
};
inline constexpr std::size_t k_entry_point_count = 92;

/// The CK name of an entry point ("" for an id beyond the list).
[[nodiscard]] const char* entry_point_name(EntryPoint ep);

/// PRKE function ids.
enum class Function : std::uint8_t {
    SetValue = 1,
    AddValue = 2,
    MultiplyValue = 3,
    AddRangeToValue = 4,
    AddActorValueMult = 5,
    AbsoluteValue = 6,
    NegativeAbsoluteValue = 7,
    AddLeveledList = 8,
    AddActivateChoice = 9,
    SelectSpell = 10,
    SelectText = 11,
    SetToActorValueMult = 12,
    MultiplyActorValueMult = 13,
    MultiplyOnePlusActorValueMult = 14,
    SetText = 15,
};

/// What an entry point's caller tells about the moment. Tab t of an effect is evaluated with
/// `subjects[t]` as the condition subject (`subjects[0]` is the owner; 0 there means `owner`).
struct EntryContext {
    std::uint32_t owner = 0;
    std::array<std::uint32_t, 3> subjects{};
    /// ModSkillUse (22): the skill (actor value index) being advanced, the action id, and the object used
    /// (EPMagic_IsAdvanceSkill, EPModSkillUsage_IsAdvanceAction, ..._AdvanceObjectHasKeyword); -1 / 0: none.
    std::int32_t advance_skill = -1;
    std::int32_t advance_action = -1;
    std::uint32_t advance_object = 0;
    /// The armor bases the owner wears (WornHasKeyword, WornApparelHasKeywordCount).
    std::vector<std::uint32_t> worn;
    /// The keywords of the attack being made (IsAttackType).
    std::vector<std::uint32_t> attack_keywords;
    /// Tempering (76): the item is enchanted (EPTemperingItemIsEnchanted). Alchemy (66): a poison is made.
    bool item_enchanted = false;
    bool making_poison = false;
};

/// An effect found at an entry point.
struct Match {
    std::uint32_t perk = 0;
    std::uint8_t rank = 0;      ///< the owner's rank of the perk
    std::uint8_t priority = 0;
    std::uint32_t order = 0;    ///< record order within the perk
    const wfb::PerkEffect* effect = nullptr;
};

/// (perk, rank) pairs of an actor reference, any order.
using PerksOf = std::function<std::vector<std::pair<std::uint32_t, std::uint8_t>>(std::uint32_t ref)>;

class EntryPoints {
public:
    EntryPoints(const wfb::World& world, conditions::ConditionWorld& cw, PerksOf perks_of)
        : world_(world), cw_(cw), perks_of_(std::move(perks_of)) {}

    /// The effects of the owner's perks at `ep` whose condition tabs all pass, in application order.
    [[nodiscard]] std::vector<Match> matching(EntryPoint ep, const EntryContext& ctx) const;

    /// `value` after the value functions (1 to 7, 12 to 14) of `matching`. An actor value in 5 and
    /// 12 to 14 is the owner's current value (ConditionWorld::actor_value).
    [[nodiscard]] double apply(EntryPoint ep, const EntryContext& ctx, double value) const;

    /// The spells of Select Spell (10) effects, in application order.
    [[nodiscard]] std::vector<std::uint32_t> select_spells(EntryPoint ep, const EntryContext& ctx) const;

    /// The texts of Select Text (11) and Set Text (15) effects, in application order.
    [[nodiscard]] std::vector<std::string> select_text(EntryPoint ep, const EntryContext& ctx) const;

    /// One value function on `value` with the effect's data and the owner's actor value `av` (see apply).
    [[nodiscard]] static double combine(Function fn, double value, const std::vector<float>& data, double av);

private:
    const wfb::World& world_;
    conditions::ConditionWorld& cw_;
    PerksOf perks_of_;
};

/// What adding or removing a perk does to the world (kind ability and quest effects).
class EffectSink {
public:
    virtual ~EffectSink() = default;
    virtual void add_ability(std::uint32_t actor, std::uint32_t spell) = 0;
    virtual void remove_ability(std::uint32_t actor, std::uint32_t spell) = 0;
    virtual void set_stage(std::uint32_t quest, std::uint16_t stage) = 0;
};

/// `actor` gained `perk` at `rank`: its ability effects of that rank are added and its quest effects set.
void perk_added(const wfb::World& world, std::uint32_t actor, std::uint32_t perk, EffectSink& sink);
/// `actor` lost `perk`: its abilities are removed.
void perk_removed(const wfb::World& world, std::uint32_t actor, std::uint32_t perk, EffectSink& sink);

} // namespace skydot::perks
