// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/world.hpp"
#include "world/fb_search.hpp"

#include "world/actor.hpp"
#include "world/actor_animation.hpp"
#include "world/actors.hpp"
#include "world/animator.hpp"
#include "world/billboard.hpp"
#include "world/collision.hpp"
#include "world/effect_asset.hpp"
#include "world/flicker.hpp"
#include "world/refs.hpp"

#include "skydot_formats/flags.hpp"
#include "skydot_formats/units.hpp"
#include "world_generated.h"

#include <godot_cpp/classes/animation_library.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>

using godot::Array;
using godot::Basis;
using godot::Color;
using godot::Dictionary;
using godot::Error;
using godot::String;
using godot::Transform3D;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {

namespace {

// Shadow casters among the lights (the hemisphere shadow light is drawn as an
// omni light with shadows).
constexpr wfb::LightFlags k_light_shadow = wfb::LightFlags::spot_shadow |
                                           wfb::LightFlags::hemisphere_shadow |
                                           wfb::LightFlags::omni_shadow;

/// RGBA bytes packed little-endian (red in the low byte); alpha is unused.
Color unpack_color(std::uint32_t rgba) {
    return Color(static_cast<float>(rgba & 0xFFu) / 255.0F,
                 static_cast<float>((rgba >> 8) & 0xFFu) / 255.0F,
                 static_cast<float>((rgba >> 16) & 0xFFu) / 255.0F);
}

String hex_id(std::uint32_t id) {
    return String("0x") + String::num_uint64(id, 16, true).lpad(8, "0");
}

String to_godot(const flatbuffers::String* s) {
    return s == nullptr ? String() : String::utf8(s->c_str(), static_cast<int>(s->size()));
}

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

/// Editor markers, which the game does not draw: bases flagged as markers
/// (XMarker, furniture markers), and the helpers in `meshes/markers/` and
/// `meshes/marker*.nif` that some unflagged activators use. Not every model
/// named "marker": crafting stations (BlacksmithForgeMarker.nif,
/// TanningRackMarker.nif) are seen in the game, and the converter has already
/// dropped their EditorMarker parts.
bool is_marker(const wfb::Base& base) {
    if (formats::has_flag(base.record_flags(), wfb::RecordFlags::editor_marker)) {
        return true;
    }
    const auto* model = base.model();
    if (model == nullptr) {
        return false;
    }
    const std::string_view path = model->string_view();
    return path.starts_with("meshes/markers/") ||
           (path.starts_with("meshes/marker") && path.find('/', 7) == std::string_view::npos);
}

/// A model's virtual path as the asset cache keys it.
String model_path(std::string_view model) {
    return String::utf8(model.data(), static_cast<int>(model.size()));
}

/// Game units per exterior cell side.
constexpr auto k_cell_units = static_cast<float>(formats::k_cell_units);

/// XCLW values this large mean "no water here".
constexpr float k_no_water = 1.0e30F;

/// Z-up to Y-up: -90 degrees about X, as the converter's mesh writer does.
const Basis& axis_conversion() {
    static const Basis basis(Vector3(1, 0, 0), -std::numbers::pi_v<godot::real_t> / 2);
    return basis;
}

/// The game places a model by its reference alone: whatever transform the
/// NIF's root node carries is replaced (Riverwood Trader's corner counter
/// piece has its root turned 90 degrees and lines up only without it).
void drop_root_transform(godot::Node3D* model) {
    auto* axes = godot::Object::cast_to<godot::Node3D>(model->get_node_or_null("bethconv_z_up_to_y_up"));
    if (axes == nullptr || axes->get_child_count() == 0) {
        return;
    }
    if (auto* nif_root = godot::Object::cast_to<godot::Node3D>(axes->get_child(0))) {
        nif_root->set_transform(Transform3D());
    }
}

/// Metres a BSOrderedNode's child is moved forward per place in its order
/// when transparent surfaces are sorted: more than the depth between a
/// flask's glass and the liquid in it.
constexpr godot::real_t k_draw_order_step = 0.25F;

void offset_sorting(godot::Node* node, godot::real_t offset, int depth) {
    if (depth > 64) {
        return;
    }
    // A nested ordered node orders its own children within this place.
    const godot::Variant order = bethconv_extras(node).get("draw_order", godot::Variant());
    if (order.get_type() == godot::Variant::INT || order.get_type() == godot::Variant::FLOAT) {
        offset += static_cast<godot::real_t>(static_cast<double>(order)) * k_draw_order_step;
    }
    if (auto* visual = godot::Object::cast_to<godot::VisualInstance3D>(node); visual != nullptr && offset != 0) {
        visual->set_sorting_offset(offset);
    }
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        offset_sorting(node->get_child(i), offset, depth + 1);
    }
}

/// The game draws a BSOrderedNode's children in their order (glass, the
/// liquid in it, the glass around it), not by depth.
void apply_draw_order(godot::Node3D* model) { offset_sorting(model, 0, 0); }

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

Vector3 SkydotWorld::skyrim_position(const Vector3& position) {
    return axis_conversion().xform(position * static_cast<godot::real_t>(UNIT_SCALE));
}

Vector3 SkydotWorld::godot_to_skyrim(const Vector3& position) {
    return axis_conversion().transposed().xform(position) / static_cast<godot::real_t>(UNIT_SCALE);
}

Transform3D SkydotWorld::skyrim_transform(const Vector3& position, const Vector3& rotation,
                                          double scale) {
    // Skyrim rotations are clockwise (negative in a right-handed frame) and
    // applied Z first, then Y, then X; see docs/coordinates.md.
    const Basis rx(Vector3(1, 0, 0), -rotation.x);
    const Basis ry(Vector3(0, 1, 0), -rotation.y);
    const Basis rz(Vector3(0, 0, 1), -rotation.z);
    const Basis skyrim = rx * ry * rz;
    const Basis& c = axis_conversion();
    Basis basis = c * skyrim * c.transposed();
    basis.scale(Vector3(1, 1, 1) * static_cast<godot::real_t>(scale));
    return Transform3D(basis, skyrim_position(position));
}

// ---- building ---------------------------------------------------------------

void SkydotWorld::set_skyrim_materials(bool enabled) { skyrim_materials_ = enabled; }
bool SkydotWorld::get_skyrim_materials() const { return skyrim_materials_; }
void SkydotWorld::set_effects(bool enabled) { effects_ = enabled; }
bool SkydotWorld::get_effects() const { return effects_; }
void SkydotWorld::set_collision(bool enabled) {
    collision_ = enabled;
    if (terrain_) {
        terrain_->set_collision(enabled);
    }
}

struct SkydotWorld::BuildStats {
    std::int64_t refs = 0;
    std::int64_t placed = 0;
    std::int64_t lights = 0;
    std::int64_t markers = 0;
    std::int64_t disabled = 0;
    std::int64_t no_base = 0;
    std::int64_t materials = 0;
    std::int64_t billboards = 0;
    std::int64_t effects = 0;
    std::int64_t flickers = 0;
    std::int64_t bodies = 0;
    std::int64_t actors = 0;
    std::int64_t actor_parts = 0;
    /// Actors not built, by reason ("no NPC_", "no animation skeleton …").
    godot::Dictionary actor_failures;
    godot::PackedStringArray missing;
};

void SkydotWorld::use_water_material(godot::Node* model, std::uint32_t cell) const {
    godot::TypedArray<godot::Node> meshes = model->find_children("*", "MeshInstance3D", true, false);
    godot::Ref<godot::ShaderMaterial> water;
    for (int i = 0; i < meshes.size(); ++i) {
        auto* instance = godot::Object::cast_to<godot::MeshInstance3D>(meshes[i]);
        if (instance == nullptr || instance->get_mesh().is_null()) {
            continue;
        }
        for (int s = 0; s < instance->get_mesh()->get_surface_count(); ++s) {
            const godot::Ref<godot::Material> m = instance->get_surface_override_material(s);
            if (m.is_null() || !m->has_meta("skydot_water")) {
                continue;
            }
            if (water.is_null()) {
                // The activator's own water type (ACTI WNAM) is not in
                // world.fb yet: the cell's, else its worldspace's.
                const auto load = [&](const std::string& vpath) -> godot::Ref<godot::Texture> {
                    return resource(String::utf8(vpath.c_str()));
                };
                const auto space = static_cast<std::uint32_t>(get_cell_space(cell));
                water = water_.material(data().water_ptr(data().water_type(space, data().cell_ptr(cell))), load);
            }
            instance->set_surface_override_material(s, water);
        }
    }
}

void SkydotWorld::place_ref(godot::Node3D* root, const wfb::Ref& ref, std::uint32_t cell,
                            BuildStats& stats, bool include_disabled) const {
    ++stats.refs;
    if (!include_disabled && data().initially_disabled(ref)) {
        ++stats.disabled;
        return;
    }
    const auto* base = data().base_ptr(ref.base());
    if (base == nullptr) {
        ++stats.no_base;
        return;
    }
    const auto& p = ref.position();
    const auto& r = ref.rotation();
    const Transform3D transform = skyrim_transform(Vector3(p.x(), p.y(), p.z()),
                                                   Vector3(r.x(), r.y(), r.z()), static_cast<double>(ref.scale()));
    const String name = hex_id(ref.id()) + " " + to_godot(base->editor_id());

    const auto* model = base->model();
    if (model != nullptr && model->size() != 0) {
        if (is_marker(*base)) {
            ++stats.markers;
        } else {
            const String scene_path = model_path(model->string_view());
            const godot::Ref<SkydotModel> scene = resource(scene_path);
            if (scene.is_valid()) {
                auto* node = godot::Object::cast_to<godot::Node3D>(scene->instantiate());
                if (node != nullptr) {
                    node->set_name(name);
                    node->set_transform(transform);
                    drop_root_transform(node);
                    apply_draw_order(node);
                    if (skyrim_materials_) {
                        stats.materials += materials().apply(node);
                        use_water_material(node, cell);
                        use_directional_material(node, *base);
                    }
                    if (skyrim_materials_ && effects_) {
                        stats.effects += attach_addons(node);
                    }
                    stats.billboards += SkydotBillboard::attach(node);
                    if (effects_) {
                        (void)materials();
                        stats.effects += SkydotAnimator::attach(node, materials_);
                    }
                    tag_ref(node, ref.id(), cell, activatable(base, cell, ref.id()));
                    // Doors that swing rather than lead somewhere: actors open
                    // them in their way (SkydotActor).
                    if (base != nullptr && door_type(base->type()) && !data().doors().contains(ref.id())) {
                        node->set_meta("skydot_plain_door", true);
                    }
                    // Last, so material and effect passes never see the bodies.
                    if (const auto& collision = scene->collision(); collision && collision_) {
                        stats.bodies += collision->attach(node);
                    }
                    root->add_child(node);
                    ++stats.placed;
                }
            } else if (!stats.missing.has(scene_path)) {
                stats.missing.push_back(scene_path);
            }
        }
    }

    if (const auto* l = base->light(); l != nullptr && base->has_light()) {
        if (formats::has_flag(l->flags(), wfb::LightFlags::off_by_default)) {
            return;
        }
        godot::Light3D* light = nullptr;
        godot::Transform3D placed = transform.orthonormalized();
        // The reference's own settings over the record's: XRDS adds to the
        // radius (vanilla interiors mostly shrink it: 512 - 261 in the inn),
        // XLIG's fade multiplies the brightness (0.5 on the farmhouse lights,
        // 2 on their fire lights) and its FOV adds to a spotlight's. Fade as
        // a factor matched five comparison shots best (total error 0.070,
        // against 0.098 as an offset and 0.114 ignored; COMPARISON-SHOTS.md).
        const wfb::LightOverride* own = nullptr;
        if (const auto* c = data().cell_ptr(cell); c != nullptr && c->light_overrides() != nullptr) {
            own = lookup(c->light_overrides(), ref.id());
        }
        float radius = static_cast<float>(l->radius());
        if (own != nullptr && own->has_radius()) {
            radius = std::max(radius + own->radius(), 1.0F);
        }
        float fade = l->fade() > 0.0F ? l->fade() : 1.0F;
        float fov = l->fov();
        if (own != nullptr && own->has_light_data()) {
            fade *= own->fade();
            fov += own->fov();
        }
        const auto range = static_cast<float>(static_cast<double>(radius) * UNIT_SCALE);
        if (formats::has_flag(l->flags(), wfb::LightFlags::spot_light)) {
            auto* spot = memnew(godot::SpotLight3D);
            spot->set_param(godot::Light3D::PARAM_RANGE, range);
            spot->set_param(godot::Light3D::PARAM_SPOT_ANGLE, std::clamp(fov / 2.0F, 1.0F, 89.0F));
            spot->set_param(godot::Light3D::PARAM_ATTENUATION, 0.0F);
            const bool spot_shadow = formats::has_flag(l->flags(), wfb::LightFlags::spot_shadow);
            spot->set_shadow(all_light_shadows_ || spot_shadow);
            spot->set_meta("skydot_game_shadow", spot_shadow);
            light = spot;
        } else {
            auto* omni = memnew(godot::OmniLight3D);
            omni->set_param(godot::Light3D::PARAM_RANGE, range);
            // The game fades a light by 1 - (d / radius)^2 (Community
            // Shaders, Lighting.hlsl); Godot's window alone, (1 - (d /
            // range)^4)^2, is within 0.13 of it. A distance exponent on top
            // left fires dim a few metres away.
            omni->set_param(godot::Light3D::PARAM_ATTENUATION, 0.0F);
            // Every light casts shadows: the game keeps lights out of
            // neighbouring rooms with its rooms and portals, which SkyDot
            // lacks; without shadows they light through walls (comparison
            // shot ref22: an inn room twice as bright as the game's).
            omni->set_shadow(all_light_shadows_ || formats::has_flag(l->flags(), k_light_shadow));
            omni->set_meta("skydot_game_shadow", formats::has_flag(l->flags(), k_light_shadow));
            light = omni;
        }
        light->set_name(name + String(" light"));
        SkydotMaterials::set_game_light(light, unpack_color(l->color()) * std::max(fade, 0.0F));
        light->set_negative(formats::has_flag(l->flags(), wfb::LightFlags::negative));
        light->set_transform(placed);
        if (effects_ && formats::has_flag(l->flags(), SkydotFlicker::ANY)) {
            auto* flicker = memnew(SkydotFlicker);
            flicker->set_name("SkydotFlicker");
            flicker->configure(static_cast<std::int64_t>(l->flags()),
                               static_cast<double>(l->flicker_period()),
                               static_cast<double>(l->flicker_intensity()),
                               static_cast<double>(l->flicker_movement()) * UNIT_SCALE);
            light->add_child(flicker);
            ++stats.flickers;
        }
        root->add_child(light);
        ++stats.lights;
    }
}

std::int64_t SkydotWorld::apply_light_shadows(godot::Node* root, bool all) {
    if (root == nullptr) {
        return 0;
    }
    std::int64_t changed = 0;
    const godot::TypedArray<godot::Node> lights = root->find_children("*", "Light3D", true, false);
    for (int64_t i = 0; i < lights.size(); ++i) {
        auto* light = godot::Object::cast_to<godot::Light3D>(lights[i]);
        if (light == nullptr || !light->has_meta("skydot_game_shadow")) {
            continue;
        }
        const bool shadow = all || static_cast<bool>(light->get_meta("skydot_game_shadow"));
        if (light->has_shadow() != shadow) {
            light->set_shadow(shadow);
            ++changed;
        }
    }
    return changed;
}

std::int64_t SkydotWorld::attach_addons(godot::Node* model) const {
    std::call_once(addon_index_once_, [this] {
        if (const auto* list = world_fb() != nullptr ? world_fb()->addon_nodes() : nullptr) {
            for (const auto* a : *list) {
                if (a->model() != nullptr && a->model()->size() != 0) {
                    addon_models_.emplace(a->index(), a->model()->str());
                }
            }
        }
    });
    if (addon_models_.empty()) {
        return 0;
    }
    std::int64_t attached = 0;
    const godot::TypedArray<godot::Node> nodes = model->find_children("AddOnNode*", "Node3D", true, false);
    for (int64_t i = 0; i < nodes.size(); ++i) {
        auto* node = godot::Object::cast_to<godot::Node3D>(nodes[i]);
        if (node == nullptr || !node->has_meta("extras")) {
            continue;
        }
        const godot::Variant extras = node->get_meta("extras");
        const godot::Variant block = extras.get_type() == godot::Variant::DICTIONARY ? Dictionary(extras).get("bethconv", godot::Variant())
                                                                       : godot::Variant();
        const godot::Variant index = block.get_type() == godot::Variant::DICTIONARY ? Dictionary(block).get("addon", godot::Variant())
                                                                      : godot::Variant();
        if (index.get_type() != godot::Variant::INT && index.get_type() != godot::Variant::FLOAT) {
            continue;
        }
        const auto found = addon_models_.find(static_cast<std::int32_t>(static_cast<std::int64_t>(index)));
        if (found == addon_models_.end()) {
            continue;
        }
        const godot::Ref<SkydotModel> scene = resource(model_path(found->second));
        auto* addon = scene.is_valid() ? godot::Object::cast_to<godot::Node3D>(scene->instantiate()) : nullptr;
        if (addon == nullptr) {
            continue;
        }
        addon->set_name("AddOn");
        drop_root_transform(addon);
        apply_draw_order(addon);
        materials().apply(addon);
        SkydotBillboard::attach(addon);
        SkydotAnimator::attach(addon, materials_);
        // The AddOnNode sits in the model's NIF space (under its axis and
        // unit conversion), and the addon brings its own conversion: hang it
        // from the model's root where the AddOnNode is, without converting
        // twice.
        godot::Transform3D at;
        for (godot::Node* n = node; n != nullptr && n != model; n = n->get_parent()) {
            if (auto* spatial = godot::Object::cast_to<godot::Node3D>(n)) {
                at = spatial->get_transform() * at;
            }
        }
        if (auto* own = godot::Object::cast_to<godot::Node3D>(addon->get_node_or_null("bethconv_z_up_to_y_up"))) {
            at = at * own->get_transform().affine_inverse();
        }
        addon->set_transform(at);
        model->add_child(addon);
        ++attached;
    }
    return attached;
}

GrassModel SkydotWorld::grass_model(const wfb::Grass& grass) const {
    const std::scoped_lock lock(grass_mutex_);
    if (auto it = grass_models_.find(grass.id()); it != grass_models_.end()) {
        return it->second;
    }
    GrassModel out;
    const auto* model = grass.model();
    godot::Ref<SkydotModel> scene;
    if (model != nullptr && model->size() != 0) {
        scene = resource(model_path(model->string_view()));
    }
    godot::Node* node = scene.is_valid() ? scene->instantiate() : nullptr;
    if (node != nullptr) {
        materials().apply(node);
        const godot::TypedArray<godot::Node> meshes = node->find_children("*", "MeshInstance3D", true, false);
        for (int64_t i = 0; i < meshes.size() && out.mesh.is_null(); ++i) {
            auto* instance = godot::Object::cast_to<godot::MeshInstance3D>(meshes[i]);
            if (instance == nullptr || instance->get_mesh().is_null()) {
                continue;
            }
            // The mesh's place inside the model, and its converted materials
            // on a copy (a MultiMesh draws the mesh's own materials).
            for (godot::Node* n = instance; n != nullptr && n != node; n = n->get_parent()) {
                if (auto* spatial = godot::Object::cast_to<godot::Node3D>(n)) {
                    out.local = spatial->get_transform() * out.local;
                }
            }
            godot::Ref<godot::ArrayMesh> copy = instance->get_mesh()->duplicate();
            if (copy.is_valid()) {
                for (int s = 0; s < copy->get_surface_count(); ++s) {
                    if (const godot::Ref<godot::ShaderMaterial> m = instance->get_surface_override_material(s); m.is_valid()) {
                        // The game draws grass with its grass shader, whose
                        // vertex alpha is the wind's weight, not opacity.
                        godot::Ref<godot::ShaderMaterial> own = m->duplicate();
                        own->set_shader_parameter("use_vertex_alpha", false);
                        copy->surface_set_material(s, own);
                    }
                }
                out.mesh = copy;
            }
        }
        memdelete(node);
    }
    grass_models_.emplace(grass.id(), out);
    return out;
}

const ProjectedMaterial* SkydotWorld::projected_material(std::uint32_t id) const {
    const std::scoped_lock lock(projected_mutex_);
    if (auto it = projected_.find(id); it != projected_.end()) {
        return it->second ? &*it->second : nullptr;
    }
    const auto* list = world_fb() != nullptr ? world_fb()->material_objects() : nullptr;
    const auto* mato = lookup(list, id);
    if (mato == nullptr) {
        projected_.emplace(id, std::nullopt);
        return nullptr;
    }
    ProjectedMaterial out;
    // The material's textures are those of its model's first shape. Single
    // pass materials (the common snow) are one colour, as the game's shader
    // draws them without projected textures.
    if (const auto* model = mato->model(); !mato->single_pass() && model != nullptr && model->size() != 0) {
        const godot::Ref<SkydotModel> scene = resource(model_path(model->string_view()));
        if (scene.is_valid()) {
            godot::Node* node = scene->instantiate();
            const godot::TypedArray<godot::Node> meshes = node != nullptr
                ? node->find_children("*", "MeshInstance3D", true, false)
                : godot::TypedArray<godot::Node>();
            for (int64_t i = 0; i < meshes.size() && out.albedo.is_null(); ++i) {
                auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(meshes[i]);
                if (mesh == nullptr || mesh->get_mesh().is_null() || mesh->get_mesh()->get_surface_count() == 0) {
                    continue;
                }
                const godot::Ref<godot::ShaderMaterial> converted =
                    materials().convert(mesh->get_mesh()->surface_get_material(0));
                if (converted.is_valid()) {
                    out.albedo = converted->get_shader_parameter("albedo_tex");
                }
            }
            if (node != nullptr) {
                memdelete(node);
            }
        }
    }
    const auto scale = static_cast<float>(UNIT_SCALE);
    const auto per_metre = [&](float units) { return units > 0.0F ? 1.0F / (units * scale) : 0.0F; };
    out.params = godot::Vector4(mato->falloff_scale(), mato->falloff_bias(), per_metre(mato->noise_uv_scale()),
                                per_metre(mato->material_uv_scale()));
    // Projected along the vector: faces turned against it take the material
    // (snow's is straight down).
    if (const auto* p = mato->projection(); p != nullptr && p->size() >= 3) {
        const Vector3 game(-p->Get(0), -p->Get(1), -p->Get(2));
        const Vector3 world(game.x, game.z, -game.y);
        if (world.length() > 0.001F) {
            out.direction = world.normalized();
        }
    }
    Vector3 colour(1, 1, 1);
    if (const auto* c = mato->single_pass_color(); c != nullptr && c->size() >= 3 &&
                                                   (c->Get(0) > 0.0F || c->Get(1) > 0.0F || c->Get(2) > 0.0F)) {
        colour = Vector3(c->Get(0), c->Get(1), c->Get(2));
    }
    out.color = colour;
    out.normal_dampener = mato->normal_dampener();
    return &*projected_.emplace(id, out).first->second;
}

void SkydotWorld::use_directional_material(godot::Node* model, const wfb::Base& base) const {
    if (base.directional_material() == 0) {
        return;
    }
    const ProjectedMaterial* with = projected_material(base.directional_material());
    if (with == nullptr) {
        return;
    }
    const godot::TypedArray<godot::Node> meshes = model->find_children("*", "MeshInstance3D", true, false);
    for (int64_t i = 0; i < meshes.size(); ++i) {
        auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(meshes[i]);
        if (mesh == nullptr || mesh->get_mesh().is_null()) {
            continue;
        }
        for (int s = 0; s < mesh->get_mesh()->get_surface_count(); ++s) {
            const godot::Ref<godot::ShaderMaterial> current = mesh->get_surface_override_material(s);
            const godot::Ref<godot::ShaderMaterial> replaced =
                materials().projected(current, base.directional_material(), *with);
            if (replaced != current) {
                mesh->set_surface_override_material(s, replaced);
            }
        }
    }
}

Dictionary SkydotWorld::stats_dictionary(const BuildStats& stats) const {
    Dictionary out;
    out["refs"] = stats.refs;
    out["placed"] = stats.placed;
    out["lights"] = stats.lights;
    out["markers"] = stats.markers;
    out["disabled"] = stats.disabled;
    out["no_base"] = stats.no_base;
    out["missing"] = stats.missing;
    out["materials"] = stats.materials;
    out["billboards"] = stats.billboards;
    out["effects"] = stats.effects;
    out["flickers"] = stats.flickers;
    out["bodies"] = stats.bodies;
    out["actors"] = stats.actors;
    out["actor_parts"] = stats.actor_parts;
    out["actor_failures"] = stats.actor_failures;
    return out;
}

godot::Node3D* SkydotWorld::build_ref(std::int64_t cell_id, std::int64_t ref_id) const {
    const auto* cell = data().cell_ptr(cell_id);
    const auto* refs = cell != nullptr ? cell->refs() : nullptr;
    if (refs == nullptr) {
        return nullptr;
    }
    const auto id = static_cast<std::uint32_t>(ref_id);
    const auto* it = lookup(refs, id);
    if (it == nullptr) {
        return nullptr;
    }
    auto* root = memnew(godot::Node3D);
    root->set_name(hex_id(id));
    BuildStats stats;
    place_ref(root, *it, cell->id(), stats, true);
    root->set_meta("skydot_stats", stats_dictionary(stats));
    return root;
}

godot::Node3D* SkydotWorld::build_cell(std::int64_t id) const {
    auto* root = begin_cell(id);
    if (root != nullptr) {
        continue_build(root, std::numeric_limits<std::int64_t>::max());
    }
    return root;
}

// ---- where actors are ---------------------------------------------------

std::uint64_t SkydotWorld::place_bucket(const ActorPlace& place) const {
    const auto* cell = data().cell_ptr(place.space);
    if (cell != nullptr && formats::has_flag(cell->flags(), wfb::CellFlags::interior)) {
        return place.space; // An interior: its id (grid keys have a world above bit 32).
    }
    const auto x = static_cast<std::int32_t>(std::floor(place.position.x / k_cell_units));
    const auto y = static_cast<std::int32_t>(std::floor(place.position.y / k_cell_units));
    return grid_key(place.space, x, y);
}

std::uint64_t SkydotWorld::placed_bucket(const wfb::ActorRef& actor) const {
    const auto* cell = data().cell_ptr(actor.cell());
    if (cell == nullptr) {
        return 0;
    }
    if (formats::has_flag(cell->flags(), wfb::CellFlags::interior) || cell->world() == 0) {
        return cell->id();
    }
    const auto x = static_cast<std::int32_t>(std::floor(actor.position().x() / k_cell_units));
    const auto y = static_cast<std::int32_t>(std::floor(actor.position().y() / k_cell_units));
    return grid_key(cell->world(), x, y);
}

std::vector<SkydotWorld::ActorAt> SkydotWorld::actors_in_cell(std::uint32_t cell) const {
    std::vector<ActorAt> out;
    const auto bucket = static_cast<std::uint64_t>(cell);
    const auto stays = [&](const wfb::ActorRef* a) {
        const auto it = actor_places_.find(a->ref());
        return it == actor_places_.end() || place_bucket(it->second) == bucket;
    };
    if (const auto* placed = data().cell_actors(cell)) {
        for (const auto* a : *placed) {
            // An exterior cell's own actors are bucketed by its grid square;
            // interiors by the cell.
            if (actor_places_.contains(a->ref())) {
                if (placed_bucket(*a) == bucket && stays(a)) {
                    out.push_back({a, &actor_places_.at(a->ref())});
                }
                continue;
            }
            out.push_back({a, nullptr});
        }
    }
    if (const auto it = moved_in_.find(bucket); it != moved_in_.end()) {
        for (const auto ref : it->second) {
            const auto* a = data().actor_ptr(ref);
            if (a != nullptr && placed_bucket(*a) != bucket) {
                out.push_back({a, &actor_places_.at(ref)});
            }
        }
    }
    return out;
}

std::vector<SkydotWorld::ActorAt> SkydotWorld::actors_in_grid(std::uint32_t world, std::int32_t x,
                                                              std::int32_t y) const {
    std::vector<ActorAt> out;
    const auto bucket = grid_key(world, x, y);
    const auto add_placed = [&](const std::vector<const wfb::ActorRef*>& list) {
        for (const auto* a : list) {
            const auto it = actor_places_.find(a->ref());
            if (it == actor_places_.end()) {
                out.push_back({a, nullptr});
            } else if (place_bucket(it->second) == bucket) {
                out.push_back({a, &it->second});
            }
        }
    };
    if (const auto* cell = data().exterior_ptr(world, x, y)) {
        if (const auto* placed = data().cell_actors(cell->id())) {
            add_placed(*placed);
        }
    }
    if (const auto* placed = data().persistent_actors(world, x, y)) {
        add_placed(*placed);
    }
    if (const auto it = moved_in_.find(bucket); it != moved_in_.end()) {
        for (const auto ref : it->second) {
            const auto* a = data().actor_ptr(ref);
            if (a != nullptr && placed_bucket(*a) != bucket) {
                out.push_back({a, &actor_places_.at(ref)});
            }
        }
    }
    return out;
}

void SkydotWorld::set_actor_place(std::int64_t ref, std::int64_t space, const Vector3& position,
                                  double rotation_z) {
    clear_actor_place(ref);
    const auto id = static_cast<std::uint32_t>(ref);
    const ActorPlace place{static_cast<std::uint32_t>(space), position, static_cast<float>(rotation_z)};
    actor_places_[id] = place;
    moved_in_[place_bucket(place)].push_back(id);
}

void SkydotWorld::clear_actor_place(std::int64_t ref) {
    const auto id = static_cast<std::uint32_t>(ref);
    const auto it = actor_places_.find(id);
    if (it == actor_places_.end()) {
        return;
    }
    if (const auto in = moved_in_.find(place_bucket(it->second)); in != moved_in_.end()) {
        std::erase(in->second, id);
    }
    actor_places_.erase(it);
}

void SkydotWorld::clear_actor_places() {
    actor_places_.clear();
    moved_in_.clear();
}

std::int64_t SkydotWorld::get_cell_space(std::int64_t id) const {
    const auto* cell = data().cell_ptr(id);
    if (cell == nullptr) {
        return 0;
    }
    return formats::has_flag(cell->flags(), wfb::CellFlags::interior) || cell->world() == 0
               ? cell->id()
               : cell->world();
}

Dictionary SkydotWorld::get_actor_place(std::int64_t ref) const {
    Dictionary out;
    const auto* a = data().actor_ptr(ref);
    if (a == nullptr) {
        return out;
    }
    ActorPlace place{static_cast<std::uint32_t>(get_cell_space(a->cell())),
                     Vector3(a->position().x(), a->position().y(), a->position().z()), a->rotation().z()};
    const auto it = actor_places_.find(a->ref());
    if (it != actor_places_.end()) {
        place = it->second;
    }
    const auto* space = data().cell_ptr(place.space);
    const bool interior =
        space != nullptr && formats::has_flag(space->flags(), wfb::CellFlags::interior);
    std::uint32_t cell = interior ? place.space : 0;
    if (!interior) {
        const auto x = static_cast<std::int32_t>(std::floor(place.position.x / k_cell_units));
        const auto y = static_cast<std::int32_t>(std::floor(place.position.y / k_cell_units));
        if (const auto* c = data().exterior_ptr(place.space, x, y)) {
            cell = c->id();
        }
    }
    out["space"] = static_cast<std::int64_t>(place.space);
    out["interior"] = interior;
    out["cell"] = static_cast<std::int64_t>(cell);
    out["position"] = place.position;
    out["rotation_z"] = static_cast<double>(place.rotation_z);
    out["moved"] = it != actor_places_.end();
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

skydot::ActorPlan plan_for(const wfb::World& world, const wfb::ActorRef& actor,
                           const std::shared_ptr<AssetCache>& assets) {
    const auto exists = [&](const std::string& vpath) { return assets != nullptr && assets->has(vpath); };
    return plan_actor(world, actor.base(), actor.ref(), exists);
}

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

std::vector<std::string> SkydotWorld::actor_resources(const wfb::ActorRef& actor) const {
    if (formats::has_flag(actor.flags(), wfb::RefFlags::initially_disabled)) {
        return {};
    }
    auto plan = plan_for(*world_fb(), actor, assets_);
    std::vector<std::string> out = std::move(plan.parts);
    if (plan.missing.empty() && !plan.skeleton.empty()) {
        const auto clips = actor_clips(plan);
        for (const std::string* file : {&clips.idle, &clips.walk, &clips.run}) {
            if (!file->empty()) {
                out.push_back(AssetCache::clip_key(*file, plan.skeleton));
            }
        }
    }
    return out;
}

SkydotWorld::ActorClips SkydotWorld::actor_clips(const ActorPlan& plan) const {
    // The project's idle, walk and run when the pack has animationdata, else
    // an idle found by file name.
    const Locomotion& loco = locomotion_of(plan.behaviour);
    ActorClips out;
    out.idle = !loco.idle.empty() ? loco.idle.file : plan.idle;
    if (!loco.walk.empty()) {
        out.walk = loco.walk.file;
    }
    if (!loco.run.empty()) {
        out.run = loco.run.file;
    }
    return out;
}

const Locomotion& SkydotWorld::locomotion_of(const std::string& behaviour) const {
    auto it = locomotion_.find(behaviour);
    if (it == locomotion_.end()) {
        const auto bytes = [&](const std::string& vpath) {
            if (assets_ == nullptr) {
                return godot::PackedByteArray();
            }
            if (vpath == "meshes/animationdatasinglefile.txt") {
                if (!animation_single_file_read_) {
                    animation_single_file_ = assets_->bytes(vpath);
                    animation_single_file_read_ = true;
                }
                return animation_single_file_;
            }
            return assets_->bytes(vpath);
        };
        const auto exists = [&](const std::string& vpath) { return assets_ != nullptr && assets_->has(vpath); };
        it = locomotion_.emplace(behaviour, find_locomotion(behaviour, bytes, exists)).first;
    }
    return it->second;
}

godot::Dictionary SkydotWorld::get_locomotion(const String& behaviour) const {
    const Locomotion& l = locomotion_of(behaviour.utf8().get_data());
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

godot::Ref<godot::Animation> SkydotWorld::actor_clip(const std::string& file,
                                                     const std::string& skeleton_path) const {
    if (assets_ == nullptr) {
        return {};
    }
    return assets_->clip(file, skeleton_path);
}

godot::Dictionary SkydotWorld::get_actor_plan(std::int64_t ref) const {
    const auto* actor = data().actor_ptr(ref);
    if (actor == nullptr) {
        return {};
    }
    return plan_dictionary(plan_for(*world_fb(), *actor, assets_));
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

godot::Node3D* SkydotWorld::build_actor(std::int64_t ref) const {
    const auto* actor = data().actor_ptr(ref);
    if (actor == nullptr) {
        return nullptr;
    }
    auto* root = memnew(godot::Node3D);
    root->set_name(hex_id(actor->ref()));
    BuildStats stats;
    const auto moved = actor_places_.find(actor->ref());
    place_actor(root, *actor, stats, moved != actor_places_.end() ? &moved->second : nullptr);
    root->set_meta("skydot_stats", stats_dictionary(stats));
    return root;
}

void SkydotWorld::place_actor(godot::Node3D* root, const wfb::ActorRef& actor, BuildStats& stats,
                              const ActorPlace* place) const {
    if (formats::has_flag(actor.flags(), wfb::RefFlags::initially_disabled)) {
        ++stats.disabled;
        return;
    }
    const auto plan = plan_for(*world_fb(), actor, assets_);
    const auto fail = [&](const String& why) {
        const std::int64_t n = stats.actor_failures.get(why, 0);
        stats.actor_failures[why] = n + 1;
    };
    if (!plan.missing.empty()) {
        fail(String::utf8(plan.missing.c_str()));
        return;
    }
    auto* skeleton = SkydotAnimation::build_skeleton(assets_->bytes(plan.skeleton));
    if (skeleton == nullptr) {
        fail("skeleton does not build");
        return;
    }
    skeleton->set_name("Skeleton");

    const auto* npc = lookup(world_fb()->npcs(), plan.npc);
    auto* node = memnew(SkydotActor);
    node->set_name(hex_id(actor.ref()) + " " + (npc != nullptr ? to_godot(npc->editor_id()) : String()));
    // Actors stand upright: only the rotation about Z counts.
    const auto& p = actor.position();
    const Vector3 spot = place != nullptr ? place->position : Vector3(p.x(), p.y(), p.z());
    const double facing = place != nullptr ? static_cast<double>(place->rotation_z) : static_cast<double>(actor.rotation().z());
    node->set_transform(skyrim_transform(spot, Vector3(0, 0, static_cast<godot::real_t>(facing)), 1.0));
    auto* units = memnew(godot::Node3D);
    units->set_name("bethconv_z_up_to_y_up");
    units->set_transform(godot::Transform3D(
        godot::Basis(Vector3(1, 0, 0), static_cast<float>(-std::numbers::pi / 2))
            .scaled(Vector3(1, 1, 1) * static_cast<float>(UNIT_SCALE) * plan.scale),
        Vector3()));
    node->add_child(units);
    units->add_child(skeleton);

    for (const auto& part : plan.parts) {
        const godot::Ref<SkydotModel> scene = resource(String::utf8(part.c_str()));
        if (scene.is_null()) {
            if (!stats.missing.has(String::utf8(part.c_str()))) {
                stats.missing.push_back(String::utf8(part.c_str()));
            }
            continue;
        }
        godot::Node* model = scene->instantiate();
        if (model == nullptr) {
            continue;
        }
        if (skyrim_materials_) {
            stats.materials += materials().apply(model);
        }
        node->add_child(model);
        stats.actor_parts += SkydotAnimation::attach_skinned(model, skeleton);
        node->remove_child(model);
        memdelete(model);
    }
    // Under a hood or helmet the head's hair shapes ("Hair…", hairlines too)
    // would show through.
    if (plan.hide_hair) {
        for (int i = 0; i < skeleton->get_child_count(); ++i) {
            auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(skeleton->get_child(i));
            if (mesh != nullptr && String(mesh->get_name()).to_lower().begins_with("hair")) {
                mesh->set_visible(false);
            }
        }
    }
    // Body skin takes this actor's tone: its own copy of the shared material.
    const godot::Color tone(plan.skin_tone[0], plan.skin_tone[1], plan.skin_tone[2]);
    for (int i = 0; i < skeleton->get_child_count(); ++i) {
        auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(skeleton->get_child(i));
        if (mesh == nullptr || mesh->get_mesh().is_null()) {
            continue;
        }
        for (int s = 0; s < mesh->get_mesh()->get_surface_count(); ++s) {
            const godot::Ref<godot::ShaderMaterial> m = mesh->get_surface_override_material(s);
            if (m.is_valid() && static_cast<bool>(m->get_shader_parameter("use_skin_tint"))) {
                godot::Ref<godot::ShaderMaterial> own = m->duplicate();
                own->set_shader_parameter("skin_tint", shader_rgb(tone));
                mesh->set_surface_override_material(s, own);
            }
        }
    }

    const Locomotion& loco = locomotion_of(plan.behaviour);
    const ActorClips clips = actor_clips(plan);
    godot::Ref<godot::AnimationLibrary> library;
    library.instantiate();
    if (!clips.idle.empty()) {
        if (const auto clip = actor_clip(clips.idle, plan.skeleton); clip.is_valid()) {
            library->add_animation("idle", clip);
        }
    }
    // Metres per second at playback speed 1; taller actors stride further.
    const double stride = UNIT_SCALE * static_cast<double>(plan.scale);
    double walk_speed = 0.0;
    double run_speed = 0.0;
    if (!clips.walk.empty()) {
        if (const auto clip = actor_clip(clips.walk, plan.skeleton); clip.is_valid()) {
            library->add_animation("walk", clip);
            walk_speed = static_cast<double>(loco.walk.speed) * stride;
        }
    }
    if (!clips.run.empty()) {
        if (const auto clip = actor_clip(clips.run, plan.skeleton); clip.is_valid()) {
            library->add_animation("run", clip);
            run_speed = static_cast<double>(loco.run.speed) * stride;
        }
    }
    if (library->has_animation("idle") || library->has_animation("walk")) {
        auto* player = memnew(godot::AnimationPlayer);
        player->set_name("AnimationPlayer");
        player->add_animation_library("", library);
        if (library->has_animation("idle")) {
            player->set_autoplay("idle");
        }
        node->add_child(player);
    }
    node->set_clip_speeds(walk_speed, run_speed);
    node->set_wander(actor_wander_);
    node->set_seed(static_cast<std::int64_t>(actor.ref()));
    // The body: a cylinder about as wide and tall as the skeleton at rest
    // (skinned meshes' own bounds are not where the bones put them). Bones
    // parked far away for props are left out.
    godot::AABB bounds;
    bool any = false;
    const godot::Transform3D to_node = units->get_transform();
    for (int i = 0; i < skeleton->get_bone_count(); ++i) {
        const godot::Vector3 at = skeleton->get_bone_global_rest(i).origin;
        if (std::abs(at.x) > 1e4F || std::abs(at.y) > 1e4F || std::abs(at.z) > 1e4F) {
            continue;
        }
        const godot::Vector3 bone = to_node.xform(at);
        if (!any) {
            bounds = godot::AABB(bone, godot::Vector3());
            any = true;
        } else {
            bounds.expand_to(bone);
        }
    }
    if (any) {
        const godot::Vector3 size = bounds.get_size();
        // The highest bones (head, magic nodes) are about the top of the head.
        const double top = static_cast<double>(bounds.get_end().y);
        node->set_radius(std::clamp(static_cast<double>(std::min(size.x, size.z)) * 0.5, 0.2, 1.5));
        node->set_height(std::clamp(top, 0.3, 12.0));
        node->set_step_height(std::clamp(0.25 * top, 0.15, 0.6));
    }
    tag_ref(node, actor.ref(), actor.cell(), false);
    root->add_child(node);
    ++stats.actors;
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

namespace {

std::string utf8(const String& s) {
    const auto bytes = s.utf8();
    return {bytes.get_data(), static_cast<std::size_t>(bytes.length())};
}

} // namespace

std::array<std::string, 2> SkydotWorld::land_texture_paths(std::uint32_t id) const {
    const auto* ltex = world_fb() != nullptr ? world_fb()->land_textures() : nullptr;
    if (id == 0 || ltex == nullptr) {
        return {TerrainBuilder::k_default_texture, ""};
    }
    const auto* it = lookup(ltex, id);
    if (it == nullptr) {
        return {TerrainBuilder::k_default_texture, ""};
    }
    const auto str = [](const flatbuffers::String* s) {
        return s != nullptr ? s->str() : std::string{};
    };
    return {str(it->diffuse()), str(it->normal())};
}

godot::Ref<godot::Resource> SkydotWorld::resource(const String& vpath) const {
    return assets_ != nullptr ? assets_->get(utf8(vpath)) : godot::Ref<godot::Resource>();
}

SkydotMaterials& SkydotWorld::materials() const {
    if (materials_.is_null()) {
        materials_.instantiate();
        materials_->set_assets(assets_);
    }
    return *materials_.ptr();
}

void SkydotWorld::add_ref_resources(const wfb::Ref& ref, godot::PackedStringArray& out) const {
    if (data().initially_disabled(ref)) {
        return;
    }
    const auto* base = data().base_ptr(ref.base());
    const auto* model = base != nullptr ? base->model() : nullptr;
    if (model == nullptr || model->size() == 0 || is_marker(*base)) {
        return;
    }
    const String path = model_path(model->string_view());
    if (!out.has(path)) {
        out.push_back(path);
    }
}

void SkydotWorld::add_actor_resources(const std::vector<ActorAt>& actors, godot::PackedStringArray& out) const {
    if (!actors_) {
        return;
    }
    for (const auto& at : actors) {
        for (const auto& part : actor_resources(*at.actor)) {
            const String path = String::utf8(part.c_str());
            if (!out.has(path)) {
                out.push_back(path);
            }
        }
    }
}

godot::PackedStringArray SkydotWorld::get_cell_resources(std::int64_t id) const {
    godot::PackedStringArray out;
    const auto* cell = data().cell_ptr(id);
    if (cell == nullptr) {
        return out;
    }
    if (cell->refs() != nullptr) {
        for (const auto* ref : *cell->refs()) {
            add_ref_resources(*ref, out);
        }
    }
    add_actor_resources(actors_in_cell(cell->id()), out);
    return out;
}

std::int64_t SkydotWorld::request_cell(std::int64_t id) {
    return request_all(get_cell_resources(id));
}

godot::PackedStringArray SkydotWorld::get_exterior_resources(std::int64_t world, std::int64_t x,
                                                             std::int64_t y) const {
    godot::PackedStringArray out;
    const auto w = static_cast<std::uint32_t>(world);
    const auto gx = static_cast<std::int32_t>(x);
    const auto gy = static_cast<std::int32_t>(y);
    if (const auto* cell = data().exterior_ptr(w, gx, gy); cell != nullptr && cell->refs() != nullptr) {
        for (const auto* ref : *cell->refs()) {
            add_ref_resources(*ref, out);
        }
    }
    if (actors_) {
        add_actor_resources(actors_in_grid(w, gx, gy), out);
    }
    if (const auto* persistent = data().persistent_refs(w, gx, gy)) {
        for (const auto* ref : *persistent) {
            add_ref_resources(*ref, out);
        }
    }
    const auto* cell = data().exterior_ptr(w, gx, gy);
    if (const auto* water = data().water_ptr(data().water_type(w, cell));
        water != nullptr && water->noise() != nullptr && water->noise()->size() != 0) {
        const String path = String::utf8(water->noise()->Get(0)->c_str());
        if (!out.has(path)) {
            out.push_back(path);
        }
    }
    const auto* land = data().exterior_ptr(data().land_world(w), gx, gy);
    if (land != nullptr && land->terrain() != nullptr && land->terrain()->layers() != nullptr) {
        for (const auto* layer : *land->terrain()->layers()) {
            // The grass growing on it (build_grass), loaded ahead too.
            const auto* textures = grass_ ? world_fb()->land_textures() : nullptr;
            const auto* t = layer->texture() != 0 ? lookup(textures, layer->texture()) : nullptr;
            if (t != nullptr && t->grasses() != nullptr && world_fb()->grasses() != nullptr) {
                for (const auto id : *t->grasses()) {
                    const auto* g = lookup(world_fb()->grasses(), id);
                    if (g != nullptr && g->model() != nullptr && g->model()->size() != 0) {
                        const String path = model_path(g->model()->string_view());
                        if (!out.has(path)) {
                            out.push_back(path);
                        }
                    }
                }
            }
            for (const auto& vpath : land_texture_paths(layer->texture())) {
                if (vpath.empty()) {
                    continue;
                }
                const String path = String::utf8(vpath.c_str());
                if (!out.has(path)) {
                    out.push_back(path);
                }
            }
        }
    }
    return out;
}

std::int64_t SkydotWorld::request_exterior(std::int64_t world, std::int64_t x, std::int64_t y) {
    return request_all(get_exterior_resources(world, x, y));
}

std::int64_t SkydotWorld::request_all(const godot::PackedStringArray& paths) {
    if (assets_ == nullptr) {
        return 0;
    }
    std::int64_t waiting = 0;
    for (const String& path : paths) {
        if (assets_->request(utf8(path)) == AssetCache::Status::loading) {
            ++waiting;
        }
    }
    return waiting;
}

std::int64_t SkydotWorld::get_cached_resource_count() const {
    return assets_ != nullptr ? static_cast<std::int64_t>(assets_->cached()) : 0;
}

std::int64_t SkydotWorld::get_pending_resource_count() const {
    return assets_ != nullptr ? static_cast<std::int64_t>(assets_->pending()) : 0;
}

void SkydotWorld::trim_cache() {
    if (assets_ != nullptr) {
        assets_->trim();
    }
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

std::int64_t SkydotWorld::warm_up() {
    if (!terrain_) {
        terrain_ = std::make_unique<TerrainBuilder>(assets_);
        terrain_->set_tiling(static_cast<float>(terrain_tiling_));
    }
    terrain_->warm_up();
    water_.warm_up();
    return materials().warm_up() + TerrainBuilder::k_max_layers + 1;
}

void SkydotWorld::set_terrain_tiling(double repeats) {
    terrain_tiling_ = repeats;
    terrain_.reset();
}
double SkydotWorld::get_terrain_tiling() const { return terrain_tiling_; }

struct SkydotWorld::BuildJob {
    /// References still to place, with the cell each belongs to.
    std::vector<std::pair<const wfb::Ref*, std::uint32_t>> refs;
    std::size_t next = 0;
    /// Actors, placed after the references.
    std::vector<ActorAt> actors;
    std::size_t next_actor = 0;
    BuildStats stats;
    /// Actors are looked up once the references are placed, so a build
    /// finished later (a place prepared ahead) has them where they are then.
    bool actors_found = false;
    bool exterior = false;
    std::uint32_t cell = 0; ///< The interior, for actors.
    std::uint32_t world = 0; ///< The worldspace and grid square, for actors.
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool terrain = false;
    bool water = false;
    std::vector<godot::Ref<godot::Resource>> kept; ///< keep_actor_resources
};

godot::Node3D* SkydotWorld::build_exterior(std::int64_t world, std::int64_t x,
                                           std::int64_t y) const {
    auto* root = begin_exterior(world, x, y);
    if (root != nullptr) {
        continue_build(root, std::numeric_limits<std::int64_t>::max());
    }
    return root;
}

bool SkydotWorld::continue_build(godot::Node3D* root, std::int64_t budget_usec) const {
    if (root == nullptr) {
        return true;
    }
    const auto it = jobs_.find(root->get_instance_id());
    if (it == jobs_.end()) {
        return true;
    }
    auto& job = *it->second;
    const auto started = godot::Time::get_singleton()->get_ticks_usec();
    if (!place_refs(root, job, started, budget_usec)) {
        return false;
    }
    if (!job.actors_found) {
        job.actors_found = true;
        if (actors_) {
            job.actors = job.exterior ? actors_in_grid(job.world, job.x, job.y) : actors_in_cell(job.cell);
        }
    }
    while (job.next_actor < job.actors.size()) {
        if (static_cast<std::int64_t>(godot::Time::get_singleton()->get_ticks_usec() - started) > budget_usec) {
            return false;
        }
        const auto& at = job.actors[job.next_actor++];
        place_actor(root, *at.actor, job.stats, at.place);
    }
    Dictionary out = stats_dictionary(job.stats);
    if (job.exterior) {
        out["terrain"] = job.terrain;
        out["water"] = job.water;
    }
    root->set_meta("skydot_stats", out);
    jobs_.erase(it);
    return true;
}

bool SkydotWorld::place_refs(godot::Node3D* root, BuildJob& job, std::uint64_t started,
                             std::int64_t budget_usec) const {
    while (job.next < job.refs.size()) {
        // At least one reference per call, so every build progresses.
        if (job.next != 0 && static_cast<std::int64_t>(godot::Time::get_singleton()->get_ticks_usec() - started) >
                                 budget_usec) {
            return false;
        }
        const auto& [ref, cell] = job.refs[job.next++];
        place_ref(root, *ref, cell, job.stats);
    }
    return true;
}

bool SkydotWorld::continue_build_static(godot::Node3D* root, std::int64_t budget_usec) const {
    if (root == nullptr) {
        return true;
    }
    const auto it = jobs_.find(root->get_instance_id());
    if (it == jobs_.end()) {
        return true;
    }
    return place_refs(root, *it->second, godot::Time::get_singleton()->get_ticks_usec(), budget_usec);
}

godot::Node3D* SkydotWorld::begin_exterior(std::int64_t world, std::int64_t x,
                                           std::int64_t y) const {
    const auto w = static_cast<std::uint32_t>(world);
    const auto gx = static_cast<std::int32_t>(x);
    const auto gy = static_cast<std::int32_t>(y);
    const auto* cell = data().exterior_ptr(w, gx, gy);
    const std::uint32_t land = data().land_world(w);
    const auto* land_cell = data().exterior_ptr(land, gx, gy);
    const wfb::Terrain* terrain = land_cell != nullptr ? land_cell->terrain() : nullptr;
    if (cell == nullptr && terrain == nullptr) {
        return nullptr;
    }

    auto* root = memnew(godot::Node3D);
    root->set_name(cell != nullptr && cell->editor_id() != nullptr && cell->editor_id()->size() != 0
                       ? to_godot(cell->editor_id())
                       : String("Exterior ") + String::num_int64(x) + "," + String::num_int64(y));
    const Vector3 corner = skyrim_position(
        Vector3(static_cast<float>(gx) * k_cell_units, static_cast<float>(gy) * k_cell_units, 0));

    if (terrain != nullptr) {
        if (!terrain_) {
            terrain_ = std::make_unique<TerrainBuilder>(assets_);
            terrain_->set_tiling(static_cast<float>(terrain_tiling_));
            terrain_->set_collision(collision_);
        }
        const auto neighbours = [&](int dx, int dy) {
            const auto* n = data().exterior_ptr(land, gx + dx, gy + dy);
            return n != nullptr && n->terrain() != nullptr ? TerrainBuilder::heights(*n->terrain())
                                                           : std::vector<float>{};
        };
        const auto paths = [&](std::uint32_t id) { return land_texture_paths(id); };
        auto* node = terrain_->build(*terrain, neighbours, paths);
        node->set_position(corner);
        root->add_child(node);
    }

    // Water: the cell's XCLW if it has water (DATA bit 1), else the
    // worldspace default.
    bool water = false;
    float water_height = 0.0F;
    if (cell != nullptr && formats::has_flag(cell->flags(), wfb::CellFlags::has_water) &&
        std::abs(cell->water_height()) < k_no_water) {
        water = true;
        water_height = cell->water_height();
    } else if (cell == nullptr) {
        if (const auto* ws = data().world_ptr(w); ws != nullptr && ws->has_defaults()) {
            water = true;
            water_height = ws->default_water_height();
        }
    }
    if (water) {
        const auto load = [&](const std::string& vpath) -> godot::Ref<godot::Texture> {
            return resource(String::utf8(vpath.c_str()));
        };
        const auto material = water_.material(data().water_ptr(data().water_type(w, cell)), load);
        godot::Ref<godot::PlaneMesh> plane;
        plane.instantiate();
        const auto side = static_cast<float>(static_cast<double>(k_cell_units) * UNIT_SCALE);
        plane->set_size(godot::Vector2(side, side));
        plane->set_material(material);
        auto* surface = memnew(godot::MeshInstance3D);
        surface->set_name("Water");
        surface->set_mesh(plane);
        surface->set_position(corner + Vector3(side / 2,
                                               static_cast<float>(static_cast<double>(water_height) * UNIT_SCALE),
                                               -side / 2));
        root->add_child(surface);
    }

    if (terrain != nullptr && grass_ && skyrim_materials_) {
        GrassInputs grass;
        grass.terrain = terrain;
        grass.grid_x = gx;
        grass.grid_y = gy;
        grass.has_water = water;
        grass.water_height = water_height;
        grass.grasses_of = [this](std::uint32_t ltex) {
            std::vector<const wfb::Grass*> out;
            const auto* textures = world_fb()->land_textures();
            const auto* list = world_fb()->grasses();
            const auto* t = ltex != 0 ? lookup(textures, ltex) : nullptr;
            if (t == nullptr || t->grasses() == nullptr || list == nullptr) {
                return out;
            }
            for (const auto id : *t->grasses()) {
                if (const auto* g = lookup(list, id)) {
                    out.push_back(g);
                }
            }
            return out;
        };
        grass.model_of = [this](const wfb::Grass& g) { return grass_model(g); };
        std::int64_t placed = 0;
        if (auto* node = build_grass(grass, placed)) {
            node->set_position(corner);
            root->add_child(node);
        }
    }

    if (cell != nullptr && navigation_) {
        if (auto* navmesh = build_navmeshes(*cell, data().navmeshes())) {
            root->add_child(navmesh);
        }
    }

    auto job = std::make_shared<BuildJob>();
    job->exterior = true;
    job->terrain = terrain != nullptr;
    job->water = water;
    if (cell != nullptr && cell->refs() != nullptr) {
        for (const auto* ref : *cell->refs()) {
            job->refs.emplace_back(ref, cell->id());
        }
    }
    if (const auto* persistent = data().persistent_refs(w, gx, gy)) {
        const auto owner = data().persistent_cell(w);
        for (const auto* ref : *persistent) {
            job->refs.emplace_back(ref, owner);
        }
    }
    job->world = w;
    job->x = gx;
    job->y = gy;
    if (actors_) {
        keep_actor_resources(*job, actors_in_grid(w, gx, gy));
    }
    add_job(root, std::move(job));
    return root;
}

godot::Node3D* SkydotWorld::begin_cell(std::int64_t id) const {
    const auto* cell = data().cell_ptr(id);
    if (cell == nullptr) {
        godot::UtilityFunctions::push_error("SkydotWorld: no cell ", hex_id(static_cast<std::uint32_t>(id)));
        return nullptr;
    }
    auto* root = memnew(godot::Node3D);
    root->set_name(cell->editor_id() != nullptr && cell->editor_id()->size() != 0
                       ? to_godot(cell->editor_id())
                       : hex_id(cell->id()));
    if (navigation_) {
        if (auto* navmesh = build_navmeshes(*cell, data().navmeshes())) {
            root->add_child(navmesh);
        }
    }
    auto job = std::make_shared<BuildJob>();
    if (const auto* refs = cell->refs()) {
        for (const auto* ref : *refs) {
            job->refs.emplace_back(ref, cell->id());
        }
    }
    job->cell = cell->id();
    if (actors_) {
        keep_actor_resources(*job, actors_in_cell(cell->id()));
    }
    add_job(root, std::move(job));
    return root;
}

void SkydotWorld::keep_actor_resources(BuildJob& job, const std::vector<ActorAt>& actors) const {
    if (assets_ == nullptr) {
        return;
    }
    for (const auto& at : actors) {
        for (const auto& key : actor_resources(*at.actor)) {
            if (auto res = assets_->cached(key); res.is_valid()) {
                job.kept.push_back(std::move(res));
            }
        }
    }
}

void SkydotWorld::add_job(godot::Node3D* root, std::shared_ptr<BuildJob> job) const {
    // Builds whose roots were freed unfinished are dropped here.
    std::erase_if(jobs_, [](const auto& entry) {
        return godot::ObjectDB::get_instance(entry.first) == nullptr;
    });
    jobs_.emplace(root->get_instance_id(), std::move(job));
}

} // namespace skydot
