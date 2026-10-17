// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which condition functions are bound, for the docs and the tests. The
// bindings themselves are `function_value` (conditions.hpp), a switch by
// index in functions.cpp.
#pragma once

#include <cstdint>
#include <span>

namespace skydot::conditions {

/// Condition function indices (xEdit's wbConditionFunctions).
namespace fn {
inline constexpr std::uint16_t get_distance = 1;
inline constexpr std::uint16_t get_actor_value = 14;
inline constexpr std::uint16_t get_current_time = 18;
inline constexpr std::uint16_t get_disabled = 35;
inline constexpr std::uint16_t get_detected = 45;
inline constexpr std::uint16_t get_dead = 46;
inline constexpr std::uint16_t get_item_count = 47;
inline constexpr std::uint16_t get_gold = 48;
inline constexpr std::uint16_t get_talked_to_pc = 50;
inline constexpr std::uint16_t get_quest_running = 56;
inline constexpr std::uint16_t get_stage = 58;
inline constexpr std::uint16_t get_stage_done = 59;
inline constexpr std::uint16_t get_should_attack = 66;
inline constexpr std::uint16_t get_in_cell = 67;
inline constexpr std::uint16_t get_is_race = 69;
inline constexpr std::uint16_t get_is_sex = 70;
inline constexpr std::uint16_t get_in_faction = 71;
inline constexpr std::uint16_t get_is_id = 72;
inline constexpr std::uint16_t get_faction_rank = 73;
inline constexpr std::uint16_t get_global_value = 74;
inline constexpr std::uint16_t get_random_percent = 77;
inline constexpr std::uint16_t get_level = 80;
inline constexpr std::uint16_t get_dead_count = 84;
inline constexpr std::uint16_t is_weapon_magic_out = 101;
inline constexpr std::uint16_t get_pc_is_race = 130;
inline constexpr std::uint16_t get_pc_is_sex = 131;
inline constexpr std::uint16_t get_is_reference = 136;
inline constexpr std::uint16_t get_trespass_warning_level = 144;
inline constexpr std::uint16_t is_trespassing = 145;
inline constexpr std::uint16_t get_is_current_package = 161;
inline constexpr std::uint16_t get_day_of_week = 170;
inline constexpr std::uint16_t is_in_dialogue_with_player = 249;
inline constexpr std::uint16_t get_offers_services_now = 255;
inline constexpr std::uint16_t is_weapon_out = 263;
inline constexpr std::uint16_t get_base_actor_value = 277;
inline constexpr std::uint16_t is_sneaking = 286;
inline constexpr std::uint16_t is_in_combat = 289;
inline constexpr std::uint16_t is_in_interior = 300;
inline constexpr std::uint16_t get_in_worldspace = 310;
inline constexpr std::uint16_t is_actor_a_victim = 314;
inline constexpr std::uint16_t get_in_current_loc = 359;
inline constexpr std::uint16_t has_linked_ref = 362;
inline constexpr std::uint16_t get_relationship_rank = 403;
inline constexpr std::uint16_t get_is_voice_type = 426;
inline constexpr std::uint16_t get_player_teammate = 453;
inline constexpr std::uint16_t get_crime_gold = 459;
inline constexpr std::uint16_t get_quest_completed = 543;
inline constexpr std::uint16_t has_keyword = 560;
inline constexpr std::uint16_t location_has_keyword = 562;
inline constexpr std::uint16_t get_is_alias_ref = 566;
inline constexpr std::uint16_t get_equipped_shout = 579;
inline constexpr std::uint16_t get_numeric_package_data = 612;
inline constexpr std::uint16_t get_vm_quest_variable = 629;
inline constexpr std::uint16_t get_vm_script_variable = 630;
inline constexpr std::uint16_t get_actor_value_percent = 640;
inline constexpr std::uint16_t is_linked_to = 650;
inline constexpr std::uint16_t get_bribe_success = 654;
inline constexpr std::uint16_t get_intimidate_success = 655;
} // namespace fn

/// The functions bound to a resting value because the system they read does
/// not exist yet (docs/conditions.md).
[[nodiscard]] std::span<const std::uint16_t> placeholder_functions();

} // namespace skydot::conditions
