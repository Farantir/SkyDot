// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyrim's fixed actor value index list (0 Aggression ... 163 ReflectDamage)
// and the names Papyrus passes. Source: CommonLibSSE's RE::ActorValue and
// UESP "Skyrim:Actor Value Indices". Nothing here touches Godot.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace skydot::vm::av {

constexpr std::size_t k_count = 164;

constexpr std::uint32_t k_aggression = 0;
constexpr std::uint32_t k_assistance = 5;
constexpr std::uint32_t k_first_skill = 6; ///< OneHanded; Enchanting is 23.
constexpr std::uint32_t k_skill_count = 18;
constexpr std::uint32_t k_health = 24;
constexpr std::uint32_t k_magicka = 25;
constexpr std::uint32_t k_stamina = 26;
constexpr std::uint32_t k_heal_rate = 27;
constexpr std::uint32_t k_resist_magic = 44;
constexpr std::uint32_t k_magicka_rate = 28;
constexpr std::uint32_t k_stamina_rate = 29;
constexpr std::uint32_t k_speed_mult = 30;
constexpr std::uint32_t k_carry_weight = 32;
constexpr std::uint32_t k_heal_rate_mult = 155;
constexpr std::uint32_t k_magicka_rate_mult = 156;
constexpr std::uint32_t k_stamina_rate_mult = 157;

/// The name of each index, in index order.
inline constexpr std::array<std::string_view, k_count> k_names = {
    "Aggression",
    "Confidence",
    "Energy",
    "Morality",
    "Mood",
    "Assistance",
    "OneHanded",
    "TwoHanded",
    "Marksman",
    "Block",
    "Smithing",
    "HeavyArmor",
    "LightArmor",
    "Pickpocket",
    "Lockpicking",
    "Sneak",
    "Alchemy",
    "Speechcraft",
    "Alteration",
    "Conjuration",
    "Destruction",
    "Illusion",
    "Restoration",
    "Enchanting",
    "Health",
    "Magicka",
    "Stamina",
    "HealRate",
    "MagickaRate",
    "StaminaRate",
    "SpeedMult",
    "InventoryWeight",
    "CarryWeight",
    "CritChance",
    "MeleeDamage",
    "UnarmedDamage",
    "Mass",
    "VoicePoints",
    "VoiceRate",
    "DamageResist",
    "PoisonResist",
    "ResistFire",
    "ResistShock",
    "ResistFrost",
    "ResistMagic",
    "ResistDisease",
    "Unknown46",
    "Unknown47",
    "Unknown48",
    "Unknown49",
    "Unknown50",
    "Unknown51",
    "Unknown52",
    "Paralysis",
    "Invisibility",
    "NightEye",
    "DetectLifeRange",
    "WaterBreathing",
    "WaterWalking",
    "Unknown59",
    "Fame",
    "Infamy",
    "JumpingBonus",
    "WardPower",
    "RightItemCharge",
    "ArmorPerks",
    "ShieldPerks",
    "WardDeflection",
    "Variable01",
    "Variable02",
    "Variable03",
    "Variable04",
    "Variable05",
    "Variable06",
    "Variable07",
    "Variable08",
    "Variable09",
    "Variable10",
    "BowSpeedBonus",
    "FavorActive",
    "FavorsPerDay",
    "FavorsPerDayTimer",
    "LeftItemCharge",
    "AbsorbChance",
    "Blindness",
    "WeaponSpeedMult",
    "ShoutRecoveryMult",
    "BowStaggerBonus",
    "Telekinesis",
    "FavorPointsBonus",
    "LastBribedIntimidated",
    "LastFlattered",
    "MovementNoiseMult",
    "BypassVendorStolenCheck",
    "BypassVendorKeywordCheck",
    "WaitingForPlayer",
    "OneHandedMod",
    "TwoHandedMod",
    "MarksmanMod",
    "BlockMod",
    "SmithingMod",
    "HeavyArmorMod",
    "LightArmorMod",
    "PickpocketMod",
    "LockpickingMod",
    "SneakingMod",
    "AlchemyMod",
    "SpeechcraftMod",
    "AlterationMod",
    "ConjurationMod",
    "DestructionMod",
    "IllusionMod",
    "RestorationMod",
    "EnchantingMod",
    "OneHandedSkillAdvance",
    "TwoHandedSkillAdvance",
    "MarksmanSkillAdvance",
    "BlockSkillAdvance",
    "SmithingSkillAdvance",
    "HeavyArmorSkillAdvance",
    "LightArmorSkillAdvance",
    "PickpocketSkillAdvance",
    "LockpickingSkillAdvance",
    "SneakingSkillAdvance",
    "AlchemySkillAdvance",
    "SpeechcraftSkillAdvance",
    "AlterationSkillAdvance",
    "ConjurationSkillAdvance",
    "DestructionSkillAdvance",
    "IllusionSkillAdvance",
    "RestorationSkillAdvance",
    "EnchantingSkillAdvance",
    "LeftWeaponSpeedMultiply",
    "DragonSouls",
    "CombatHealthRegenMultiply",
    "OneHandedPowerMod",
    "TwoHandedPowerMod",
    "MarksmanPowerMod",
    "BlockPowerMod",
    "SmithingPowerMod",
    "HeavyArmorPowerMod",
    "LightArmorPowerMod",
    "PickpocketPowerMod",
    "LockpickingPowerMod",
    "SneakingPowerMod",
    "AlchemyPowerMod",
    "SpeechcraftPowerMod",
    "AlterationPowerMod",
    "ConjurationPowerMod",
    "DestructionPowerMod",
    "IllusionPowerMod",
    "RestorationPowerMod",
    "EnchantingPowerMod",
    "DragonRend",
    "AttackDamageMult",
    "HealRateMult",
    "MagickaRateMult",
    "StaminaRateMult",
    "WerewolfPerks",
    "VampirePerks",
    "GrabActorOffset",
    "Grabbed",
    "Unknown162",
    "ReflectDamage"
};

/// Other spellings scripts use for an index (the CK's names differ from the
/// engine's for a few).
struct Alias {
    std::string_view name;
    std::uint32_t index;
};
inline constexpr std::array<Alias, 14> k_aliases = {{
    {"Archery", 8},          {"Speech", 17},           {"Sneaking", 15},
    {"CriticalChance", 33},  {"Aggresion", 0},         {"OneHandedModifier", 96},
    {"TwoHandedModifier", 97}, {"MarksmanModifier", 98}, {"BlockModifier", 99},
    {"SmithingModifier", 100}, {"HeavyArmorModifier", 101}, {"LightArmorModifier", 102},
    {"PickpocketModifier", 103}, {"LockpickingModifier", 104},
}};

namespace detail {
constexpr char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool equal_nocase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}
} // namespace detail

/// The index of the actor value called `name` (any case); none if unknown.
[[nodiscard]] constexpr std::optional<std::uint32_t> index_of(std::string_view name) {
    for (std::size_t i = 0; i < k_count; ++i) {
        if (detail::equal_nocase(k_names[i], name)) {
            return static_cast<std::uint32_t>(i);
        }
    }
    for (const Alias& a : k_aliases) {
        if (detail::equal_nocase(a.name, name)) {
            return a.index;
        }
    }
    return std::nullopt;
}

/// ShoutRecoveryMult (base 1: the multiplier of a shout's recovery) and DragonSouls (the player's
/// unspent souls).
inline constexpr std::uint32_t k_shout_recovery_mult = *index_of("ShoutRecoveryMult");
inline constexpr std::uint32_t k_dragon_souls = *index_of("DragonSouls");

} // namespace skydot::vm::av

namespace skydot::vm {

/// What the data gives an actor: the base of every actor value, and the two
/// flags that decide what happens at Health 0.
struct ActorBase {
    std::array<double, av::k_count> base{};
    bool essential = false;
    bool is_player = false;
    /// The NPC_ the reference is (templates and leveled lists resolved); 0 if unknown. The flags a
    /// script sets on an `ActorBase` are kept by it.
    std::uint32_t npc = 0;
    /// The placed reference starts dead (ACHR "Starts Dead"): dead from the first touch.
    bool starts_dead = false;
};

} // namespace skydot::vm
