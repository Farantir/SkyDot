// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the bound SkydotWorld calls that only read world.fb answer: lists and
// records as Dictionaries and Arrays, built from a WorldData. Free functions,
// so each is testable without a SkydotWorld and none can change the world.
// Each is documented where it is bound, in world.hpp. A closed WorldData
// answers every one with nothing (an empty Dictionary or Array, 0).
#pragma once

#include "data/world_data.hpp"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace skydot::queries {

// ---- cells, bases, worldspaces (queries_cells.cpp) ------------------------
std::int64_t get_cell_count(const WorldData& data);
std::int64_t get_base_count(const WorldData& data);
std::int64_t find_cell(const WorldData& data, const godot::String& editor_id);
godot::Array list_cells(const WorldData& data, const godot::String& filter, bool interior_only);
godot::Dictionary get_cell(const WorldData& data, std::int64_t id);
godot::Dictionary get_image_space(const WorldData& data, std::int64_t id);
godot::Array get_refs(const WorldData& data, std::int64_t cell_id);
godot::Dictionary get_base(const WorldData& data, std::int64_t id);
godot::PackedInt64Array get_cell_actors(const WorldData& data, std::int64_t cell);
godot::Array list_worlds(const WorldData& data);
std::int64_t find_world(const WorldData& data, const godot::String& editor_id);
std::int64_t get_exterior_cell(const WorldData& data, std::int64_t world, std::int64_t x, std::int64_t y);
godot::Array get_navmeshes(const WorldData& data, std::int64_t cell_id);
godot::Dictionary get_navmesh(const WorldData& data, std::int64_t id);
godot::Vector3 nearest_nav_point(const WorldData& data, std::int64_t space, const godot::Vector3& position,
                                 double reach);

// ---- weather (queries_sky.cpp) --------------------------------------------
std::int64_t find_weather(const WorldData& data, const godot::String& editor_id);
godot::Dictionary get_sky(const WorldData& data, std::int64_t world, double hour, std::int64_t weather_id);

// ---- references (queries_refs.cpp) ----------------------------------------
godot::Dictionary get_door(const WorldData& data, std::int64_t ref);
godot::Dictionary get_ref_info(const WorldData& data, std::int64_t cell_id, std::int64_t ref_id);
godot::PackedInt64Array get_enable_children(const WorldData& data, std::int64_t ref);
godot::Array get_activate_children(const WorldData& data, std::int64_t ref);
godot::PackedInt64Array get_scripted_refs(const WorldData& data, std::int64_t cell_id);
std::int64_t get_large_ref_count(const WorldData& data, std::int64_t world);
godot::Array get_large_refs(const WorldData& data, std::int64_t world, std::int64_t x, std::int64_t y,
                            std::int64_t radius);

// ---- quests, globals, placed actors, NPCs (queries_quests.cpp) ------------
std::int64_t get_quest_count(const WorldData& data);
bool has_quest(const WorldData& data, std::int64_t id);
godot::Array list_quests(const WorldData& data, const godot::String& filter);
std::int64_t find_quest(const WorldData& data, const godot::String& editor_id);
godot::Dictionary get_quest(const WorldData& data, std::int64_t id);
godot::Dictionary get_global(const WorldData& data, std::int64_t id);
godot::Dictionary get_actor(const WorldData& data, std::int64_t ref);
std::int64_t get_form_from_file(const WorldData& data, std::int64_t id, const godot::String& plugin);
std::int64_t find_actor_of(const WorldData& data, std::int64_t npc);
std::int64_t find_npc(const WorldData& data, const godot::String& editor_id);

} // namespace skydot::queries
