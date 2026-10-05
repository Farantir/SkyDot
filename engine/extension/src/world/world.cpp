// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/world.hpp"
#include "world/coordinates.hpp"
#include "world/fb_search.hpp"
#include "world/text.hpp"

#include "world/collision.hpp"
#include "world/refs.hpp"

#include "skydot_formats/flags.hpp"
#include "skydot_formats/units.hpp"
#include "world_generated.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using godot::Array;
using godot::Color;
using godot::Dictionary;
using godot::Error;
using godot::String;
using godot::Transform3D;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {

namespace {

bool contains_ci(const flatbuffers::String* haystack, const std::string& needle_lower) {
    if (haystack == nullptr) {
        return false;
    }
    std::string lower(haystack->string_view());
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
        return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    });
    return lower.find(needle_lower) != std::string::npos;
}

/// Game units per exterior cell side.
constexpr auto k_cell_units = static_cast<float>(formats::k_cell_units);

} // namespace

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

std::int64_t SkydotWorld::get_cell_count() const {
    return world_fb() != nullptr && world_fb()->cells() != nullptr ? world_fb()->cells()->size() : 0;
}

std::int64_t SkydotWorld::get_base_count() const {
    return world_fb() != nullptr && world_fb()->bases() != nullptr ? world_fb()->bases()->size() : 0;
}

std::int64_t SkydotWorld::find_cell(const String& editor_id) const {
    const auto* cells = world_fb() != nullptr ? world_fb()->cells() : nullptr;
    if (cells == nullptr) {
        return 0;
    }
    const auto wanted = editor_id.to_lower();
    for (const auto* cell : *cells) {
        if (cell->editor_id() != nullptr && to_godot(cell->editor_id()).to_lower() == wanted) {
            return cell->id();
        }
    }
    return 0;
}

Array SkydotWorld::list_cells(const String& filter, bool interior_only) const {
    Array out;
    const auto* cells = world_fb() != nullptr ? world_fb()->cells() : nullptr;
    if (cells == nullptr) {
        return out;
    }
    const auto needle = filter.to_lower().utf8();
    const std::string needle_str(needle.get_data(), static_cast<std::size_t>(needle.length()));
    for (const auto* cell : *cells) {
        const bool interior = formats::has_flag(cell->flags(), wfb::CellFlags::interior);
        if (interior_only && !interior) {
            continue;
        }
        if (!needle_str.empty() && !contains_ci(cell->editor_id(), needle_str)) {
            continue;
        }
        Dictionary entry;
        entry["id"] = static_cast<std::int64_t>(cell->id());
        entry["editor_id"] = to_godot(cell->editor_id());
        entry["interior"] = interior;
        entry["ref_count"] = static_cast<std::int64_t>(cell->refs() ? cell->refs()->size() : 0);
        out.push_back(entry);
    }
    return out;
}

Dictionary SkydotWorld::get_cell(std::int64_t id) const {
    Dictionary out;
    const auto* cell = data().cell_ptr(id);
    if (cell == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(cell->id());
    out["editor_id"] = to_godot(cell->editor_id());
    out["interior"] = formats::has_flag(cell->flags(), wfb::CellFlags::interior);
    out["world"] = static_cast<std::int64_t>(cell->world());
    out["grid"] = cell->has_grid() ? godot::Variant(godot::Vector2i(cell->grid_x(), cell->grid_y()))
                                   : godot::Variant();
    out["water_height"] = static_cast<double>(cell->water_height()) * UNIT_SCALE;
    out["ref_count"] = static_cast<std::int64_t>(cell->refs() ? cell->refs()->size() : 0);
    out["door_count"] = static_cast<std::int64_t>(cell->doors() ? cell->doors()->size() : 0);
    out["lighting_template"] = static_cast<std::int64_t>(cell->lighting_template());
    if (const auto* l = cell->lighting(); l != nullptr && cell->has_lighting()) {
        Dictionary lighting;
        lighting["ambient"] = unpack_color(l->ambient());
        lighting["directional"] = unpack_color(l->directional());
        lighting["directional_rotation_xy"] = l->directional_rotation_xy();
        lighting["directional_rotation_z"] = l->directional_rotation_z();
        lighting["directional_fade"] = l->directional_fade();
        lighting["fog_near_color"] = unpack_color(l->fog_near_color());
        lighting["fog_far_color"] = unpack_color(l->fog_far_color());
        lighting["fog_near"] = static_cast<double>(l->fog_near()) * UNIT_SCALE;
        lighting["fog_far"] = static_cast<double>(l->fog_far()) * UNIT_SCALE;
        lighting["fog_power"] = l->fog_power();
        lighting["fog_max"] = l->fog_max();
        lighting["light_fade_begin"] = static_cast<double>(l->light_fade_begin()) * UNIT_SCALE;
        lighting["light_fade_end"] = static_cast<double>(l->light_fade_end()) * UNIT_SCALE;
        lighting["inherit"] = static_cast<std::int64_t>(l->inherit());
        out["lighting"] = lighting;
    } else {
        out["lighting"] = godot::Variant();
    }
    // Format 10: the directional ambient (XCLL, or its lighting template's)
    // as six Colors, x+, x-, y+, y-, z+, z-; and the image space.
    godot::Array ambient;
    if (const auto* d = cell->directional_ambient(); d != nullptr && d->size() >= 6) {
        for (flatbuffers::uoffset_t i = 0; i < 6; ++i) {
            ambient.push_back(unpack_color(d->Get(i)));
        }
    }
    out["directional_ambient"] = ambient;
    out["image_space"] = static_cast<std::int64_t>(cell->image_space());
    return out;
}

Dictionary SkydotWorld::get_image_space(std::int64_t id) const {
    Dictionary out;
    const auto* list = world_fb() != nullptr ? world_fb()->image_spaces() : nullptr;
    const auto* is = id != 0 ? lookup(list, static_cast<std::uint32_t>(id)) : nullptr;
    if (is == nullptr) {
        return out;
    }
    const auto floats = [](const flatbuffers::Vector<float>* v) {
        godot::PackedFloat32Array a;
        if (v != nullptr) {
            for (const float f : *v) {
                a.push_back(f);
            }
        }
        return a;
    };
    out["id"] = static_cast<std::int64_t>(is->id());
    out["editor_id"] = to_godot(is->editor_id());
    out["hdr"] = floats(is->hdr());
    out["cinematic"] = floats(is->cinematic());
    out["tint"] = floats(is->tint());
    return out;
}

Array SkydotWorld::get_refs(std::int64_t cell_id) const {
    Array out;
    const auto* cell = data().cell_ptr(cell_id);
    if (cell == nullptr || cell->refs() == nullptr) {
        return out;
    }
    for (const auto* ref : *cell->refs()) {
        const auto& p = ref->position();
        const auto& r = ref->rotation();
        Dictionary entry;
        entry["id"] = static_cast<std::int64_t>(ref->id());
        entry["base"] = static_cast<std::int64_t>(ref->base());
        entry["transform"] = skyrim_transform(Vector3(p.x(), p.y(), p.z()),
                                              Vector3(r.x(), r.y(), r.z()), static_cast<double>(ref->scale()));
        entry["scale"] = ref->scale();
        entry["disabled"] = data().initially_disabled(*ref);
        entry["persistent"] = formats::has_flag(ref->flags(), wfb::RefFlags::persistent);
        entry["enable_parent"] = static_cast<std::int64_t>(ref->enable_parent());
        out.push_back(entry);
    }
    return out;
}

Dictionary SkydotWorld::get_base(std::int64_t id) const {
    Dictionary out;
    const auto* base = data().base_ptr(id);
    if (base == nullptr) {
        return out;
    }
    const auto type = base->type();
    const char chars[5] = {static_cast<char>(type & 0xFF), static_cast<char>((type >> 8) & 0xFF),
                           static_cast<char>((type >> 16) & 0xFF),
                           static_cast<char>((type >> 24) & 0xFF), 0};
    out["id"] = static_cast<std::int64_t>(base->id());
    out["type"] = String(chars);
    out["editor_id"] = to_godot(base->editor_id());
    out["model"] = to_godot(base->model());
    if (const auto* l = base->light(); l != nullptr && base->has_light()) {
        Dictionary light;
        light["radius"] = static_cast<double>(l->radius()) * UNIT_SCALE;
        light["color"] = unpack_color(l->color());
        light["flags"] = static_cast<std::int64_t>(l->flags());
        light["falloff_exponent"] = l->falloff_exponent();
        light["fov"] = l->fov();
        light["fade"] = l->fade();
        out["light"] = light;
    } else {
        out["light"] = godot::Variant();
    }
    out["flags"] = static_cast<std::int64_t>(base->flags());
    out["scripts"] = script_list(base->scripts(), false);
    return out;
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

namespace {

/// The point of triangle (a, b, c) nearest `p` (Ericson, Real-Time Collision
/// Detection, 5.1.5).
using godot::real_t;

Vector3 closest_on_triangle(const Vector3& p, const Vector3& a, const Vector3& b, const Vector3& c) {
    const Vector3 ab = b - a;
    const Vector3 ac = c - a;
    const Vector3 ap = p - a;
    const real_t d1 = ab.dot(ap);
    const real_t d2 = ac.dot(ap);
    if (d1 <= 0 && d2 <= 0) {
        return a;
    }
    const Vector3 bp = p - b;
    const real_t d3 = ab.dot(bp);
    const real_t d4 = ac.dot(bp);
    if (d3 >= 0 && d4 <= d3) {
        return b;
    }
    const real_t vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        return a + ab * (d1 / (d1 - d3));
    }
    const Vector3 cp = p - c;
    const real_t d5 = ab.dot(cp);
    const real_t d6 = ac.dot(cp);
    if (d6 >= 0 && d5 <= d6) {
        return c;
    }
    const real_t vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        return a + ac * (d2 / (d2 - d6));
    }
    const real_t va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const real_t denom = 1 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

} // namespace

Vector3 SkydotWorld::nearest_nav_point(std::int64_t space, const Vector3& position, double reach) const {
    std::vector<const wfb::Cell*> cells;
    const auto* s = data().cell_ptr(space);
    if (s != nullptr &&
        (formats::has_flag(s->flags(), wfb::CellFlags::interior) || s->world() == 0)) {
        cells.push_back(s);
    } else {
        // The cell the point is in, and its neighbours only as far as `reach`.
        const auto r = static_cast<float>(reach);
        const auto x0 = static_cast<std::int32_t>(std::floor((position.x - r) / k_cell_units));
        const auto x1 = static_cast<std::int32_t>(std::floor((position.x + r) / k_cell_units));
        const auto y0 = static_cast<std::int32_t>(std::floor((position.y - r) / k_cell_units));
        const auto y1 = static_cast<std::int32_t>(std::floor((position.y + r) / k_cell_units));
        for (auto y = y0; y <= y1; ++y) {
            for (auto x = x0; x <= x1; ++x) {
                if (const auto* c = data().exterior_ptr(static_cast<std::uint32_t>(space), x, y)) {
                    cells.push_back(c);
                }
            }
        }
    }
    Vector3 best = position;
    auto best_d = static_cast<godot::real_t>(reach * reach);
    for (const auto* cell : cells) {
        const auto* navs = cell->navmeshes();
        if (navs == nullptr) {
            continue;
        }
        for (const auto* nav : *navs) {
            const auto* verts = nav->vertices();
            const auto* tris = nav->triangles();
            if (verts == nullptr || tris == nullptr) {
                continue;
            }
            const auto vertex = [&](std::int32_t i) {
                const auto* v = verts->Get(static_cast<flatbuffers::uoffset_t>(i));
                return Vector3(v->x(), v->y(), v->z());
            };
            const auto n = static_cast<std::int32_t>(verts->size());
            for (const auto* t : *tris) {
                if (t->v0() >= n || t->v1() >= n || t->v2() >= n) {
                    continue;
                }
                const Vector3 a = vertex(t->v0());
                const Vector3 b = vertex(t->v1());
                const Vector3 c = vertex(t->v2());
                // Its box is no nearer than the best so far: skip the exact test.
                const godot::real_t dx = std::max({std::min({a.x, b.x, c.x}) - position.x, godot::real_t(0),
                                                   position.x - std::max({a.x, b.x, c.x})});
                const godot::real_t dy = std::max({std::min({a.y, b.y, c.y}) - position.y, godot::real_t(0),
                                                   position.y - std::max({a.y, b.y, c.y})});
                if (dx * dx + dy * dy > best_d) {
                    continue;
                }
                const Vector3 q = closest_on_triangle(position, a, b, c);
                const godot::real_t d = q.distance_squared_to(position);
                if (d < best_d) {
                    best_d = d;
                    best = q;
                }
            }
        }
    }
    return best;
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

godot::PackedInt64Array SkydotWorld::get_cell_actors(std::int64_t cell) const {
    godot::PackedInt64Array out;
    if (const auto* actors = data().cell_actors(static_cast<std::uint32_t>(cell))) {
        for (const auto* a : *actors) {
            out.push_back(a->ref());
        }
    }
    return out;
}

std::int64_t SkydotWorld::wake_clutter(godot::Node* root, const Vector3& centre, double radius) {
    return skydot::wake_clutter(root, centre, static_cast<godot::real_t>(radius));
}

godot::Array SkydotWorld::get_navmeshes(std::int64_t cell_id) const {
    Array out;
    const auto* cell = data().cell_ptr(cell_id);
    if (cell == nullptr || cell->navmeshes() == nullptr) {
        return out;
    }
    for (const auto* nav : *cell->navmeshes()) {
        out.push_back(navmesh_info(*nav, *cell, data().navmeshes()));
    }
    return out;
}

Dictionary SkydotWorld::get_navmesh(std::int64_t id) const {
    const auto& index = data().navmeshes();
    const auto it = index.find(static_cast<std::uint32_t>(id));
    if (it == index.end()) {
        return {};
    }
    return navmesh_info(*it->second.first, *it->second.second, index);
}

// ---- exteriors ------------------------------------------------------------

Array SkydotWorld::list_worlds() const {
    Array out;
    const auto* worlds = world_fb() != nullptr ? world_fb()->worlds() : nullptr;
    if (worlds == nullptr) {
        return out;
    }
    for (const auto* w : *worlds) {
        Dictionary entry;
        entry["id"] = static_cast<std::int64_t>(w->id());
        entry["editor_id"] = to_godot(w->editor_id());
        entry["parent"] = static_cast<std::int64_t>(w->parent());
        entry["land_world"] = static_cast<std::int64_t>(data().land_world(w->id()));
        entry["default_water_height"] =
            w->has_defaults() ? godot::Variant(static_cast<double>(w->default_water_height()) *
                                               UNIT_SCALE)
                              : godot::Variant();
        entry["bounds"] = godot::Rect2(w->min_x(), w->min_y(), w->max_x() - w->min_x(),
                                       w->max_y() - w->min_y());
        out.push_back(entry);
    }
    return out;
}

std::int64_t SkydotWorld::find_world(const String& editor_id) const {
    const auto* worlds = world_fb() != nullptr ? world_fb()->worlds() : nullptr;
    if (worlds == nullptr) {
        return 0;
    }
    const String wanted = editor_id.to_lower();
    for (const auto* w : *worlds) {
        if (to_godot(w->editor_id()).to_lower() == wanted) {
            return w->id();
        }
    }
    return 0;
}

std::int64_t SkydotWorld::get_exterior_cell(std::int64_t world, std::int64_t x,
                                            std::int64_t y) const {
    const auto* cell = data().exterior_ptr(static_cast<std::uint32_t>(world), static_cast<std::int32_t>(x),
                                    static_cast<std::int32_t>(y));
    return cell != nullptr ? cell->id() : 0;
}

std::int64_t SkydotWorld::find_weather(const String& editor_id) const {
    const auto* weathers = world_fb() != nullptr ? world_fb()->weathers() : nullptr;
    if (weathers == nullptr) {
        return 0;
    }
    const String wanted = editor_id.to_lower();
    for (const auto* w : *weathers) {
        if (to_godot(w->editor_id()).to_lower() == wanted) {
            return w->id();
        }
    }
    return 0;
}

Dictionary SkydotWorld::get_sky(std::int64_t world, double hour, std::int64_t weather_id) const {
    Dictionary out;
    const auto* ws = data().world_ptr(world);
    const auto* climates = world_fb() != nullptr ? world_fb()->climates() : nullptr;
    const auto* weathers = world_fb() != nullptr ? world_fb()->weathers() : nullptr;
    if (ws == nullptr || climates == nullptr || weathers == nullptr) {
        return out;
    }
    const wfb::Climate* climate = lookup(climates, ws->climate());
    if (climate == nullptr && ws->parent() != 0) {
        if (const auto* parent = data().world_ptr(ws->parent())) {
            climate = lookup(climates, parent->climate());
        }
    }
    const wfb::Weather* weather =
        weather_id != 0 ? lookup(weathers, static_cast<std::uint32_t>(weather_id)) : nullptr;
    if (weather == nullptr && climate != nullptr && climate->weathers() != nullptr) {
        std::int32_t best = -1;
        for (const auto* entry : *climate->weathers()) {
            if (entry->chance() > best) {
                if (const auto* w = lookup(weathers, entry->weather())) {
                    weather = w;
                    best = entry->chance();
                }
            }
        }
    }
    const auto* colors = weather != nullptr ? weather->colors() : nullptr;
    if (colors == nullptr || colors->size() < 68) {
        return out;
    }

    // Times of day: sunrise, day, sunset, night. Keys at the start, middle and
    // end of sunrise and sunset; linear in between.
    float sun[4] = {5.5F, 10.0F, 16.0F, 20.5F};
    if (climate != nullptr) {
        sun[0] = climate->sunrise_begin();
        sun[1] = climate->sunrise_end();
        sun[2] = climate->sunset_begin();
        sun[3] = climate->sunset_end();
    }
    const float h = static_cast<float>(std::fmod(std::fmod(hour, 24.0) + 24.0, 24.0));
    struct Key {
        float hour;
        int time;
    };
    const Key keys[] = {{sun[0], 3},
                        {(sun[0] + sun[1]) / 2, 0},
                        {sun[1], 1},
                        {sun[2], 1},
                        {(sun[2] + sun[3]) / 2, 2},
                        {sun[3], 3}};
    int from = 3;
    int to = 3;
    float t = 0.0F;
    for (std::size_t i = 0; i + 1 < std::size(keys); ++i) {
        if (h >= keys[i].hour && h <= keys[i + 1].hour) {
            from = keys[i].time;
            to = keys[i + 1].time;
            const float span = keys[i + 1].hour - keys[i].hour;
            t = span > 0.0F ? (h - keys[i].hour) / span : 0.0F;
            break;
        }
    }
    const auto colour = [&](int index) {
        const Color a = unpack_color(colors->Get(static_cast<flatbuffers::uoffset_t>(index * 4 + from)));
        const Color b = unpack_color(colors->Get(static_cast<flatbuffers::uoffset_t>(index * 4 + to)));
        return a.lerp(b, t);
    };
    const auto weight = [](int time) { return time == 1 ? 1.0F : time == 3 ? 0.0F : 0.5F; };
    const float daylight = weight(from) + (weight(to) - weight(from)) * t;

    out["weather"] = to_godot(weather->editor_id());
    out["sky_upper"] = colour(0);
    out["fog_near_color"] = colour(1);
    out["ambient"] = colour(3);
    out["sunlight"] = colour(4);
    out["sky_lower"] = colour(7);
    out["horizon"] = colour(8);
    out["fog_far_color"] = colour(12);
    out["daylight"] = daylight;
    if (const auto* fog = weather->fog(); fog != nullptr && fog->size() >= 8) {
        const auto mix = [&](flatbuffers::uoffset_t day, flatbuffers::uoffset_t night) {
            return static_cast<double>(fog->Get(night) + (fog->Get(day) - fog->Get(night)) * daylight);
        };
        out["fog_near"] = mix(0, 2) * UNIT_SCALE;
        out["fog_far"] = mix(1, 3) * UNIT_SCALE;
        out["fog_power"] = mix(4, 5);
        out["fog_max"] = mix(6, 7);
    }

    // The sun rises in the east (+X), peaks in the south (+Z) and sets in the
    // west; between sunset and sunrise the moon takes its place.
    const float pi = std::numbers::pi_v<float>;
    const bool day = h >= sun[0] && h <= sun[3];
    float phase = 0.0F;
    if (day) {
        phase = (h - sun[0]) / std::max(sun[3] - sun[0], 0.1F);
    } else {
        const float night_length = 24.0F - (sun[3] - sun[0]);
        phase = std::fmod(h - sun[3] + 24.0F, 24.0F) / std::max(night_length, 0.1F);
    }
    const float azimuth = pi * phase; // 0 east, pi west
    const float elevation = std::sin(pi * phase) * pi * 0.38F + 0.05F;
    out["sun_direction"] = Vector3(std::cos(azimuth) * std::cos(elevation), std::sin(elevation),
                                   std::sin(azimuth) * std::cos(elevation))
                               .normalized();
    return out;
}

} // namespace skydot
