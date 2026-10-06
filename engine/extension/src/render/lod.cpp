// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/lod.hpp"
#include "render/materials.hpp"
#include "render/shader_source.hpp"

#include "skydot_formats/units.hpp"
#include "lod_generated.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

using godot::Array;
using godot::Dictionary;
using godot::Ref;
using godot::String;
using godot::Vector3;

namespace lfb = bethconv::pack::lfb;

namespace skydot {

namespace {

constexpr auto k_cell_units = static_cast<double>(formats::k_cell_units);
constexpr std::uint32_t k_lod_format = 1;

std::string utf8(const String& s) {
    const auto bytes = s.utf8();
    return {bytes.get_data(), static_cast<std::size_t>(bytes.length())};
}

/// A verified LOD asset's root, or null.
const lfb::Lod* read_lod(const godot::PackedByteArray& bytes) {
    // An empty array may have no data pointer; the root is read only if it has.
    const auto* data = bytes.ptr();
    flatbuffers::Verifier verifier(data, static_cast<std::size_t>(bytes.size()));
    if (data == nullptr || bytes.is_empty() || !lfb::VerifyLodBuffer(verifier)) {
        return nullptr;
    }
    const auto* root = lfb::GetLod(data);
    return root->format_version() == k_lod_format ? root : nullptr;
}

} // namespace

Dictionary SkydotLod::shader_codes() {
    Dictionary out;
    out["lod_terrain"] = shader_source::code("lod_terrain.gdshader");
    out["lod_object"] = shader_source::code("lod_object.gdshader");
    out["lod_water"] = shader_source::code("lod_water.gdshader");
    out["lod_tree"] = shader_source::code("lod_tree.gdshader");
    return out;
}

void SkydotLod::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("setup", "pack", "world", "world_id"), &SkydotLod::setup);
    godot::ClassDB::bind_method(D_METHOD("get_error"), &SkydotLod::get_error);
    godot::ClassDB::bind_method(D_METHOD("update", "camera", "budget_usec"), &SkydotLod::update);
    godot::ClassDB::bind_method(D_METHOD("set_cell_loaded", "x", "y", "loaded"), &SkydotLod::set_cell_loaded);
    godot::ClassDB::bind_method(D_METHOD("clear_loaded_cells"), &SkydotLod::clear_loaded_cells);
    godot::ClassDB::bind_method(D_METHOD("set_split_distance", "factor"), &SkydotLod::set_split_distance);
    godot::ClassDB::bind_method(D_METHOD("get_split_distance"), &SkydotLod::get_split_distance);
    godot::ClassDB::bind_method(D_METHOD("set_tree_distance", "cells"), &SkydotLod::set_tree_distance);
    godot::ClassDB::bind_method(D_METHOD("get_tree_distance"), &SkydotLod::get_tree_distance);
    godot::ClassDB::bind_method(D_METHOD("get_stats"), &SkydotLod::get_stats);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::FLOAT, "split_distance"), "set_split_distance",
                 "get_split_distance");
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::FLOAT, "tree_distance"), "set_tree_distance",
                 "get_tree_distance");
}

godot::Error SkydotLod::setup(const Ref<SkydotPack>& pack, const Ref<SkydotWorld>& world,
                              std::int64_t world_id) {
    error_ = String();
    if (pack.is_null() || world.is_null()) {
        error_ = "setup needs a pack and a world";
        return godot::ERR_INVALID_PARAMETER;
    }
    pack_ = pack;
    world_ = world;

    // The worldspace's own LOD, else that of the one whose land it shows.
    std::vector<String> names;
    const Array worlds = world->list_worlds();
    std::int64_t land = 0;
    for (std::int64_t i = 0; i < worlds.size(); ++i) {
        const Dictionary w = worlds[i];
        if (static_cast<std::int64_t>(w["id"]) == world_id) {
            names.push_back(String(w["editor_id"]).to_lower());
            land = w["land_world"];
        }
    }
    for (std::int64_t i = 0; i < worlds.size() && land != world_id; ++i) {
        const Dictionary w = worlds[i];
        if (static_cast<std::int64_t>(w["id"]) == land) {
            names.push_back(String(w["editor_id"]).to_lower());
        }
    }
    const lfb::LodSettings* s = nullptr;
    godot::PackedByteArray bytes;
    for (const auto& name : names) {
        bytes = pack->get_bytes(String("lodsettings/") + name + String(".lod"));
        if (bytes.is_empty()) {
            continue;
        }
        const auto* lod = read_lod(bytes);
        s = lod != nullptr ? lod->settings() : nullptr;
        if (s != nullptr) {
            name_ = name;
            break;
        }
    }
    if (s == nullptr) {
        error_ = "no LOD settings for this worldspace";
        return godot::ERR_FILE_NOT_FOUND;
    }
    south_west_x_ = s->south_west_x();
    south_west_y_ = s->south_west_y();
    stride_ = s->stride();
    lowest_ = s->lowest_level();
    highest_ = s->highest_level();
    if (stride_ <= 0 || stride_ > 4096 || lowest_ < 1 || highest_ < lowest_) {
        error_ = "LOD settings out of range";
        return godot::ERR_INVALID_DATA;
    }

    tree_types_.clear();
    const auto list_bytes =
        pack->get_bytes(String("meshes/terrain/") + name_ + String("/trees/") + name_ + String(".lst"));
    if (!list_bytes.is_empty()) {
        if (const auto* lod = read_lod(list_bytes); lod != nullptr && lod->tree_types() != nullptr) {
            for (const auto* t : *lod->tree_types()) {
                tree_types_[t->index()] = TreeType{.width = t->width(), .height = t->height(),
                                                   .u0 = t->u0(), .v0 = t->v0(), .u1 = t->u1(), .v1 = t->v1()};
            }
        }
    }
    tree_atlas_ = pack->load_texture(String("textures/terrain/") + name_ + String("/trees/") + name_ +
                                     String("treelod.dds"));

    noise_ = pack->has(String("textures/terrain/noise.dds"))
                 ? Ref<godot::Texture2D>(pack->load_texture(String("textures/terrain/noise.dds")))
                 : Ref<godot::Texture2D>();

    mask_image_ = godot::Image::create_empty(stride_, stride_, false, godot::Image::FORMAT_R8);
    mask_image_->fill(godot::Color(0, 0, 0));
    mask_ = godot::ImageTexture::create_from_image(mask_image_);
    loaded_cells_ = 0;

    terrain_shader_ = shader_source::shader("lod_terrain.gdshader");
    object_shader_ = shader_source::shader("lod_object.gdshader");
    water_shader_ = shader_source::shader("lod_water.gdshader");
    tree_shader_ = shader_source::shader("lod_tree.gdshader");
    water_material_.instantiate();
    water_material_->set_shader(water_shader_);
    apply_mask(water_material_);
    tree_material_.instantiate();
    tree_material_->set_shader(tree_shader_);
    tree_material_->set_shader_parameter("atlas", tree_atlas_);
    apply_mask(tree_material_);
    return godot::OK;
}

String SkydotLod::get_error() const { return error_; }

void SkydotLod::apply_mask(const Ref<godot::ShaderMaterial>& m) const {
    m->set_shader_parameter("skydot_cell_mask", mask_);
    m->set_shader_parameter("skydot_mask_origin", godot::Vector2(static_cast<float>(south_west_x_),
                                                                 static_cast<float>(south_west_y_)));
    m->set_shader_parameter("skydot_mask_size", static_cast<float>(stride_));
    m->set_shader_parameter("skydot_unit_scale", SkydotWorld::UNIT_SCALE);
    m->set_shader_parameter("skydot_cell_units", k_cell_units);
}

void SkydotLod::set_cell_loaded(std::int64_t x, std::int64_t y, bool loaded) {
    if (mask_image_.is_null()) {
        return;
    }
    const auto px = static_cast<std::int32_t>(x - south_west_x_);
    const auto py = static_cast<std::int32_t>(y - south_west_y_);
    if (px < 0 || py < 0 || px >= stride_ || py >= stride_) {
        return;
    }
    const bool was = mask_image_->get_pixel(px, py).r > 0.5F;
    if (was == loaded) {
        return;
    }
    mask_image_->set_pixel(px, py, loaded ? godot::Color(1, 1, 1) : godot::Color(0, 0, 0));
    loaded_cells_ = loaded ? loaded_cells_ + 1 : loaded_cells_ - 1;
    mask_dirty_ = true;
}

void SkydotLod::clear_loaded_cells() {
    if (mask_image_.is_valid()) {
        mask_image_->fill(godot::Color(0, 0, 0));
        loaded_cells_ = 0;
        mask_dirty_ = true;
    }
}

void SkydotLod::set_split_distance(double factor) { split_distance_ = std::max(0.0, factor); }
double SkydotLod::get_split_distance() const { return split_distance_; }
void SkydotLod::set_tree_distance(double cells) { tree_distance_ = std::max(0.0, cells); }
double SkydotLod::get_tree_distance() const { return tree_distance_; }

String SkydotLod::vpath(const Quad& q, const char* kind) const {
    const std::string name = utf8(name_);
    const std::string base = name + "." + std::to_string(std::abs(q.level)) + "." + std::to_string(q.x) +
                             "." + std::to_string(q.y);
    const std::string k(kind);
    std::string out = "meshes/terrain/" + name + "/";
    if (k == "bto") {
        out += "objects/";
    } else if (k == "btt") {
        out += "trees/";
    }
    out += base + "." + k;
    return String::utf8(out.c_str());
}

std::vector<String> SkydotLod::scenes_for(const Quad& q) const {
    std::vector<String> out;
    for (const char* kind : {"btr", "bto"}) {
        const String v = vpath(q, kind);
        const std::string key = utf8(v);
        auto it = exists_.find(key);
        if (it == exists_.end()) {
            it = exists_.emplace(key, pack_->has(v)).first;
        }
        if (it->second) {
            out.push_back(v);
        }
    }
    return out;
}

bool SkydotLod::loaded(const String& path) {
    const std::string key = utf8(path);
    if (resources_.contains(key)) {
        return true;
    }
    const auto assets = pack_->assets();
    if (assets == nullptr) {
        return true;
    }
    if (assets->request(key) == AssetCache::Status::loading) {
        pending_.insert(key);
        return false;
    }
    pending_.erase(key);
    resources_.emplace(key, assets->get(key));
    return true;
}

void SkydotLod::select(const Quad& q, double cx, double cy, std::set<Quad>& out) const {
    if (q.level > lowest_) {
        const double dx = std::max({static_cast<double>(q.x) - cx, 0.0, cx - (q.x + q.level)});
        const double dy = std::max({static_cast<double>(q.y) - cy, 0.0, cy - (q.y + q.level)});
        if (std::sqrt(dx * dx + dy * dy) < split_distance_ * q.level) {
            const int half = q.level / 2;
            for (const auto& [ox, oy] : {std::pair{0, 0}, {half, 0}, {0, half}, {half, half}}) {
                select(Quad{.level = half, .x = q.x + ox, .y = q.y + oy}, cx, cy, out);
            }
            return;
        }
    }
    out.insert(q);
}

Ref<godot::ShaderMaterial> SkydotLod::material(const Ref<godot::Shader>& shader,
                                                const Ref<godot::Texture2D>& albedo,
                                                const Ref<godot::Texture2D>& normal) {
    const auto id = [](const auto& r) { return r.is_valid() ? static_cast<std::int64_t>(r->get_instance_id()) : 0; };
    const auto key = std::pair{id(albedo) * 4 + (shader == terrain_shader_ ? 1 : 2), id(normal)};
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    Ref<godot::ShaderMaterial> m;
    m.instantiate();
    m->set_shader(shader);
    m->set_shader_parameter("albedo_tex", albedo);
    m->set_shader_parameter("normal_tex", normal);
    m->set_shader_parameter("has_normal", normal.is_valid());
    if (shader == terrain_shader_) {
        m->set_shader_parameter("noise_tex", noise_);
        m->set_shader_parameter("has_noise", noise_.is_valid());
    }
    apply_mask(m);
    materials_.emplace(key, m);
    return m;
}

namespace {

/// A texture slot's path in a material's bethconv extras; pack meshes carry
/// no glTF images, so this is where a surface's textures are named.
String slot_path(const Ref<godot::Material>& material, int slot) {
    if (material.is_null() || !material->has_meta("extras")) {
        return {};
    }
    const godot::Variant extras = material->get_meta("extras");
    if (extras.get_type() != godot::Variant::DICTIONARY) {
        return {};
    }
    const godot::Variant block = godot::Dictionary(extras).get("bethconv", godot::Variant());
    if (block.get_type() != godot::Variant::DICTIONARY) {
        return {};
    }
    const godot::Variant slots = godot::Dictionary(block).get("texture_slots", godot::Variant());
    if (slots.get_type() != godot::Variant::DICTIONARY) {
        return {};
    }
    const godot::Variant entry = godot::Dictionary(slots).get(String::num_int64(slot), godot::Variant());
    if (entry.get_type() != godot::Variant::DICTIONARY) {
        return {};
    }
    return godot::Dictionary(entry).get("path", String());
}

} // namespace

void SkydotLod::retexture(godot::Node* node, bool terrain, bool water) {
    const bool is_water = water || String(node->get_name()).to_lower() == "water";
    if (auto* mi = godot::Object::cast_to<godot::MeshInstance3D>(node); mi != nullptr && mi->get_mesh().is_valid()) {
        const auto surfaces = mi->get_mesh()->get_surface_count();
        for (std::int32_t i = 0; i < surfaces; ++i) {
            if (is_water) {
                mi->set_surface_override_material(i, water_material_);
                continue;
            }
            Ref<godot::BaseMaterial3D> base = mi->get_active_material(i);
            Ref<godot::Texture2D> albedo;
            Ref<godot::Texture2D> normal;
            if (base.is_valid()) {
                albedo = base->get_texture(godot::BaseMaterial3D::TEXTURE_ALBEDO);
                normal = base->get_texture(godot::BaseMaterial3D::TEXTURE_NORMAL);
            }
            if (albedo.is_null() && pack_.is_valid()) {
                const String path = slot_path(base, 0);
                albedo = path.is_empty() ? Ref<godot::Texture2D>() : Ref<godot::Texture2D>(pack_->load_texture(path));
            }
            if (normal.is_null() && pack_.is_valid()) {
                const String path = slot_path(base, 1);
                normal = path.is_empty() ? Ref<godot::Texture2D>() : Ref<godot::Texture2D>(pack_->load_texture(path));
            }
            mi->set_surface_override_material(i, material(terrain ? terrain_shader_ : object_shader_, albedo, normal));
        }
        mi->set_cast_shadows_setting(godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    }
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        retexture(node->get_child(i), terrain, is_water);
    }
}

void SkydotLod::add_trees(godot::Node3D* parent, const Quad& q, Shown& stats) {
    if (tree_atlas_.is_null()) {
        return;
    }
    const auto bytes = pack_->get_bytes(vpath(q, "btt"));
    const auto* lod = read_lod(bytes);
    const auto* tree_list = lod != nullptr ? lod->trees() : nullptr;
    if (tree_list == nullptr || tree_list->size() == 0) {
        return;
    }
    Ref<godot::ArrayMesh>& quad = tree_quad_;
    if (quad.is_null()) {
        Array arrays;
        arrays.resize(godot::Mesh::ARRAY_MAX);
        godot::PackedVector3Array vertices;
        godot::PackedVector2Array uvs;
        for (const auto& [x, y] : {std::pair{-0.5F, 0.0F}, {0.5F, 0.0F}, {0.5F, 1.0F},
                                   {-0.5F, 0.0F}, {0.5F, 1.0F}, {-0.5F, 1.0F}}) {
            vertices.push_back(Vector3(x, y, 0.0F));
            uvs.push_back(godot::Vector2(x + 0.5F, 1.0F - y));
        }
        arrays[godot::Mesh::ARRAY_VERTEX] = vertices;
        arrays[godot::Mesh::ARRAY_TEX_UV] = uvs;
        quad.instantiate();
        quad->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays);
    }
    Ref<godot::MultiMesh> multi;
    multi.instantiate();
    multi->set_transform_format(godot::MultiMesh::TRANSFORM_3D);
    multi->set_use_custom_data(true);
    multi->set_mesh(quad);
    std::vector<const lfb::TreeInstance*> trees;
    for (const auto* t : *tree_list) {
        if (tree_types_.contains(t->type())) {
            trees.push_back(t);
        }
    }
    multi->set_instance_count(static_cast<std::int32_t>(trees.size()));
    const auto scale = static_cast<float>(SkydotWorld::UNIT_SCALE);
    float tallest = 0.0F;
    for (std::size_t i = 0; i < trees.size(); ++i) {
        const auto* t = trees[i];
        const auto& type = tree_types_[t->type()];
        const float w = type.width * t->scale() * scale;
        const float h = type.height * t->scale() * scale;
        tallest = std::max({tallest, w, h});
        const godot::Basis basis = godot::Basis().scaled(Vector3(w, h, 1.0F));
        const Vector3 origin = SkydotWorld::skyrim_position(Vector3(t->x(), t->y(), t->z()));
        multi->set_instance_transform(static_cast<std::int32_t>(i), godot::Transform3D(basis, origin));
        multi->set_instance_custom_data(static_cast<std::int32_t>(i),
                                        godot::Color(type.u0, type.v0, type.u1, type.v1));
    }
    auto* instance = memnew(godot::MultiMeshInstance3D);
    instance->set_name("trees");
    instance->set_multimesh(multi);
    instance->set_material_override(tree_material_);
    instance->set_cast_shadows_setting(godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    // The shader turns the quads, so the mesh's own bounds are too thin.
    godot::AABB bounds = multi->get_aabb();
    instance->set_custom_aabb(bounds.grow(tallest));
    parent->add_child(instance);
    stats.trees = trees.size();
}

godot::Node3D* SkydotLod::build(const Quad& q, bool trees, Shown& stats) {
    auto* root = memnew(godot::Node3D);
    root->set_name(String(trees ? "trees_" : "quad_") + String::num_int64(std::abs(q.level)) + String("_") +
                   String::num_int64(q.x) + String("_") + String::num_int64(q.y));
    if (trees) {
        add_trees(root, q, stats);
        return root;
    }
    for (const String& path : scenes_for(q)) {
        const auto it = resources_.find(utf8(path));
        const Ref<SkydotModel> scene = it != resources_.end() ? it->second : Ref<godot::Resource>();
        if (scene.is_null()) {
            continue;
        }
        auto* node = godot::Object::cast_to<godot::Node3D>(scene->instantiate());
        if (node == nullptr) {
            continue;
        }
        const bool terrain = path.get_base_dir().get_file() != "objects";
        if (terrain) {
            // Terrain vertices are relative to the quad's south-west corner.
            node->set_position(SkydotWorld::skyrim_position(
                Vector3(static_cast<float>(q.x * k_cell_units), static_cast<float>(q.y * k_cell_units), 0.0F)));
        } else {
            stats.objects = true;
        }
        retexture(node, terrain, false);
        root->add_child(node);
    }
    return root;
}

std::int64_t SkydotLod::update(const Vector3& camera, std::int64_t budget_usec) {
    if (stride_ <= 0) {
        return 0;
    }
    if (mask_dirty_) {
        mask_->update(mask_image_);
        mask_dirty_ = false;
    }
    const Vector3 game = SkydotWorld::godot_to_skyrim(camera);
    const double cx = static_cast<double>(game.x) / k_cell_units;
    const double cy = static_cast<double>(game.y) / k_cell_units;

    std::set<Quad> wanted;
    for (int y = south_west_y_; y < south_west_y_ + stride_; y += highest_) {
        for (int x = south_west_x_; x < south_west_x_ + stride_; x += highest_) {
            select(Quad{.level = highest_, .x = x, .y = y}, cx, cy, wanted);
        }
    }
    // Trees: level-4 quads within tree_distance (marked with a negative level).
    std::set<Quad> wanted_trees;
    if (tree_atlas_.is_valid() && !tree_types_.empty()) {
        const int r = static_cast<int>(std::ceil(tree_distance_ / lowest_)) + 1;
        const int qx = south_west_x_ + static_cast<int>(std::floor((cx - south_west_x_) / lowest_)) * lowest_;
        const int qy = south_west_y_ + static_cast<int>(std::floor((cy - south_west_y_) / lowest_)) * lowest_;
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                const Quad q{.level = -lowest_, .x = qx + dx * lowest_, .y = qy + dy * lowest_};
                const double ex = std::max({static_cast<double>(q.x) - cx, 0.0, cx - (q.x + lowest_)});
                const double ey = std::max({static_cast<double>(q.y) - cy, 0.0, cy - (q.y + lowest_)});
                if (std::sqrt(ex * ex + ey * ey) < tree_distance_ &&
                    pack_->has(vpath(Quad{.level = lowest_, .x = q.x, .y = q.y}, "btt"))) {
                    wanted_trees.insert(q);
                }
            }
        }
    }

    // Build what has loaded, nearest first, within the budget.
    std::vector<std::pair<double, Quad>> ready;
    std::int64_t pending = 0;
    const auto consider = [&](const Quad& q) {
        if (shown_.contains(q)) {
            return;
        }
        bool all = true;
        if (q.level > 0) {
            for (const auto& path : scenes_for(q)) {
                all = loaded(path) && all;
            }
        }
        const int size = std::abs(q.level);
        const double dx = q.x + size / 2.0 - cx;
        const double dy = q.y + size / 2.0 - cy;
        if (all) {
            ready.emplace_back(dx * dx + dy * dy, q);
        } else {
            ++pending;
        }
    };
    for (const auto& q : wanted) {
        consider(q);
    }
    for (const auto& q : wanted_trees) {
        consider(q);
    }
    std::ranges::sort(ready, {}, &std::pair<double, Quad>::first);
    const auto started = godot::Time::get_singleton()->get_ticks_usec();
    for (const auto& [distance, q] : ready) {
        if (static_cast<std::int64_t>(godot::Time::get_singleton()->get_ticks_usec() - started) > budget_usec) {
            ++pending;
            continue;
        }
        Shown shown;
        shown.node = build(q, q.level < 0, shown);
        add_child(shown.node);
        shown_.emplace(q, shown);
    }

    // Drop what is no longer wanted once what replaces it is up.
    for (auto it = shown_.begin(); it != shown_.end();) {
        const Quad& q = it->first;
        bool drop = false;
        if (q.level < 0) {
            drop = !wanted_trees.contains(q);
        } else if (!wanted.contains(q)) {
            drop = true;
            for (const auto& w : wanted) {
                if (w.overlaps(q) && !shown_.contains(w)) {
                    drop = false;
                    break;
                }
            }
        }
        if (drop) {
            it->second.node->queue_free();
            it = shown_.erase(it);
        } else {
            ++it;
        }
    }
    // Drop loaded scenes no quad holds and no wanted quad still needs.
    if (resources_.size() > 2 * wanted.size() + 64) {
        std::set<std::string> needed;
        for (const auto& q : wanted) {
            if (!shown_.contains(q)) {
                for (const auto& path : scenes_for(q)) {
                    needed.insert(utf8(path));
                }
            }
        }
        std::erase_if(resources_, [&](const auto& e) {
            return e.second.is_valid() && e.second->get_reference_count() <= 1 &&
                   !needed.contains(e.first);
        });
    }
    return pending;
}

Dictionary SkydotLod::get_stats() const {
    Dictionary out;
    Dictionary levels;
    std::int64_t objects = 0;
    std::int64_t tree_quads = 0;
    std::int64_t trees = 0;
    for (const auto& [q, s] : shown_) {
        if (q.level < 0) {
            ++tree_quads;
            trees += static_cast<std::int64_t>(s.trees);
            continue;
        }
        levels[q.level] = static_cast<std::int64_t>(levels.get(q.level, 0)) + 1;
        objects += s.objects ? 1 : 0;
    }
    out["levels"] = levels;
    out["objects"] = objects;
    out["tree_quads"] = tree_quads;
    out["trees"] = trees;
    out["pending"] = static_cast<std::int64_t>(pending_.size());
    out["loaded_cells"] = static_cast<std::int64_t>(loaded_cells_);
    return out;
}

} // namespace skydot
