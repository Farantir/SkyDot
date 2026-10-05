// SPDX-License-Identifier: GPL-3.0-or-later
#include "build/cell_builder.hpp"
#include "data/coordinates.hpp"
#include "data/fb_search.hpp"
#include "data/text.hpp"

#include "actors/actor.hpp"
#include "actors/actor_animation.hpp"
#include "data/actors.hpp"
#include "render/flicker.hpp"
#include "nav/navmesh.hpp"
#include "build/refs.hpp"

#include "skydot_formats/flags.hpp"
#include "skydot_formats/units.hpp"
#include "world_generated.h"

#include <godot_cpp/classes/animation_library.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

using godot::Dictionary;
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

/// Game units per exterior cell side.
constexpr auto k_cell_units = static_cast<float>(formats::k_cell_units);

/// XCLW values this large mean "no water here".
constexpr float k_no_water = 1.0e30F;


skydot::ActorPlan plan_for(const wfb::World& world, const wfb::ActorRef& actor,
                           const std::shared_ptr<AssetCache>& assets) {
    const auto exists = [&](const std::string& vpath) { return assets != nullptr && assets->has(vpath); };
    return plan_actor(world, actor.base(), actor.ref(), exists);
}

} // namespace

CellBuilder::CellBuilder(std::shared_ptr<const WorldData> data, ActorPlacement& placement)
    : data_(std::move(data)), placement_(placement), decorator_(data_, options_) {}

void CellBuilder::open(std::shared_ptr<const WorldData> data) {
    data_ = data;
    decorator_.open(std::move(data));
}

void CellBuilder::set_assets(std::shared_ptr<AssetCache> assets) {
    assets_ = assets;
    decorator_.set_assets(std::move(assets));
}

TerrainBuilder& CellBuilder::terrain_builder() {
    // Made again when the tiling changed: its materials are made for one.
    if (!terrain_ || terrain_tiling_built_ != options_.terrain_tiling) {
        terrain_ = std::make_unique<TerrainBuilder>(assets_);
        terrain_->set_tiling(static_cast<float>(options_.terrain_tiling));
        terrain_tiling_built_ = options_.terrain_tiling;
    }
    terrain_->set_collision(options_.collision);
    return *terrain_;
}

ActorPlan CellBuilder::actor_plan(const wfb::ActorRef& actor) const { return plan_for(*world_fb(), actor, assets_); }

/// What a build counts; the models' own counts (materials, billboards, effects, bodies) are
/// DecorationStats'.
struct CellBuilder::BuildStats : DecorationStats {
    std::int64_t refs = 0;
    std::int64_t placed = 0;
    std::int64_t lights = 0;
    std::int64_t markers = 0;
    std::int64_t disabled = 0;
    std::int64_t no_base = 0;
    std::int64_t flickers = 0;
    std::int64_t actors = 0;
    std::int64_t actor_parts = 0;
    /// Actors not built, by reason ("no NPC_", "no animation skeleton …").
    godot::Dictionary actor_failures;
    godot::PackedStringArray missing;
};

struct CellBuilder::BuildJob {
    /// References still to place, with the cell each belongs to.
    std::vector<std::pair<const wfb::Ref*, std::uint32_t>> refs;
    std::size_t next = 0;
    /// Actors, placed after the references.
    std::vector<ActorPlacement::ActorAt> actors;
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

void CellBuilder::place_ref(godot::Node3D* root, const wfb::Ref& ref, std::uint32_t cell,
                            BuildStats& stats, bool include_disabled) {
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
                    Decoration decoration{stats, &ref, base, cell, scene.ptr()};
                    decorator_.decorate(node, decoration);
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
        const auto range = static_cast<float>(static_cast<double>(radius) * formats::k_metres_per_unit);
        if (formats::has_flag(l->flags(), wfb::LightFlags::spot_light)) {
            auto* spot = memnew(godot::SpotLight3D);
            spot->set_param(godot::Light3D::PARAM_RANGE, range);
            spot->set_param(godot::Light3D::PARAM_SPOT_ANGLE, std::clamp(fov / 2.0F, 1.0F, 89.0F));
            spot->set_param(godot::Light3D::PARAM_ATTENUATION, 0.0F);
            const bool spot_shadow = formats::has_flag(l->flags(), wfb::LightFlags::spot_shadow);
            spot->set_shadow(options_.all_light_shadows || spot_shadow);
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
            omni->set_shadow(options_.all_light_shadows || formats::has_flag(l->flags(), k_light_shadow));
            omni->set_meta("skydot_game_shadow", formats::has_flag(l->flags(), k_light_shadow));
            light = omni;
        }
        light->set_name(name + String(" light"));
        SkydotMaterials::set_game_light(light, unpack_color(l->color()) * std::max(fade, 0.0F));
        light->set_negative(formats::has_flag(l->flags(), wfb::LightFlags::negative));
        light->set_transform(placed);
        if (options_.effects && formats::has_flag(l->flags(), SkydotFlicker::ANY)) {
            auto* flicker = memnew(SkydotFlicker);
            flicker->set_name("SkydotFlicker");
            flicker->configure(static_cast<std::int64_t>(l->flags()),
                               static_cast<double>(l->flicker_period()),
                               static_cast<double>(l->flicker_intensity()),
                               static_cast<double>(l->flicker_movement()) * formats::k_metres_per_unit);
            light->add_child(flicker);
            ++stats.flickers;
        }
        root->add_child(light);
        ++stats.lights;
    }
}

std::int64_t CellBuilder::apply_light_shadows(godot::Node* root, bool all) {
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

GrassModel CellBuilder::grass_model(const wfb::Grass& grass) {
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

Dictionary CellBuilder::stats_dictionary(const BuildStats& stats) const {
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

godot::Node3D* CellBuilder::build_ref(std::int64_t cell_id, std::int64_t ref_id) {
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

godot::Node3D* CellBuilder::build_cell(std::int64_t id) {
    auto* root = begin_cell(id);
    if (root != nullptr) {
        continue_build(root, std::numeric_limits<std::int64_t>::max());
    }
    return root;
}

godot::Node3D* CellBuilder::build_exterior(std::int64_t world, std::int64_t x,
                                           std::int64_t y) {
    auto* root = begin_exterior(world, x, y);
    if (root != nullptr) {
        continue_build(root, std::numeric_limits<std::int64_t>::max());
    }
    return root;
}

bool CellBuilder::continue_build(godot::Node3D* root, std::int64_t budget_usec) {
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
        if (options_.actors) {
            job.actors = job.exterior ? placement_.actors_in_grid(job.world, job.x, job.y)
                                          : placement_.actors_in_cell(job.cell);
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

bool CellBuilder::place_refs(godot::Node3D* root, BuildJob& job, std::uint64_t started,
                             std::int64_t budget_usec) {
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

bool CellBuilder::continue_build_static(godot::Node3D* root, std::int64_t budget_usec) {
    if (root == nullptr) {
        return true;
    }
    const auto it = jobs_.find(root->get_instance_id());
    if (it == jobs_.end()) {
        return true;
    }
    return place_refs(root, *it->second, godot::Time::get_singleton()->get_ticks_usec(), budget_usec);
}

godot::Node3D* CellBuilder::begin_exterior(std::int64_t world, std::int64_t x,
                                           std::int64_t y) {
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
        const auto neighbours = [&](int dx, int dy) {
            const auto* n = data().exterior_ptr(land, gx + dx, gy + dy);
            return n != nullptr && n->terrain() != nullptr ? TerrainBuilder::heights(*n->terrain())
                                                           : std::vector<float>{};
        };
        const auto paths = [&](std::uint32_t id) { return land_texture_paths(id); };
        auto* node = terrain_builder().build(*terrain, neighbours, paths);
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
        const auto material = decorator_.water_material(data().water_ptr(data().water_type(w, cell)));
        godot::Ref<godot::PlaneMesh> plane;
        plane.instantiate();
        const auto side = static_cast<float>(static_cast<double>(k_cell_units) * formats::k_metres_per_unit);
        plane->set_size(godot::Vector2(side, side));
        plane->set_material(material);
        auto* surface = memnew(godot::MeshInstance3D);
        surface->set_name("Water");
        surface->set_mesh(plane);
        surface->set_position(corner + Vector3(side / 2,
                                               static_cast<float>(static_cast<double>(water_height) * formats::k_metres_per_unit),
                                               -side / 2));
        root->add_child(surface);
    }

    if (terrain != nullptr && options_.grass && options_.skyrim_materials) {
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

    if (cell != nullptr && options_.navigation) {
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
    if (options_.actors) {
        keep_actor_resources(*job, placement_.actors_in_grid(w, gx, gy));
    }
    add_job(root, std::move(job));
    return root;
}

godot::Node3D* CellBuilder::begin_cell(std::int64_t id) {
    const auto* cell = data().cell_ptr(id);
    if (cell == nullptr) {
        godot::UtilityFunctions::push_error("SkydotWorld: no cell ", hex_id(static_cast<std::uint32_t>(id)));
        return nullptr;
    }
    auto* root = memnew(godot::Node3D);
    root->set_name(cell->editor_id() != nullptr && cell->editor_id()->size() != 0
                       ? to_godot(cell->editor_id())
                       : hex_id(cell->id()));
    if (options_.navigation) {
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
    if (options_.actors) {
        keep_actor_resources(*job, placement_.actors_in_cell(cell->id()));
    }
    add_job(root, std::move(job));
    return root;
}

void CellBuilder::keep_actor_resources(BuildJob& job, const std::vector<ActorPlacement::ActorAt>& actors) {
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

void CellBuilder::add_job(godot::Node3D* root, std::shared_ptr<BuildJob> job) {
    // Builds whose roots were freed unfinished are dropped here.
    std::erase_if(jobs_, [](const auto& entry) {
        return godot::ObjectDB::get_instance(entry.first) == nullptr;
    });
    jobs_.emplace(root->get_instance_id(), std::move(job));
}

std::vector<std::string> CellBuilder::actor_resources(const wfb::ActorRef& actor) {
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

CellBuilder::ActorClips CellBuilder::actor_clips(const ActorPlan& plan) {
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

const Locomotion& CellBuilder::locomotion_of(const std::string& behaviour) {
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

godot::Ref<godot::Animation> CellBuilder::actor_clip(const std::string& file,
                                                     const std::string& skeleton_path) const {
    if (assets_ == nullptr) {
        return {};
    }
    return assets_->clip(file, skeleton_path);
}

godot::Node3D* CellBuilder::build_actor(std::int64_t ref) {
    const auto* actor = data().actor_ptr(ref);
    if (actor == nullptr) {
        return nullptr;
    }
    auto* root = memnew(godot::Node3D);
    root->set_name(hex_id(actor->ref()));
    BuildStats stats;
    place_actor(root, *actor, stats, placement_.moved_place(actor->ref()));
    root->set_meta("skydot_stats", stats_dictionary(stats));
    return root;
}

void CellBuilder::place_actor(godot::Node3D* root, const wfb::ActorRef& actor, BuildStats& stats,
                              const ActorPlacement::Place* place) {
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
            .scaled(Vector3(1, 1, 1) * static_cast<float>(formats::k_metres_per_unit) * plan.scale),
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
        if (options_.skyrim_materials) {
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
    const double stride = formats::k_metres_per_unit * static_cast<double>(plan.scale);
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
    node->set_wander(options_.actor_wander);
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

std::array<std::string, 2> CellBuilder::land_texture_paths(std::uint32_t id) const {
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

godot::Ref<godot::Resource> CellBuilder::resource(const String& vpath) const { return decorator_.resource(vpath); }

void CellBuilder::add_ref_resources(const wfb::Ref& ref, godot::PackedStringArray& out) const {
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

void CellBuilder::add_actor_resources(const std::vector<ActorPlacement::ActorAt>& actors, godot::PackedStringArray& out) {
    if (!options_.actors) {
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

godot::PackedStringArray CellBuilder::cell_resources(std::int64_t id) {
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
    add_actor_resources(placement_.actors_in_cell(cell->id()), out);
    return out;
}

std::int64_t CellBuilder::request_cell(std::int64_t id) {
    return request_all(cell_resources(id));
}

godot::PackedStringArray CellBuilder::exterior_resources(std::int64_t world, std::int64_t x,
                                                             std::int64_t y) {
    godot::PackedStringArray out;
    const auto w = static_cast<std::uint32_t>(world);
    const auto gx = static_cast<std::int32_t>(x);
    const auto gy = static_cast<std::int32_t>(y);
    if (const auto* cell = data().exterior_ptr(w, gx, gy); cell != nullptr && cell->refs() != nullptr) {
        for (const auto* ref : *cell->refs()) {
            add_ref_resources(*ref, out);
        }
    }
    if (options_.actors) {
        add_actor_resources(placement_.actors_in_grid(w, gx, gy), out);
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
            const auto* textures = options_.grass ? world_fb()->land_textures() : nullptr;
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

std::int64_t CellBuilder::request_exterior(std::int64_t world, std::int64_t x, std::int64_t y) {
    return request_all(exterior_resources(world, x, y));
}

std::int64_t CellBuilder::request_all(const godot::PackedStringArray& paths) {
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

std::int64_t CellBuilder::cached_resource_count() const {
    return assets_ != nullptr ? static_cast<std::int64_t>(assets_->cached()) : 0;
}

std::int64_t CellBuilder::pending_resource_count() const {
    return assets_ != nullptr ? static_cast<std::int64_t>(assets_->pending()) : 0;
}

void CellBuilder::trim_cache() {
    if (assets_ != nullptr) {
        assets_->trim();
    }
}

std::int64_t CellBuilder::warm_up() {
    terrain_builder().warm_up();
    return decorator_.warm_up() + TerrainBuilder::k_max_layers + 1;
}

} // namespace skydot
