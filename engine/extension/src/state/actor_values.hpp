// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyrim's fixed actor value indices (the names are shared with the converter,
// skydot_formats/actor_values.hpp) and the other spellings Papyrus passes.
// Nothing here touches Godot.
#pragma once

#include "skydot_formats/actor_values.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace skydot::vm::av {

constexpr std::size_t k_count = skydot::formats::k_actor_value_count;

constexpr std::uint32_t k_aggression = 0;
constexpr std::uint32_t k_assistance = 5;
constexpr std::uint32_t k_first_skill = skydot::formats::k_first_skill_actor_value; ///< OneHanded.
constexpr std::uint32_t k_skill_count = skydot::formats::k_skill_count;
constexpr std::uint32_t k_health = 24;
constexpr std::uint32_t k_magicka = 25;
constexpr std::uint32_t k_stamina = 26;
constexpr std::uint32_t k_heal_rate = 27;
constexpr std::uint32_t k_resist_magic = 44;
constexpr std::uint32_t k_magicka_rate = 28;
constexpr std::uint32_t k_stamina_rate = 29;
constexpr std::uint32_t k_speed_mult = 30;
constexpr std::uint32_t k_carry_weight = 32;
constexpr std::uint32_t k_combat_health_regen_multiply = 134;
constexpr std::uint32_t k_heal_rate_mult = 155;
constexpr std::uint32_t k_magicka_rate_mult = 156;
constexpr std::uint32_t k_stamina_rate_mult = 157;

/// The name of each index, in index order.
inline constexpr const auto& k_names = skydot::formats::k_actor_value_names;

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
