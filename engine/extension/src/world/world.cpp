// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/world.hpp"
#include "data/coordinates.hpp"
#include "physics/collision.hpp"
#include "world/queries.hpp"
#include "build/refs.hpp"
#include "data/text.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using godot::Array;
using godot::Dictionary;
using godot::Error;
using godot::String;
using godot::Transform3D;
using godot::Vector3;

namespace skydot {

// ---- binding --------------------------------------------------------------

void SkydotWorld::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("open", "path"), &SkydotWorld::open);
    godot::ClassDB::bind_method(D_METHOD("is_open"), &SkydotWorld::is_open);
    godot::ClassDB::bind_method(D_METHOD("get_error"), &SkydotWorld::get_error);
    godot::ClassDB::bind_method(D_METHOD("get_cell_count"), &SkydotWorld::get_cell_count);
    godot::ClassDB::bind_method(D_METHOD("get_base_count"), &SkydotWorld::get_base_count);
    godot::ClassDB::bind_method(D_METHOD("find_cell", "editor_id"), &SkydotWorld::find_cell);
    godot::ClassDB::bind_method(D_METHOD("list_cells", "filter", "interior_only"),
                                &SkydotWorld::list_cells);
    godot::ClassDB::bind_method(D_METHOD("get_cell", "id"), &SkydotWorld::get_cell);
    godot::ClassDB::bind_method(D_METHOD("get_image_space", "id"), &SkydotWorld::get_image_space);
    godot::ClassDB::bind_method(D_METHOD("get_refs", "cell_id"), &SkydotWorld::get_refs);
    godot::ClassDB::bind_method(D_METHOD("get_base", "id"), &SkydotWorld::get_base);
    godot::ClassDB::bind_method(D_METHOD("build_cell", "id"), &SkydotWorld::build_cell);
    godot::ClassDB::bind_method(D_METHOD("begin_cell", "id"), &SkydotWorld::begin_cell);
    godot::ClassDB::bind_method(D_METHOD("get_cell_resources", "id"), &SkydotWorld::get_cell_resources);
    godot::ClassDB::bind_method(D_METHOD("request_cell", "id"), &SkydotWorld::request_cell);
    godot::ClassDB::bind_method(D_METHOD("get_quest_count"), &SkydotWorld::get_quest_count);
    godot::ClassDB::bind_method(D_METHOD("has_quest", "id"), &SkydotWorld::has_quest);
    godot::ClassDB::bind_method(D_METHOD("list_quests", "filter"), &SkydotWorld::list_quests);
    godot::ClassDB::bind_method(D_METHOD("find_quest", "editor_id"), &SkydotWorld::find_quest);
    godot::ClassDB::bind_method(D_METHOD("get_quest", "id"), &SkydotWorld::get_quest);
    godot::ClassDB::bind_method(D_METHOD("get_global", "id"), &SkydotWorld::get_global);
    godot::ClassDB::bind_method(D_METHOD("get_actor", "ref"), &SkydotWorld::get_actor);
    godot::ClassDB::bind_method(D_METHOD("find_actor_of", "npc"), &SkydotWorld::find_actor_of);
    godot::ClassDB::bind_method(D_METHOD("find_npc", "editor_id"), &SkydotWorld::find_npc);
    godot::ClassDB::bind_method(D_METHOD("get_form_from_file", "id", "plugin"),
                                &SkydotWorld::get_form_from_file);
    godot::ClassDB::bind_method(D_METHOD("get_door", "ref"), &SkydotWorld::get_door);
    godot::ClassDB::bind_method(D_METHOD("get_ref_info", "cell", "ref"), &SkydotWorld::get_ref_info);
    godot::ClassDB::bind_method(D_METHOD("pick_ref", "root", "from", "to"), &SkydotWorld::pick_ref);
    godot::ClassDB::bind_method(D_METHOD("get_ref_cell", "ref"), &SkydotWorld::get_ref_cell);
    godot::ClassDB::bind_method(D_METHOD("build_ref", "cell", "ref"), &SkydotWorld::build_ref);
    godot::ClassDB::bind_method(D_METHOD("get_scripted_refs", "cell"), &SkydotWorld::get_scripted_refs);
    godot::ClassDB::bind_method(D_METHOD("get_activate_children", "ref"),
                                &SkydotWorld::get_activate_children);
    godot::ClassDB::bind_method(D_METHOD("get_large_ref_count", "world"), &SkydotWorld::get_large_ref_count);
    godot::ClassDB::bind_method(D_METHOD("get_large_refs", "world", "x", "y", "radius"),
                                &SkydotWorld::get_large_refs);
    godot::ClassDB::bind_method(D_METHOD("get_enable_children", "ref"),
                                &SkydotWorld::get_enable_children);
    godot::ClassDB::bind_method(D_METHOD("list_worlds"), &SkydotWorld::list_worlds);
    godot::ClassDB::bind_method(D_METHOD("find_world", "editor_id"), &SkydotWorld::find_world);
    godot::ClassDB::bind_method(D_METHOD("get_exterior_cell", "world", "x", "y"),
                                &SkydotWorld::get_exterior_cell);
    godot::ClassDB::bind_method(D_METHOD("build_exterior", "world", "x", "y"),
                                &SkydotWorld::build_exterior);
    godot::ClassDB::bind_method(D_METHOD("begin_exterior", "world", "x", "y"),
                                &SkydotWorld::begin_exterior);
    godot::ClassDB::bind_method(D_METHOD("continue_build", "root", "budget_usec"),
                                &SkydotWorld::continue_build);
    godot::ClassDB::bind_method(D_METHOD("continue_build_static", "root", "budget_usec"),
                                &SkydotWorld::continue_build_static);
    godot::ClassDB::bind_method(D_METHOD("get_exterior_resources", "world", "x", "y"),
                                &SkydotWorld::get_exterior_resources);
    godot::ClassDB::bind_method(D_METHOD("request_exterior", "world", "x", "y"),
                                &SkydotWorld::request_exterior);
    godot::ClassDB::bind_method(D_METHOD("get_cached_resource_count"),
                                &SkydotWorld::get_cached_resource_count);
    godot::ClassDB::bind_method(D_METHOD("get_pending_resource_count"),
                                &SkydotWorld::get_pending_resource_count);
    godot::ClassDB::bind_method(D_METHOD("trim_cache"), &SkydotWorld::trim_cache);
    godot::ClassDB::bind_method(D_METHOD("warm_up"), &SkydotWorld::warm_up);
    godot::ClassDB::bind_method(D_METHOD("find_weather", "editor_id"), &SkydotWorld::find_weather);
    godot::ClassDB::bind_method(D_METHOD("get_sky", "world", "hour", "weather"),
                                &SkydotWorld::get_sky, DEFVAL(0));
    godot::ClassDB::bind_method(D_METHOD("set_terrain_tiling", "repeats"),
                                &SkydotWorld::set_terrain_tiling);
    godot::ClassDB::bind_method(D_METHOD("get_terrain_tiling"), &SkydotWorld::get_terrain_tiling);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::FLOAT, "terrain_tiling"),
                 "set_terrain_tiling", "get_terrain_tiling");
    godot::ClassDB::bind_method(D_METHOD("set_foliage_bias", "levels"), &SkydotWorld::set_foliage_bias);
    godot::ClassDB::bind_method(D_METHOD("get_foliage_bias"), &SkydotWorld::get_foliage_bias);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::FLOAT, "foliage_bias"), "set_foliage_bias",
                 "get_foliage_bias");
    godot::ClassDB::bind_method(D_METHOD("set_skyrim_materials", "enabled"),
                                &SkydotWorld::set_skyrim_materials);
    godot::ClassDB::bind_method(D_METHOD("get_skyrim_materials"),
                                &SkydotWorld::get_skyrim_materials);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "skyrim_materials"),
                 "set_skyrim_materials", "get_skyrim_materials");
    godot::ClassDB::bind_method(D_METHOD("set_effects", "enabled"), &SkydotWorld::set_effects);
    godot::ClassDB::bind_method(D_METHOD("get_effects"), &SkydotWorld::get_effects);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "effects"), "set_effects", "get_effects");
    godot::ClassDB::bind_method(D_METHOD("set_all_light_shadows", "enabled"), &SkydotWorld::set_all_light_shadows);
    godot::ClassDB::bind_method(D_METHOD("get_all_light_shadows"), &SkydotWorld::get_all_light_shadows);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "all_light_shadows"), "set_all_light_shadows",
                 "get_all_light_shadows");
    godot::ClassDB::bind_static_method("SkydotWorld", D_METHOD("apply_light_shadows", "root", "all"),
                                       &SkydotWorld::apply_light_shadows);
    godot::ClassDB::bind_method(D_METHOD("set_grass", "enabled"), &SkydotWorld::set_grass);
    godot::ClassDB::bind_method(D_METHOD("get_grass"), &SkydotWorld::get_grass);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "grass"), "set_grass", "get_grass");
    godot::ClassDB::bind_method(D_METHOD("set_actors", "enabled"), &SkydotWorld::set_actors);
    godot::ClassDB::bind_method(D_METHOD("get_actors"), &SkydotWorld::get_actors);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "actors"), "set_actors", "get_actors");
    godot::ClassDB::bind_method(D_METHOD("get_actor_plan", "ref"), &SkydotWorld::get_actor_plan);
    godot::ClassDB::bind_method(D_METHOD("set_actor_wander", "enabled"), &SkydotWorld::set_actor_wander);
    godot::ClassDB::bind_method(D_METHOD("get_actor_wander"), &SkydotWorld::get_actor_wander);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "actor_wander"), "set_actor_wander", "get_actor_wander");
    godot::ClassDB::bind_method(D_METHOD("get_locomotion", "behaviour"), &SkydotWorld::get_locomotion);
    godot::ClassDB::bind_method(D_METHOD("build_actor", "ref"), &SkydotWorld::build_actor);
    godot::ClassDB::bind_method(D_METHOD("get_cell_actors", "cell"), &SkydotWorld::get_cell_actors);
    godot::ClassDB::bind_method(D_METHOD("set_actor_place", "ref", "space", "position", "rotation_z"),
                                &SkydotWorld::set_actor_place);
    godot::ClassDB::bind_method(D_METHOD("clear_actor_place", "ref"), &SkydotWorld::clear_actor_place);
    godot::ClassDB::bind_method(D_METHOD("clear_actor_places"), &SkydotWorld::clear_actor_places);
    godot::ClassDB::bind_method(D_METHOD("get_actor_place", "ref"), &SkydotWorld::get_actor_place);
    godot::ClassDB::bind_method(D_METHOD("get_cell_space", "cell"), &SkydotWorld::get_cell_space);
    godot::ClassDB::bind_method(D_METHOD("nearest_nav_point", "space", "position", "reach"),
                                &SkydotWorld::nearest_nav_point);
    godot::ClassDB::bind_method(D_METHOD("set_collision", "enabled"), &SkydotWorld::set_collision);
    godot::ClassDB::bind_method(D_METHOD("get_collision"), &SkydotWorld::get_collision);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "collision"), "set_collision", "get_collision");
    godot::ClassDB::bind_method(D_METHOD("set_navigation", "enabled"), &SkydotWorld::set_navigation);
    godot::ClassDB::bind_method(D_METHOD("get_navigation"), &SkydotWorld::get_navigation);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "navigation"), "set_navigation", "get_navigation");
    godot::ClassDB::bind_static_method("SkydotWorld", D_METHOD("wake_clutter", "root", "centre", "radius"),
                                       &SkydotWorld::wake_clutter);
    godot::ClassDB::bind_method(D_METHOD("get_navmeshes", "cell"), &SkydotWorld::get_navmeshes);
    godot::ClassDB::bind_method(D_METHOD("get_navmesh", "id"), &SkydotWorld::get_navmesh);
    godot::ClassDB::bind_static_method("SkydotWorld", D_METHOD("unit_scale"),
                                       &SkydotWorld::unit_scale);
    godot::ClassDB::bind_static_method("SkydotWorld",
                                       D_METHOD("skyrim_transform", "position", "rotation", "scale"),
                                       &SkydotWorld::skyrim_transform);
    godot::ClassDB::bind_static_method("SkydotWorld", D_METHOD("skyrim_position", "position"),
                                       &SkydotWorld::skyrim_position);
    godot::ClassDB::bind_static_method("SkydotWorld", D_METHOD("godot_to_skyrim", "position"),
                                       &SkydotWorld::godot_to_skyrim);
    BIND_CONSTANT(WORLD_FORMAT_VERSION);
    BIND_CONSTANT(WORLD_FORMAT_VERSION_MIN);
    BIND_CONSTANT(CELL_UNITS);
}

// ---- opening --------------------------------------------------------------

Error SkydotWorld::fail(Error code, const String& why) {
    error_ = why;
    godot::UtilityFunctions::push_error("SkydotWorld: ", why);
    return code;
}

Error SkydotWorld::open(const String& path) {
    // Every index, cache and build under way points into the open buffer, so
    // a world is opened once; SkydotPack::open_world makes a new one.
    if (data_->is_open()) {
        return fail(godot::ERR_ALREADY_IN_USE,
                    String("this SkydotWorld already has a world.fb open; open ") + path +
                        " with a new SkydotWorld");
    }
    error_ = String();
    if (!godot::FileAccess::file_exists(path)) {
        return fail(godot::ERR_FILE_NOT_FOUND, String("no world.fb at ") + path);
    }
    // Mapped from where it lies on disk, so a res:// or user:// path is
    // turned into one. Verified before anything keeps it: a refused file
    // leaves the world as it was, closed and without indexes.
    auto data = std::make_shared<WorldData>();
    const auto native = godot::ProjectSettings::get_singleton()->globalize_path(path).utf8();
    const auto opened = data->open(std::string(native.get_data(), static_cast<std::size_t>(native.length())));
    if (opened.status == WorldData::Status::unreadable) {
        return fail(godot::ERR_FILE_CANT_OPEN, String::utf8(opened.error.c_str()));
    }
    if (opened.status == WorldData::Status::corrupt) {
        return fail(godot::ERR_FILE_CORRUPT, path + String(" is not a valid world.fb"));
    }
    if (opened.status == WorldData::Status::unsupported) {
        return fail(godot::ERR_FILE_UNRECOGNIZED,
                    String("world.fb format version ") + String::num_int64(opened.version) +
                        " is not one this engine reads (it reads " +
                        String::num_int64(WORLD_FORMAT_VERSION_MIN) + " to " +
                        String::num_int64(WORLD_FORMAT_VERSION) + ")");
    }
    data_ = std::move(data);
    placement_ = ActorPlacement(data_);
    builder_.open(data_);
    return godot::OK;
}

bool SkydotWorld::is_open() const { return data_->is_open(); }
String SkydotWorld::get_error() const { return error_; }

// ---- reading world.fb (queries_*.cpp) --------------------------------------

std::int64_t SkydotWorld::get_cell_count() const { return queries::get_cell_count(data()); }
std::int64_t SkydotWorld::get_base_count() const { return queries::get_base_count(data()); }
std::int64_t SkydotWorld::find_cell(const String& editor_id) const { return queries::find_cell(data(), editor_id); }
Array SkydotWorld::list_cells(const String& filter, bool interior_only) const {
    return queries::list_cells(data(), filter, interior_only);
}
Dictionary SkydotWorld::get_cell(std::int64_t id) const { return queries::get_cell(data(), id); }
Dictionary SkydotWorld::get_image_space(std::int64_t id) const { return queries::get_image_space(data(), id); }
Array SkydotWorld::get_refs(std::int64_t cell_id) const { return queries::get_refs(data(), cell_id); }
Dictionary SkydotWorld::get_base(std::int64_t id) const { return queries::get_base(data(), id); }
Dictionary SkydotWorld::get_door(std::int64_t ref) const { return queries::get_door(data(), ref); }
Dictionary SkydotWorld::get_ref_info(std::int64_t cell, std::int64_t ref) const {
    return queries::get_ref_info(data(), cell, ref);
}
Dictionary SkydotWorld::pick_ref(godot::Node* root, const Vector3& from, const Vector3& to) const {
    return skydot::pick_ref(root, from, to);
}
godot::PackedInt64Array SkydotWorld::get_scripted_refs(std::int64_t cell) const {
    return queries::get_scripted_refs(data(), cell);
}
std::int64_t SkydotWorld::get_ref_cell(std::int64_t ref) const { return data().cell_of_ref(ref); }
Array SkydotWorld::get_activate_children(std::int64_t ref) const {
    return queries::get_activate_children(data(), ref);
}
std::int64_t SkydotWorld::get_large_ref_count(std::int64_t world) const {
    return queries::get_large_ref_count(data(), world);
}
godot::Array SkydotWorld::get_large_refs(std::int64_t world, std::int64_t x, std::int64_t y,
                                         std::int64_t radius) const {
    return queries::get_large_refs(data(), world, x, y, radius);
}
godot::PackedInt64Array SkydotWorld::get_enable_children(std::int64_t ref) const {
    return queries::get_enable_children(data(), ref);
}

std::int64_t SkydotWorld::get_quest_count() const { return queries::get_quest_count(data()); }
bool SkydotWorld::has_quest(std::int64_t id) const { return queries::has_quest(data(), id); }
Array SkydotWorld::list_quests(const String& filter) const { return queries::list_quests(data(), filter); }
std::int64_t SkydotWorld::find_quest(const String& editor_id) const { return queries::find_quest(data(), editor_id); }
Dictionary SkydotWorld::get_quest(std::int64_t id) const { return queries::get_quest(data(), id); }
Dictionary SkydotWorld::get_global(std::int64_t id) const { return queries::get_global(data(), id); }
Dictionary SkydotWorld::get_actor(std::int64_t ref) const { return queries::get_actor(data(), ref); }
std::int64_t SkydotWorld::get_form_from_file(std::int64_t id, const String& plugin) const {
    return queries::get_form_from_file(data(), id, plugin);
}
std::int64_t SkydotWorld::find_actor_of(std::int64_t npc) const { return queries::find_actor_of(data(), npc); }
std::int64_t SkydotWorld::find_npc(const String& editor_id) const { return queries::find_npc(data(), editor_id); }

Array SkydotWorld::list_worlds() const { return queries::list_worlds(data()); }
std::int64_t SkydotWorld::find_world(const String& editor_id) const { return queries::find_world(data(), editor_id); }
std::int64_t SkydotWorld::get_exterior_cell(std::int64_t world, std::int64_t x, std::int64_t y) const {
    return queries::get_exterior_cell(data(), world, x, y);
}
std::int64_t SkydotWorld::find_weather(const String& editor_id) const {
    return queries::find_weather(data(), editor_id);
}
Dictionary SkydotWorld::get_sky(std::int64_t world, double hour, std::int64_t weather) const {
    return queries::get_sky(data(), world, hour, weather);
}

godot::PackedInt64Array SkydotWorld::get_cell_actors(std::int64_t cell) const {
    return queries::get_cell_actors(data(), cell);
}
Array SkydotWorld::get_navmeshes(std::int64_t cell) const { return queries::get_navmeshes(data(), cell); }
Dictionary SkydotWorld::get_navmesh(std::int64_t id) const { return queries::get_navmesh(data(), id); }
Vector3 SkydotWorld::nearest_nav_point(std::int64_t space, const Vector3& position, double reach) const {
    return queries::nearest_nav_point(data(), space, position, reach);
}

// ---- coordinates ----------------------------------------------------------

Vector3 SkydotWorld::skyrim_position(const Vector3& position) { return skydot::skyrim_position(position); }

Vector3 SkydotWorld::godot_to_skyrim(const Vector3& position) { return skydot::godot_to_skyrim(position); }

Transform3D SkydotWorld::skyrim_transform(const Vector3& position, const Vector3& rotation,
                                          double scale) {
    return skydot::skyrim_transform(position, rotation, scale);
}

// ---- building -------------------------------------------------------------

void SkydotWorld::set_skyrim_materials(bool enabled) { builder_.options().skyrim_materials = enabled; }
bool SkydotWorld::get_skyrim_materials() const { return builder_.options().skyrim_materials; }
void SkydotWorld::set_foliage_bias(double bias) {
    foliage_bias_ = bias;
    SkydotMaterials::set_foliage_bias(static_cast<float>(bias));
}
double SkydotWorld::get_foliage_bias() const { return foliage_bias_; }
void SkydotWorld::set_effects(bool enabled) { builder_.options().effects = enabled; }
bool SkydotWorld::get_effects() const { return builder_.options().effects; }
void SkydotWorld::set_collision(bool enabled) { builder_.options().collision = enabled; }
void SkydotWorld::set_terrain_tiling(double repeats) { builder_.options().terrain_tiling = repeats; }
double SkydotWorld::get_terrain_tiling() const { return builder_.options().terrain_tiling; }

godot::Node3D* SkydotWorld::build_cell(std::int64_t id) { return builder_.build_cell(id); }
godot::Node3D* SkydotWorld::begin_cell(std::int64_t id) { return builder_.begin_cell(id); }
godot::Node3D* SkydotWorld::build_exterior(std::int64_t world, std::int64_t x, std::int64_t y) {
    return builder_.build_exterior(world, x, y);
}
godot::Node3D* SkydotWorld::begin_exterior(std::int64_t world, std::int64_t x, std::int64_t y) {
    return builder_.begin_exterior(world, x, y);
}
bool SkydotWorld::continue_build(godot::Node3D* root, std::int64_t budget_usec) {
    return builder_.continue_build(root, budget_usec);
}
bool SkydotWorld::continue_build_static(godot::Node3D* root, std::int64_t budget_usec) {
    return builder_.continue_build_static(root, budget_usec);
}
godot::Node3D* SkydotWorld::build_ref(std::int64_t cell, std::int64_t ref) {
    return builder_.build_ref(cell, ref);
}
godot::Node3D* SkydotWorld::build_actor(std::int64_t ref) { return builder_.build_actor(ref); }

godot::PackedStringArray SkydotWorld::get_cell_resources(std::int64_t id) {
    return builder_.cell_resources(id);
}
godot::PackedStringArray SkydotWorld::get_exterior_resources(std::int64_t world, std::int64_t x,
                                                             std::int64_t y) {
    return builder_.exterior_resources(world, x, y);
}
std::int64_t SkydotWorld::request_cell(std::int64_t id) { return builder_.request_cell(id); }
std::int64_t SkydotWorld::request_exterior(std::int64_t world, std::int64_t x, std::int64_t y) {
    return builder_.request_exterior(world, x, y);
}
std::int64_t SkydotWorld::get_cached_resource_count() const { return builder_.cached_resource_count(); }
std::int64_t SkydotWorld::get_pending_resource_count() const { return builder_.pending_resource_count(); }
void SkydotWorld::trim_cache() { builder_.trim_cache(); }
std::int64_t SkydotWorld::warm_up() { return builder_.warm_up(); }
std::int64_t SkydotWorld::apply_light_shadows(godot::Node* root, bool all) {
    return CellBuilder::apply_light_shadows(root, all);
}
godot::Ref<godot::Resource> SkydotWorld::resource(const String& vpath) const { return builder_.resource(vpath); }

// ---- where actors are ---------------------------------------------------

void SkydotWorld::set_actor_place(std::int64_t ref, std::int64_t space, const Vector3& position,
                                  double rotation_z) {
    placement_.set_place(static_cast<std::uint32_t>(ref), static_cast<std::uint32_t>(space), position,
                         static_cast<float>(rotation_z));
}

void SkydotWorld::clear_actor_place(std::int64_t ref) {
    placement_.clear_place(static_cast<std::uint32_t>(ref));
}

void SkydotWorld::clear_actor_places() { placement_.clear_places(); }

std::int64_t SkydotWorld::get_cell_space(std::int64_t id) const { return data().cell_space(id); }

Dictionary SkydotWorld::get_actor_place(std::int64_t ref) const {
    Dictionary out;
    const auto where = placement_.where(static_cast<std::uint32_t>(ref));
    if (!where) {
        return out;
    }
    out["space"] = static_cast<std::int64_t>(where->place.space);
    out["interior"] = where->interior;
    out["cell"] = static_cast<std::int64_t>(where->cell);
    out["position"] = where->place.position;
    out["rotation_z"] = static_cast<double>(where->place.rotation_z);
    out["moved"] = where->moved;
    return out;
}

// ---- actors -------------------------------------------------------------

namespace {

godot::Dictionary plan_dictionary(const skydot::ActorPlan& plan) {
    godot::Dictionary out;
    out["npc"] = static_cast<std::int64_t>(plan.npc);
    out["race"] = static_cast<std::int64_t>(plan.race);
    out["female"] = plan.female;
    out["scale"] = plan.scale;
    out["skin_tone"] = godot::Color(plan.skin_tone[0], plan.skin_tone[1], plan.skin_tone[2]);
    out["hide_hair"] = plan.hide_hair;
    out["skeleton"] = String::utf8(plan.skeleton.c_str());
    out["idle"] = String::utf8(plan.idle.c_str());
    out["behaviour"] = String::utf8(plan.behaviour.c_str());
    godot::PackedStringArray parts;
    for (const auto& p : plan.parts) {
        parts.push_back(String::utf8(p.c_str()));
    }
    out["parts"] = parts;
    out["missing"] = String::utf8(plan.missing.c_str());
    return out;
}

} // namespace

godot::Dictionary SkydotWorld::get_locomotion(const String& behaviour) {
    const Locomotion& l = builder_.locomotion_of(behaviour.utf8().get_data());
    const auto gait = [](const GaitClip& g) {
        godot::Dictionary d;
        d["name"] = String::utf8(g.name.c_str());
        d["file"] = String::utf8(g.file.c_str());
        d["playback"] = g.playback;
        d["speed"] = g.speed;
        return d;
    };
    godot::Dictionary out;
    out["idle"] = gait(l.idle);
    out["walk"] = gait(l.walk);
    out["run"] = gait(l.run);
    out["missing"] = String::utf8(l.missing.c_str());
    return out;
}

godot::Dictionary SkydotWorld::get_actor_plan(std::int64_t ref) const {
    const auto* actor = data().actor_ptr(ref);
    if (actor == nullptr) {
        return {};
    }
    return plan_dictionary(builder_.actor_plan(*actor));
}

std::int64_t SkydotWorld::wake_clutter(godot::Node* root, const Vector3& centre, double radius) {
    return skydot::wake_clutter(root, centre, static_cast<godot::real_t>(radius));
}
} // namespace skydot
