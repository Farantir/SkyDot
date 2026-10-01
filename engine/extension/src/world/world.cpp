// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/world.hpp"

#include "world/animator.hpp"
#include "world/billboard.hpp"
#include "world/flicker.hpp"
#include "world/refs.hpp"

#include "world_generated.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/time.hpp>
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

// LIGH DATA flags (UESP, LIGH record).
constexpr std::uint32_t k_light_negative = 0x0004;
constexpr std::uint32_t k_light_off_by_default = 0x0020;
constexpr std::uint32_t k_light_spot = 0x0400;

// Ref flags (world.fbs).
constexpr std::uint32_t k_ref_initially_disabled = 0x1;
constexpr std::uint32_t k_ref_persistent = 0x2;
constexpr std::uint32_t k_ref_enable_opposite = 0x4;

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

/// Editor-only marker meshes (XMarker, heading markers, idle markers...), which
/// the game does not render.
bool is_marker_model(std::string_view model) {
    const auto slash = model.rfind('/');
    const auto file = slash == std::string_view::npos ? model : model.substr(slash + 1);
    return file.find("marker") != std::string_view::npos ||
           model.find("/markers/") != std::string_view::npos;
}

/// `meshes/foo/bar.nif` -> `res://meshes/foo/bar.scn` (the bake's naming).
String scene_path_for(std::string_view model) {
    std::string path(model);
    if (path.ends_with(".nif")) {
        path.replace(path.size() - 4, 4, ".scn");
    }
    return String("res://") + String::utf8(path.c_str(), static_cast<int>(path.size()));
}

/// Game units per exterior cell side.
constexpr float k_cell_units = 4096.0F;

/// XCLW values this large mean "no water here".
constexpr float k_no_water = 1.0e30F;

std::uint64_t grid_key(std::uint32_t world, std::int32_t x, std::int32_t y) {
    return (static_cast<std::uint64_t>(world) << 32) |
           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(x)) << 16) |
           static_cast<std::uint64_t>(static_cast<std::uint16_t>(y));
}

/// Z-up to Y-up: -90 degrees about X, as the converter's mesh writer does.
const Basis& axis_conversion() {
    static const Basis basis(Vector3(1, 0, 0), -std::numbers::pi_v<godot::real_t> / 2);
    return basis;
}

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
    godot::ClassDB::bind_method(D_METHOD("get_refs", "cell_id"), &SkydotWorld::get_refs);
    godot::ClassDB::bind_method(D_METHOD("get_base", "id"), &SkydotWorld::get_base);
    godot::ClassDB::bind_method(D_METHOD("build_cell", "id"), &SkydotWorld::build_cell);
    godot::ClassDB::bind_method(D_METHOD("get_quest_count"), &SkydotWorld::get_quest_count);
    godot::ClassDB::bind_method(D_METHOD("has_quest", "id"), &SkydotWorld::has_quest);
    godot::ClassDB::bind_method(D_METHOD("list_quests", "filter"), &SkydotWorld::list_quests);
    godot::ClassDB::bind_method(D_METHOD("find_quest", "editor_id"), &SkydotWorld::find_quest);
    godot::ClassDB::bind_method(D_METHOD("get_quest", "id"), &SkydotWorld::get_quest);
    godot::ClassDB::bind_method(D_METHOD("get_global", "id"), &SkydotWorld::get_global);
    godot::ClassDB::bind_method(D_METHOD("get_actor", "ref"), &SkydotWorld::get_actor);
    godot::ClassDB::bind_method(D_METHOD("find_actor_of", "npc"), &SkydotWorld::find_actor_of);
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
    godot::ClassDB::bind_static_method("SkydotWorld",
                                       D_METHOD("skyrim_transform", "position", "rotation", "scale"),
                                       &SkydotWorld::skyrim_transform);
    godot::ClassDB::bind_static_method("SkydotWorld", D_METHOD("skyrim_position", "position"),
                                       &SkydotWorld::skyrim_position);
    godot::ClassDB::bind_static_method("SkydotWorld", D_METHOD("godot_to_skyrim", "position"),
                                       &SkydotWorld::godot_to_skyrim);
    BIND_CONSTANT(WORLD_FORMAT_VERSION);
}

// ---- opening --------------------------------------------------------------

Error SkydotWorld::fail(Error code, const String& why) {
    error_ = why;
    root_ = nullptr;
    bytes_ = godot::PackedByteArray();
    exteriors_.clear();
    persistent_.clear();
    persistent_cells_.clear();
    doors_.clear();
    ref_cells_.clear();
    activate_children_.clear();
    actor_of_.clear();
    godot::UtilityFunctions::push_error("SkydotWorld: ", why);
    return code;
}

Error SkydotWorld::open(const String& path) {
    root_ = nullptr;
    error_ = String();
    if (!godot::FileAccess::file_exists(path)) {
        return fail(godot::ERR_FILE_NOT_FOUND, String("no world.fb at ") + path);
    }
    bytes_ = godot::FileAccess::get_file_as_bytes(path);
    const auto* data = bytes_.ptr();
    const auto size = static_cast<std::size_t>(bytes_.size());
    flatbuffers::Verifier verifier(data, size);
    if (size == 0 || !wfb::VerifyWorldBuffer(verifier)) {
        return fail(godot::ERR_FILE_CORRUPT, path + String(" is not a valid world.fb"));
    }
    const auto* root = wfb::GetWorld(data);
    if (root->format_version() != WORLD_FORMAT_VERSION) {
        return fail(godot::ERR_FILE_UNRECOGNIZED,
                    String("world.fb format version ") +
                        String::num_int64(root->format_version()) +
                        " is not one this engine reads (it reads " +
                        String::num_int64(WORLD_FORMAT_VERSION) + ")");
    }
    root_ = root;
    build_indexes();
    return godot::OK;
}

void SkydotWorld::build_indexes() {
    actor_of_.clear();
    exteriors_.clear();
    persistent_.clear();
    persistent_cells_.clear();
    doors_.clear();
    ref_cells_.clear();
    activate_children_.clear();
    enable_children_.clear();
    enable_parents_.clear();
    const auto* cells = root_->cells();
    if (cells == nullptr) {
        return;
    }
    for (const auto* cell : *cells) {
        if (const auto* refs = cell->refs()) {
            for (const auto* ref : *refs) {
                if (ref->enable_parent() != 0) {
                    enable_children_.emplace(ref->enable_parent(), ref->id());
                }
            }
        }
    }
    for (const auto* cell : *cells) {
        if (const auto* refs = cell->refs()) {
            for (const auto* ref : *refs) {
                if (enable_children_.contains(ref->id())) {
                    enable_parents_.emplace(ref->id(), ref);
                }
            }
        }
        if (const auto* doors = cell->doors()) {
            for (const auto* door : *doors) {
                doors_.emplace(door->ref(), std::pair{cell, door});
            }
        }
        if (const auto* parents = cell->activate_parents()) {
            for (const auto* p : *parents) {
                activate_children_.emplace(p->parent(), std::tuple{p->ref(), cell->id(), p->delay()});
            }
        }
        if ((cell->flags() & 0x1u) != 0 || cell->world() == 0) {
            continue;
        }
        if (cell->persistent()) {
            persistent_cells_.emplace(cell->world(), cell->id());
            if (const auto* refs = cell->refs()) {
                for (const auto* ref : *refs) {
                    const auto x = static_cast<std::int32_t>(
                        std::floor(ref->position().x() / k_cell_units));
                    const auto y = static_cast<std::int32_t>(
                        std::floor(ref->position().y() / k_cell_units));
                    persistent_[grid_key(cell->world(), x, y)].push_back(ref);
                }
            }
        } else if (cell->has_grid()) {
            exteriors_.emplace(grid_key(cell->world(), cell->grid_x(), cell->grid_y()), cell);
        }
    }
}

bool SkydotWorld::is_open() const { return root_ != nullptr; }
String SkydotWorld::get_error() const { return error_; }

std::int64_t SkydotWorld::get_cell_count() const {
    return root_ != nullptr && root_->cells() != nullptr ? root_->cells()->size() : 0;
}

std::int64_t SkydotWorld::get_base_count() const {
    return root_ != nullptr && root_->bases() != nullptr ? root_->bases()->size() : 0;
}

// ---- lookups --------------------------------------------------------------

const wfb::Cell* SkydotWorld::cell_ptr(std::int64_t id) const {
    const auto* cells = root_ != nullptr ? root_->cells() : nullptr;
    if (cells == nullptr) {
        return nullptr;
    }
    const auto key = static_cast<std::uint32_t>(id);
    const auto it = std::lower_bound(cells->begin(), cells->end(), key,
                                     [](const wfb::Cell* c, std::uint32_t v) { return c->id() < v; });
    return (it != cells->end() && it->id() == key) ? *it : nullptr;
}

const wfb::Worldspace* SkydotWorld::world_ptr(std::int64_t id) const {
    const auto* worlds = root_ != nullptr ? root_->worlds() : nullptr;
    if (worlds == nullptr) {
        return nullptr;
    }
    const auto key = static_cast<std::uint32_t>(id);
    const auto it = std::lower_bound(
        worlds->begin(), worlds->end(), key,
        [](const wfb::Worldspace* w, std::uint32_t v) { return w->id() < v; });
    return (it != worlds->end() && it->id() == key) ? *it : nullptr;
}

const wfb::Cell* SkydotWorld::exterior_ptr(std::uint32_t world, std::int32_t x,
                                           std::int32_t y) const {
    const auto it = exteriors_.find(grid_key(world, x, y));
    return it != exteriors_.end() ? it->second : nullptr;
}

/// The worldspace whose terrain `world` shows: its parent when PNAM bit 0
/// (use land data) is set.
const wfb::Water* SkydotWorld::water_ptr(std::uint32_t id) const {
    const auto* waters = root_ != nullptr ? root_->waters() : nullptr;
    if (waters == nullptr || id == 0) {
        return nullptr;
    }
    const auto it = std::lower_bound(waters->begin(), waters->end(), id,
                                     [](const wfb::Water* w, std::uint32_t v) { return w->id() < v; });
    return (it != waters->end() && it->id() == id) ? *it : nullptr;
}

std::uint32_t SkydotWorld::water_type(std::uint32_t world, const wfb::Cell* cell) const {
    if (cell != nullptr && cell->water() != 0) {
        return cell->water();
    }
    const auto* w = world_ptr(world);
    return w != nullptr ? w->water() : 0;
}

std::uint32_t SkydotWorld::land_world(std::uint32_t world) const {
    const auto* w = world_ptr(world);
    if (w != nullptr && w->parent() != 0 && (w->parent_flags() & 0x1u) != 0) {
        return w->parent();
    }
    return world;
}

const wfb::Base* SkydotWorld::base_ptr(std::int64_t id) const {
    const auto* bases = root_ != nullptr ? root_->bases() : nullptr;
    if (bases == nullptr) {
        return nullptr;
    }
    const auto key = static_cast<std::uint32_t>(id);
    const auto it = std::lower_bound(bases->begin(), bases->end(), key,
                                     [](const wfb::Base* b, std::uint32_t v) { return b->id() < v; });
    return (it != bases->end() && it->id() == key) ? *it : nullptr;
}

std::int64_t SkydotWorld::find_cell(const String& editor_id) const {
    const auto* cells = root_ != nullptr ? root_->cells() : nullptr;
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
    const auto* cells = root_ != nullptr ? root_->cells() : nullptr;
    if (cells == nullptr) {
        return out;
    }
    const auto needle = filter.to_lower().utf8();
    const std::string needle_str(needle.get_data(), static_cast<std::size_t>(needle.length()));
    for (const auto* cell : *cells) {
        const bool interior = (cell->flags() & 0x1u) != 0;
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
    const auto* cell = cell_ptr(id);
    if (cell == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(cell->id());
    out["editor_id"] = to_godot(cell->editor_id());
    out["interior"] = (cell->flags() & 0x1u) != 0;
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
    return out;
}

Array SkydotWorld::get_refs(std::int64_t cell_id) const {
    Array out;
    const auto* cell = cell_ptr(cell_id);
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
                                              Vector3(r.x(), r.y(), r.z()), ref->scale());
        entry["scale"] = ref->scale();
        entry["disabled"] = initially_disabled(*ref);
        entry["persistent"] = (ref->flags() & k_ref_persistent) != 0;
        entry["enable_parent"] = static_cast<std::int64_t>(ref->enable_parent());
        out.push_back(entry);
    }
    return out;
}

Dictionary SkydotWorld::get_base(std::int64_t id) const {
    Dictionary out;
    const auto* base = base_ptr(id);
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
    godot::PackedStringArray missing;
};

bool SkydotWorld::initially_disabled(const wfb::Ref& ref) const {
    // A reference with an enable parent takes the parent's state (inverted if
    // flagged); its own flag counts only when the parent is unknown.
    const wfb::Ref* r = &ref;
    bool opposite = false;
    for (int depth = 0; depth < 16 && r->enable_parent() != 0; ++depth) {
        const auto it = enable_parents_.find(r->enable_parent());
        if (it == enable_parents_.end()) {
            break;
        }
        opposite ^= (r->flags() & k_ref_enable_opposite) != 0;
        r = it->second;
    }
    return ((r->flags() & k_ref_initially_disabled) != 0) != opposite;
}

void SkydotWorld::place_ref(godot::Node3D* root, const wfb::Ref& ref, std::uint32_t cell,
                            BuildStats& stats, bool include_disabled) const {
    ++stats.refs;
    if (!include_disabled && initially_disabled(ref)) {
        ++stats.disabled;
        return;
    }
    const auto* base = base_ptr(ref.base());
    if (base == nullptr) {
        ++stats.no_base;
        return;
    }
    if (materials_.is_null()) {
        materials_.instantiate();
    }
    const auto& p = ref.position();
    const auto& r = ref.rotation();
    const Transform3D transform = skyrim_transform(Vector3(p.x(), p.y(), p.z()),
                                                   Vector3(r.x(), r.y(), r.z()), ref.scale());
    const String name = hex_id(ref.id()) + " " + to_godot(base->editor_id());

    const auto* model = base->model();
    if (model != nullptr && model->size() != 0) {
        if (is_marker_model(model->string_view())) {
            ++stats.markers;
        } else {
            const String scene_path = scene_path_for(model->string_view());
            const godot::Ref<godot::PackedScene> scene = resource(scene_path);
            if (scene.is_valid()) {
                auto* node = godot::Object::cast_to<godot::Node3D>(scene->instantiate());
                if (node != nullptr) {
                    node->set_name(name);
                    node->set_transform(transform);
                    if (skyrim_materials_) {
                        stats.materials += materials_->apply(node);
                    }
                    stats.billboards += SkydotBillboard::attach(node);
                    if (effects_) {
                        stats.effects += SkydotAnimator::attach(node, materials_);
                    }
                    tag_ref(node, ref.id(), cell, activatable(base, cell, ref.id()));
                    root->add_child(node);
                    ++stats.placed;
                }
            } else if (!stats.missing.has(scene_path)) {
                stats.missing.push_back(scene_path);
            }
        }
    }

    if (const auto* l = base->light(); l != nullptr && base->has_light()) {
        if ((l->flags() & k_light_off_by_default) != 0) {
            return;
        }
        godot::Light3D* light = nullptr;
        if ((l->flags() & k_light_spot) != 0) {
            auto* spot = memnew(godot::SpotLight3D);
            spot->set_param(godot::Light3D::PARAM_RANGE,
                            static_cast<float>(l->radius() * UNIT_SCALE));
            spot->set_param(godot::Light3D::PARAM_SPOT_ANGLE,
                            std::clamp(l->fov() / 2.0F, 1.0F, 89.0F));
            light = spot;
        } else {
            auto* omni = memnew(godot::OmniLight3D);
            omni->set_param(godot::Light3D::PARAM_RANGE,
                            static_cast<float>(l->radius() * UNIT_SCALE));
            omni->set_param(godot::Light3D::PARAM_ATTENUATION,
                            l->falloff_exponent() > 0.0F ? l->falloff_exponent() : 1.0F);
            light = omni;
        }
        light->set_name(name + String(" light"));
        light->set_color(unpack_color(l->color()));
        light->set_param(godot::Light3D::PARAM_ENERGY, l->fade() > 0.0F ? l->fade() : 1.0F);
        light->set_negative((l->flags() & k_light_negative) != 0);
        light->set_transform(transform.orthonormalized());
        if (effects_ && (l->flags() & SkydotFlicker::ANY) != 0) {
            auto* flicker = memnew(SkydotFlicker);
            flicker->set_name("SkydotFlicker");
            flicker->configure(l->flags(), static_cast<double>(l->flicker_period()),
                               static_cast<double>(l->flicker_intensity()),
                               static_cast<double>(l->flicker_movement()) * UNIT_SCALE);
            light->add_child(flicker);
            ++stats.flickers;
        }
        root->add_child(light);
        ++stats.lights;
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
    return out;
}

godot::Node3D* SkydotWorld::build_ref(std::int64_t cell_id, std::int64_t ref_id) const {
    const auto* cell = cell_ptr(cell_id);
    const auto* refs = cell != nullptr ? cell->refs() : nullptr;
    if (refs == nullptr) {
        return nullptr;
    }
    const auto id = static_cast<std::uint32_t>(ref_id);
    const auto it = std::lower_bound(refs->begin(), refs->end(), id,
                                     [](const wfb::Ref* r, std::uint32_t v) { return r->id() < v; });
    if (it == refs->end() || it->id() != id) {
        return nullptr;
    }
    auto* root = memnew(godot::Node3D);
    root->set_name(hex_id(id));
    BuildStats stats;
    place_ref(root, **it, cell->id(), stats, true);
    root->set_meta("skydot_stats", stats_dictionary(stats));
    return root;
}

godot::Node3D* SkydotWorld::build_cell(std::int64_t id) const {
    const auto* cell = cell_ptr(id);
    if (cell == nullptr) {
        godot::UtilityFunctions::push_error("SkydotWorld: no cell ", hex_id(static_cast<std::uint32_t>(id)));
        return nullptr;
    }

    auto* root = memnew(godot::Node3D);
    root->set_name(cell->editor_id() != nullptr ? to_godot(cell->editor_id())
                                                : hex_id(cell->id()));
    BuildStats stats;
    if (const auto* refs = cell->refs()) {
        for (const auto* ref : *refs) {
            place_ref(root, *ref, cell->id(), stats);
        }
    }
    root->set_meta("skydot_stats", stats_dictionary(stats));
    return root;
}

// ---- exteriors ------------------------------------------------------------

Array SkydotWorld::list_worlds() const {
    Array out;
    const auto* worlds = root_ != nullptr ? root_->worlds() : nullptr;
    if (worlds == nullptr) {
        return out;
    }
    for (const auto* w : *worlds) {
        Dictionary entry;
        entry["id"] = static_cast<std::int64_t>(w->id());
        entry["editor_id"] = to_godot(w->editor_id());
        entry["parent"] = static_cast<std::int64_t>(w->parent());
        entry["land_world"] = static_cast<std::int64_t>(land_world(w->id()));
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
    const auto* worlds = root_ != nullptr ? root_->worlds() : nullptr;
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
    const auto* cell = exterior_ptr(static_cast<std::uint32_t>(world), static_cast<std::int32_t>(x),
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
    const auto* ltex = root_ != nullptr ? root_->land_textures() : nullptr;
    if (id == 0 || ltex == nullptr) {
        return {TerrainBuilder::k_default_texture, ""};
    }
    const auto it = std::lower_bound(
        ltex->begin(), ltex->end(), id,
        [](const wfb::LandTexture* l, std::uint32_t v) { return l->id() < v; });
    if (it == ltex->end() || it->id() != id) {
        return {TerrainBuilder::k_default_texture, ""};
    }
    const auto str = [](const flatbuffers::String* s) {
        return s != nullptr ? s->str() : std::string{};
    };
    return {str(it->diffuse()), str(it->normal())};
}

godot::Ref<godot::Resource> SkydotWorld::resource(const String& path) const {
    const std::string key = utf8(path);
    if (const auto it = cache_.find(key); it != cache_.end()) {
        return it->second;
    }
    godot::Ref<godot::Resource> loaded;
    auto* loader = godot::ResourceLoader::get_singleton();
    if (loader->exists(path)) {
        loaded = loader->load(path);
    }
    cache_.emplace(key, loaded);
    return loaded;
}

godot::PackedStringArray SkydotWorld::get_exterior_resources(std::int64_t world, std::int64_t x,
                                                             std::int64_t y) const {
    godot::PackedStringArray out;
    const auto w = static_cast<std::uint32_t>(world);
    const auto gx = static_cast<std::int32_t>(x);
    const auto gy = static_cast<std::int32_t>(y);
    const auto add_ref = [&](const wfb::Ref& ref) {
        if (initially_disabled(ref)) {
            return;
        }
        const auto* base = base_ptr(ref.base());
        const auto* model = base != nullptr ? base->model() : nullptr;
        if (model == nullptr || model->size() == 0 || is_marker_model(model->string_view())) {
            return;
        }
        const String path = scene_path_for(model->string_view());
        if (!out.has(path)) {
            out.push_back(path);
        }
    };
    if (const auto* cell = exterior_ptr(w, gx, gy); cell != nullptr && cell->refs() != nullptr) {
        for (const auto* ref : *cell->refs()) {
            add_ref(*ref);
        }
    }
    if (const auto it = persistent_.find(grid_key(w, gx, gy)); it != persistent_.end()) {
        for (const auto* ref : it->second) {
            add_ref(*ref);
        }
    }
    const auto* cell = exterior_ptr(w, gx, gy);
    if (const auto* water = water_ptr(water_type(w, cell));
        water != nullptr && water->noise() != nullptr && water->noise()->size() != 0) {
        const String path = String("res://") + String::utf8(water->noise()->Get(0)->c_str());
        if (!out.has(path)) {
            out.push_back(path);
        }
    }
    const auto* land = exterior_ptr(land_world(w), gx, gy);
    if (land != nullptr && land->terrain() != nullptr && land->terrain()->layers() != nullptr) {
        for (const auto* layer : *land->terrain()->layers()) {
            for (const auto& vpath : land_texture_paths(layer->texture())) {
                if (vpath.empty()) {
                    continue;
                }
                const String path = String("res://") + String::utf8(vpath.c_str());
                if (!out.has(path)) {
                    out.push_back(path);
                }
            }
        }
    }
    return out;
}

std::int64_t SkydotWorld::request_exterior(std::int64_t world, std::int64_t x, std::int64_t y) {
    auto* loader = godot::ResourceLoader::get_singleton();
    std::int64_t waiting = 0;
    for (const String& path : get_exterior_resources(world, x, y)) {
        const std::string key = utf8(path);
        if (cache_.contains(key)) {
            continue;
        }
        if (!pending_.contains(key)) {
            if (!loader->exists(path)) {
                cache_.emplace(key, godot::Ref<godot::Resource>());
                continue;
            }
            loader->load_threaded_request(path);
            pending_.emplace(key, true);
            ++waiting;
            continue;
        }
        switch (loader->load_threaded_get_status(path)) {
        case godot::ResourceLoader::THREAD_LOAD_IN_PROGRESS:
            ++waiting;
            break;
        case godot::ResourceLoader::THREAD_LOAD_LOADED:
            cache_.emplace(key, loader->load_threaded_get(path));
            pending_.erase(key);
            break;
        default: // failed or unknown: remember the miss
            cache_.emplace(key, godot::Ref<godot::Resource>());
            pending_.erase(key);
            break;
        }
    }
    return waiting;
}

std::int64_t SkydotWorld::get_cached_resource_count() const {
    return static_cast<std::int64_t>(cache_.size());
}

std::int64_t SkydotWorld::get_pending_resource_count() const {
    return static_cast<std::int64_t>(pending_.size());
}

void SkydotWorld::trim_cache() {
    std::erase_if(cache_, [](const auto& entry) {
        return entry.second.is_valid() && entry.second->get_reference_count() <= 1;
    });
}

std::int64_t SkydotWorld::find_weather(const String& editor_id) const {
    const auto* weathers = root_ != nullptr ? root_->weathers() : nullptr;
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
    const auto* ws = world_ptr(world);
    const auto* climates = root_ != nullptr ? root_->climates() : nullptr;
    const auto* weathers = root_ != nullptr ? root_->weathers() : nullptr;
    if (ws == nullptr || climates == nullptr || weathers == nullptr) {
        return out;
    }
    const auto find = [](const auto* list, std::uint32_t id) -> decltype(list->Get(0)) {
        const auto it = std::lower_bound(list->begin(), list->end(), id,
                                         [](const auto* e, std::uint32_t v) { return e->id() < v; });
        return (it != list->end() && it->id() == id) ? *it : nullptr;
    };
    const wfb::Climate* climate = find(climates, ws->climate());
    if (climate == nullptr && ws->parent() != 0) {
        if (const auto* parent = world_ptr(ws->parent())) {
            climate = find(climates, parent->climate());
        }
    }
    const wfb::Weather* weather =
        weather_id != 0 ? find(weathers, static_cast<std::uint32_t>(weather_id)) : nullptr;
    if (weather == nullptr && climate != nullptr && climate->weathers() != nullptr) {
        std::int32_t best = -1;
        for (const auto* entry : *climate->weathers()) {
            if (entry->chance() > best) {
                if (const auto* w = find(weathers, entry->weather())) {
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
    if (materials_.is_null()) {
        materials_.instantiate();
    }
    if (!terrain_) {
        terrain_ = std::make_unique<TerrainBuilder>();
        terrain_->set_tiling(static_cast<float>(terrain_tiling_));
    }
    terrain_->warm_up();
    water_.warm_up();
    return materials_->warm_up() + TerrainBuilder::k_max_layers + 1;
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
    BuildStats stats;
    bool terrain = false;
    bool water = false;
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
    while (job.next < job.refs.size()) {
        // At least one reference per call, so every build progresses.
        if (job.next != 0 && static_cast<std::int64_t>(godot::Time::get_singleton()->get_ticks_usec() - started) >
                                 budget_usec) {
            return false;
        }
        const auto& [ref, cell] = job.refs[job.next++];
        place_ref(root, *ref, cell, job.stats);
    }
    Dictionary out = stats_dictionary(job.stats);
    out["terrain"] = job.terrain;
    out["water"] = job.water;
    root->set_meta("skydot_stats", out);
    jobs_.erase(it);
    return true;
}

godot::Node3D* SkydotWorld::begin_exterior(std::int64_t world, std::int64_t x,
                                           std::int64_t y) const {
    const auto w = static_cast<std::uint32_t>(world);
    const auto gx = static_cast<std::int32_t>(x);
    const auto gy = static_cast<std::int32_t>(y);
    const auto* cell = exterior_ptr(w, gx, gy);
    const std::uint32_t land = land_world(w);
    const auto* land_cell = exterior_ptr(land, gx, gy);
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
            terrain_ = std::make_unique<TerrainBuilder>();
            terrain_->set_tiling(static_cast<float>(terrain_tiling_));
        }
        const auto neighbours = [&](int dx, int dy) {
            const auto* n = exterior_ptr(land, gx + dx, gy + dy);
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
    if (cell != nullptr && (cell->flags() & 0x2u) != 0 &&
        std::abs(cell->water_height()) < k_no_water) {
        water = true;
        water_height = cell->water_height();
    } else if (cell == nullptr) {
        if (const auto* ws = world_ptr(w); ws != nullptr && ws->has_defaults()) {
            water = true;
            water_height = ws->default_water_height();
        }
    }
    if (water) {
        const auto load = [&](const std::string& vpath) -> godot::Ref<godot::Texture> {
            return resource(String("res://") + String::utf8(vpath.c_str()));
        };
        const auto material = water_.material(water_ptr(water_type(w, cell)), load);
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

    auto job = std::make_shared<BuildJob>();
    job->terrain = terrain != nullptr;
    job->water = water;
    if (cell != nullptr && cell->refs() != nullptr) {
        for (const auto* ref : *cell->refs()) {
            job->refs.emplace_back(ref, cell->id());
        }
    }
    if (const auto it = persistent_.find(grid_key(w, gx, gy)); it != persistent_.end()) {
        const auto owner = persistent_cells_.find(w);
        for (const auto* ref : it->second) {
            job->refs.emplace_back(ref, owner != persistent_cells_.end() ? owner->second : 0);
        }
    }
    // Builds whose roots were freed unfinished are dropped here.
    std::erase_if(jobs_, [](const auto& entry) {
        return godot::ObjectDB::get_instance(entry.first) == nullptr;
    });
    jobs_.emplace(root->get_instance_id(), std::move(job));
    return root;
}

} // namespace skydot
