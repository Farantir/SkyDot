// SPDX-License-Identifier: GPL-3.0-or-later
//
// An AVIF record's actor value index by editor id. The editor ids of Skyrim.esm are "AV" + the
// shared name (skydot_formats/actor_values.hpp), except the few in `k_actor_value_aliases`
// (checked against all 149 AVIF records).
#pragma once

#include "skydot_formats/actor_values.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace bethconv::pack::detail {

using skydot::formats::k_actor_value_names;

/// AVIF editor ids that are not "AV" + a shared name.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 14> k_actor_value_aliases = {{
    {"Mysticism", "Illusion"},
    {"PickPocketMod", "PickpocketMod"},
    {"PickPocketPowerMod", "PickpocketPowerMod"},
    {"PickPocketSkillAdvance", "PickpocketSkillAdvance"},
    {"SneakMod", "SneakingMod"},
    {"SneakPowerMod", "SneakingPowerMod"},
    {"SneakSkillAdvance", "SneakingSkillAdvance"},
    {"FireResist", "ResistFire"},
    {"FrostResist", "ResistFrost"},
    {"ElectricResist", "ResistShock"},
    {"MagicResist", "ResistMagic"},
    {"CombatHealthRegenMult", "CombatHealthRegenMultiply"},
    {"LeftWeaponSpeedMult", "LeftWeaponSpeedMultiply"},
    {"DiseaseResist", "ResistDisease"},
}};

/// The actor value index of an AVIF record by editor id, -1 if it has none.
[[nodiscard]] constexpr int actor_value_index(std::string_view editor_id) noexcept {
    if (!editor_id.starts_with("AV")) {
        return -1;
    }
    std::string_view name = editor_id.substr(2);
    for (const auto& [alias, target] : k_actor_value_aliases) {
        if (alias == name) {
            name = target;
            break;
        }
    }
    for (std::size_t i = 0; i < k_actor_value_names.size(); ++i) {
        if (k_actor_value_names[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace bethconv::pack::detail
