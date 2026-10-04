// SPDX-License-Identifier: GPL-3.0-or-later
#include "places.hpp"

#include "fb_write.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/forms_world.hpp"
#include "skydot_formats/flags.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>

namespace bethconv::pack::detail {
namespace {

using io::FourCC;
using skydot::formats::has_flag;

static_assert(same_bit(wfb::CellFlags::interior, record::Cell::Flag::interior) &&
              same_bit(wfb::CellFlags::has_water, record::Cell::Flag::has_water) &&
              same_bit(wfb::CellFlags::cant_travel_from_here,
                       record::Cell::Flag::cant_travel_from_here) &&
              same_bit(wfb::CellFlags::no_lod_water, record::Cell::Flag::no_lod_water) &&
              same_bit(wfb::CellFlags::public_area, record::Cell::Flag::public_area) &&
              same_bit(wfb::CellFlags::hand_changed, record::Cell::Flag::hand_changed) &&
              same_bit(wfb::CellFlags::show_sky, record::Cell::Flag::show_sky) &&
              same_bit(wfb::CellFlags::use_sky_lighting, record::Cell::Flag::use_sky_lighting));

/// VHGT: a float offset, 33 x 33 signed deltas, 3 bytes of padding.
std::optional<std::pair<float, std::vector<std::int8_t>>> decode_vhgt(
    std::span<const std::byte> raw) {
    constexpr std::size_t count = WorldTerrain::k_grid * WorldTerrain::k_grid;
    if (raw.size() < 4 + count) {
        return std::nullopt;
    }
    io::SpanReader r(raw, "VHGT");
    const float offset = r.get<float>().value_or(0.0F);
    std::vector<std::int8_t> deltas;
    deltas.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        deltas.push_back(r.get<std::int8_t>().value_or(0));
    }
    return std::pair{offset, std::move(deltas)};
}

/// XCLL, 92-byte layout (source: UESP, CELL record, XCLL). The 64-byte pre-1.70
/// form is not decoded.
std::optional<WorldCellLighting> decode_xcll(std::span<const std::byte> raw) {
    if (raw.size() < 92) {
        return std::nullopt;
    }
    io::SpanReader r(raw, "XCLL");
    WorldCellLighting out;
    const auto u32 = [&]() { return r.get<std::uint32_t>().value_or(0); };
    const auto i32 = [&]() { return r.get<std::int32_t>().value_or(0); };
    const auto f32 = [&]() { return r.get<float>().value_or(0.0F); };
    out.ambient = u32();
    out.directional = u32();
    out.fog_near_color = u32();
    out.fog_near = f32();
    out.fog_far = f32();
    out.directional_rotation_xy = i32();
    out.directional_rotation_z = i32();
    out.directional_fade = f32();
    (void)f32(); // fog clip distance
    out.fog_power = f32();
    for (auto& colour : out.directional_ambient) {
        colour = u32();
    }
    (void)r.skip(4 + 4); // specular, fresnel power
    out.fog_far_color = u32();
    out.fog_max = f32();
    out.light_fade_begin = f32();
    out.light_fade_end = f32();
    out.inherit = u32();
    return out;
}

/// XCLL inherit flags (UESP, CELL record): set bits take the value from the
/// lighting template. The directional ambient goes with the ambient colour.
constexpr std::uint32_t k_inherit_ambient = 0x1;
constexpr std::uint32_t k_inherit_directional = 0x2;
constexpr std::uint32_t k_inherit_fog_color = 0x4;
constexpr std::uint32_t k_inherit_fog_near = 0x8;
constexpr std::uint32_t k_inherit_fog_far = 0x10;
constexpr std::uint32_t k_inherit_rotation = 0x20;
constexpr std::uint32_t k_inherit_fade = 0x40;
constexpr std::uint32_t k_inherit_fog_power = 0x100;
constexpr std::uint32_t k_inherit_fog_max = 0x200;
constexpr std::uint32_t k_inherit_light_fade = 0x400;

/// The lighting of an interior as the game uses it: XCLL with the inherited
/// values taken from the lighting template, or the template's alone when the
/// cell has no XCLL. Exteriors keep XCLL as it is.
std::optional<WorldCellLighting> resolve_lighting(
    const CellEntry& cell, const std::map<std::uint32_t, LightingTemplateEntry>& templates) {
    const auto it = templates.find(cell.lighting_template);
    if (!has_flag(cell.flags, wfb::CellFlags::interior) || it == templates.end()) {
        return cell.lighting;
    }
    const auto& t = it->second.lighting;
    if (!cell.lighting) {
        return t;
    }
    WorldCellLighting out = *cell.lighting;
    const auto inherits = [&](std::uint32_t bit) { return (out.inherit & bit) != 0; };
    if (inherits(k_inherit_ambient)) {
        out.ambient = t.ambient;
        out.directional_ambient = t.directional_ambient;
    }
    if (inherits(k_inherit_directional)) {
        out.directional = t.directional;
    }
    if (inherits(k_inherit_fog_color)) {
        out.fog_near_color = t.fog_near_color;
        out.fog_far_color = t.fog_far_color;
    }
    if (inherits(k_inherit_fog_near)) {
        out.fog_near = t.fog_near;
    }
    if (inherits(k_inherit_fog_far)) {
        out.fog_far = t.fog_far;
    }
    if (inherits(k_inherit_rotation)) {
        out.directional_rotation_xy = t.directional_rotation_xy;
        out.directional_rotation_z = t.directional_rotation_z;
    }
    if (inherits(k_inherit_fade)) {
        out.directional_fade = t.directional_fade;
    }
    if (inherits(k_inherit_fog_power)) {
        out.fog_power = t.fog_power;
    }
    if (inherits(k_inherit_fog_max)) {
        out.fog_max = t.fog_max;
    }
    if (inherits(k_inherit_light_fade)) {
        out.light_fade_begin = t.light_fade_begin;
        out.light_fade_end = t.light_fade_end;
    }
    return out;
}

/// A cell's navmeshes in id order.
std::vector<flatbuffers::Offset<wfb::NavMesh>> write_navmeshes(
    flatbuffers::FlatBufferBuilder& builder, std::vector<WorldNavMesh>& navmeshes,
    WorldStats& stats) {
    std::vector<flatbuffers::Offset<wfb::NavMesh>> out;
    std::ranges::sort(navmeshes, {}, &WorldNavMesh::id);
    for (const auto& nav : navmeshes) {
        std::vector<wfb::Vec3f> vertices;
        vertices.reserve(nav.vertices.size());
        for (const auto& v : nav.vertices) {
            vertices.push_back(to_fb(v));
        }
        std::vector<wfb::NavTriangle> triangles;
        triangles.reserve(nav.triangles.size());
        for (const auto& t : nav.triangles) {
            triangles.emplace_back(t.vertices[0], t.vertices[1], t.vertices[2],
                                   t.edges[0], t.edges[1], t.edges[2], t.flags, t.cover);
        }
        std::vector<wfb::NavLink> nav_links;
        for (const auto& l : nav.links) {
            nav_links.emplace_back(l.type, l.navmesh, l.triangle);
        }
        std::vector<wfb::NavDoor> nav_doors;
        for (const auto& d : nav.doors) {
            nav_doors.emplace_back(d.triangle, d.door);
        }
        const auto v_off = builder.CreateVectorOfStructs(vertices);
        const auto t_off = builder.CreateVectorOfStructs(triangles);
        const auto l_off = builder.CreateVectorOfStructs(nav_links);
        const auto d_off = builder.CreateVectorOfStructs(nav_doors);
        out.push_back(wfb::CreateNavMesh(builder, nav.id, v_off, t_off, l_off, d_off));
        ++stats.navmeshes;
        stats.nav_triangles += nav.triangles.size();
    }
    return out;
}

/// A cell's terrain: heights, vertex colours and texture layers.
flatbuffers::Offset<wfb::Terrain> write_terrain(flatbuffers::FlatBufferBuilder& builder,
                                                const WorldTerrain& terrain, WorldStats& stats) {
    std::vector<flatbuffers::Offset<wfb::TerrainLayer>> layers;
    layers.reserve(terrain.layers.size());
    for (const auto& layer : terrain.layers) {
        const auto points = builder.CreateVector(layer.points);
        const auto opacity = builder.CreateVector(layer.opacity);
        layers.push_back(wfb::CreateTerrainLayer(builder, layer.texture, layer.quadrant,
                                                 layer.layer, points, opacity));
    }
    const auto deltas = builder.CreateVector(terrain.height_deltas);
    const auto colours = builder.CreateVector(terrain.colours);
    const auto layers_off = builder.CreateVector(layers);
    const auto out =
        wfb::CreateTerrain(builder, terrain.height_offset, deltas, colours, layers_off);
    ++stats.terrains;
    stats.terrain_layers += terrain.layers.size();
    return out;
}

} // namespace

void PlaceCollector::collect(const record::MergedRecord& merged, const record::RecordContext& ctx,
                             io::SpanReader& data, const record::FormContext& form_ctx) {
    switch (merged.type.value) {
    case FourCC{"CELL"}.value:
        on_cell(merged, data, form_ctx);
        break;
    case FourCC{"REFR"}.value:
        on_reference(merged, ctx, data, form_ctx);
        break;
    case FourCC{"ACHR"}.value:
        on_actor(merged, ctx, data, form_ctx);
        break;
    case FourCC{"LAND"}.value:
        on_land(merged, data, form_ctx);
        break;
    case FourCC{"NAVM"}.value:
        on_navmesh(merged, data, form_ctx);
        break;
    case FourCC{"LGTM"}.value:
        on_lighting_template(merged, data);
        break;
    default:
        break;
    }
}

void PlaceCollector::on_cell(const record::MergedRecord& merged, io::SpanReader& data,
                             const record::FormContext& form_ctx) {
    auto cell = record::parse_cell(data, form_ctx);
    if (!cell) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    CellEntry entry{
        .id = merged.form.value,
        .editor_id = cell->editor_id,
        .world = cell->is_interior() ? 0 : merged.parent.value,
        .flags = static_cast<wfb::CellFlags>(cell->flags),
        .grid = cell->grid,
        .water_height = cell->water_height,
        .lighting = decode_xcll(cell->lighting),
        .lighting_template = shared_.global(merged, cell->lighting_template, failed),
        .image_space = shared_.global(merged, cell->image_space, failed),
        .persistent = record::has_flag(merged.flags, record::RecordFlag::persistent),
        .water = shared_.global(merged, cell->water, failed),
    };
    if (failed) {
        ++shared_.stats().unresolved;
    }
    cells_[entry.id] = std::move(entry);
}

void PlaceCollector::on_reference(const record::MergedRecord& merged,
                                  const record::RecordContext& ctx, io::SpanReader& data,
                                  const record::FormContext& form_ctx) {
    auto ref = record::parse_reference(ctx.header, data, form_ctx);
    if (!ref) {
        ++shared_.stats().parse_errors;
        return;
    }
    if (merged.parent.is_null()) {
        ++shared_.stats().orphan_refs;
        return;
    }
    bool failed = false;
    WorldRef out{
        .id = merged.form.value,
        .base = shared_.global(merged, ref->base, failed),
        .position = ref->position,
        .rotation = ref->rotation,
        .scale = ref->scale,
        .flags = {},
        .enable_parent = 0,
    };
    if (ref->initially_disabled) {
        out.flags |= wfb::RefFlags::initially_disabled;
    }
    if (ref->persistent) {
        out.flags |= wfb::RefFlags::persistent;
    }
    if (ref->enable_parent) {
        out.enable_parent = shared_.global(merged, ref->enable_parent->parent, failed);
        if (ref->enable_parent->set_enable_state_opposite()) {
            out.flags |= wfb::RefFlags::enable_opposite;
        }
    }
    if ((ref->activate_parent_flags & 0x01u) != 0) {
        out.flags |= wfb::RefFlags::parent_activate_only;
    }
    auto& extras = extras_[merged.parent.value];
    if (!ref->scripts.empty()) {
        extras.scripts.push_back(WorldRefScripts{
            .ref = out.id,
            .scripts = shared_.global_scripts(merged, std::move(ref->scripts), failed),
        });
    }
    if (ref->has_radius || ref->light_data.size() >= 16) {
        WorldLightOverride light{.ref = out.id, .has_radius = ref->has_radius, .radius = ref->radius};
        if (ref->light_data.size() >= 16) {
            io::SpanReader r(ref->light_data, "REFR XLIG");
            light.has_light_data = true;
            light.fov = r.get<float>().value_or(0.0F);
            light.fade = r.get<float>().value_or(0.0F);
            light.end_distance_cap = r.get<float>().value_or(0.0F);
            light.shadow_depth_bias = r.get<float>().value_or(0.0F);
        }
        extras.light_overrides.push_back(light);
    }
    if (ref->lock) {
        extras.locks.push_back(WorldLock{
            .ref = out.id,
            .level = ref->lock->level,
            .flags = ref->lock->flags,
            .key = shared_.global(merged, ref->lock->key, failed),
        });
    }
    for (const auto& link : ref->linked_references) {
        extras.links.push_back(WorldLink{
            .ref = out.id,
            .keyword = shared_.global(merged, link.keyword, failed),
            .target = shared_.global(merged, link.target, failed),
        });
    }
    for (const auto& parent : ref->activate_parents) {
        extras.activate_parents.push_back(WorldActivateParent{
            .ref = out.id,
            .parent = shared_.global(merged, parent.ref, failed),
            .delay = parent.delay,
        });
    }
    if (ref->primitive) {
        extras.primitives.push_back(WorldPrimitive{
            .ref = out.id,
            .bounds = ref->primitive->bounds,
            .type = ref->primitive->type,
        });
    }
    if (ref->teleport) {
        doors_[merged.parent.value].push_back(WorldDoor{
            .ref = out.id,
            .destination = shared_.global(merged, ref->teleport->destination_door, failed),
            .position = ref->teleport->position,
            .rotation = ref->teleport->rotation,
        });
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    refs_[merged.parent.value].push_back(out);
}

/// LAND's parent is its CELL. Texture FormIDs are made global; an
/// unresolvable one falls back to the default texture (0).
void PlaceCollector::on_land(const record::MergedRecord& merged, io::SpanReader& data,
                             const record::FormContext& form_ctx) {
    auto land = record::parse_landscape(data, form_ctx);
    if (!land) {
        ++shared_.stats().parse_errors;
        return;
    }
    auto heights = decode_vhgt(land->heights);
    if (!heights || merged.parent.is_null()) {
        return;
    }
    bool failed = false;
    WorldTerrain terrain;
    terrain.height_offset = heights->first;
    terrain.height_deltas = std::move(heights->second);
    if (land->vertex_colours.size() == record::Landscape::k_vnml_size) {
        terrain.colours.reserve(land->vertex_colours.size());
        for (const std::byte b : land->vertex_colours) {
            terrain.colours.push_back(static_cast<std::uint8_t>(b));
        }
    }
    for (const auto& base : land->base_layers) {
        terrain.layers.push_back(WorldTerrainLayer{
            .texture = shared_.global(merged, base.texture, failed),
            .quadrant = base.quadrant,
            .layer = -1,
            .points = {},
            .opacity = {},
        });
    }
    for (const auto& extra : land->additional_layers) {
        WorldTerrainLayer layer{
            .texture = shared_.global(merged, extra.texture, failed),
            .quadrant = extra.quadrant,
            .layer = extra.layer,
            .points = {},
            .opacity = {},
        };
        // VTXT, 8 bytes a point: vertex index, an unknown word, opacity.
        io::SpanReader r(extra.alpha_map, "VTXT");
        while (r.remaining() >= record::Landscape::k_alpha_point_size) {
            const auto point = r.get<std::uint16_t>().value_or(0);
            (void)r.get<std::uint16_t>();
            const float opacity = std::clamp(r.get<float>().value_or(0.0F), 0.0F, 1.0F);
            layer.points.push_back(point);
            layer.opacity.push_back(static_cast<std::uint8_t>(opacity * 255.0F + 0.5F));
        }
        terrain.layers.push_back(std::move(layer));
    }
    std::ranges::stable_sort(terrain.layers, [](const auto& a, const auto& b) {
        return std::pair{a.quadrant, a.layer} < std::pair{b.quadrant, b.layer};
    });
    if (failed) {
        ++shared_.stats().unresolved;
    }
    terrains_[merged.parent.value] = std::move(terrain);
}

void PlaceCollector::on_actor(const record::MergedRecord& merged,
                              const record::RecordContext& ctx, io::SpanReader& data,
                              const record::FormContext& form_ctx) {
    auto actor = record::parse_actor_reference(ctx.header, data, form_ctx);
    if (!actor) {
        ++shared_.stats().parse_errors;
        return;
    }
    if (merged.parent.is_null()) {
        ++shared_.stats().orphan_refs;
        return;
    }
    bool failed = false;
    WorldActor out{
        .ref = merged.form.value,
        .base = shared_.global(merged, actor->base, failed),
        .cell = merged.parent.value,
        .position = actor->position,
        .rotation = actor->rotation,
        .flags = {},
    };
    if (actor->initially_disabled) {
        out.flags |= wfb::RefFlags::initially_disabled;
    }
    if (actor->persistent) {
        out.flags |= wfb::RefFlags::persistent;
    }
    // Packages walk to linked references (beds, work markers, patrols).
    for (const auto& link : actor->linked_references) {
        extras_[merged.parent.value].links.push_back(WorldLink{
            .ref = out.ref,
            .keyword = shared_.global(merged, link.keyword, failed),
            .target = shared_.global(merged, link.target, failed),
        });
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    actors_.push_back(out);
}

/// NAVM's parent is its CELL. Edge link and door FormIDs are made global.
void PlaceCollector::on_navmesh(const record::MergedRecord& merged, io::SpanReader& data,
                                const record::FormContext& form_ctx) {
    auto navm = record::parse_nav_mesh(data, form_ctx);
    if (!navm) {
        ++shared_.stats().parse_errors;
        return;
    }
    auto geometry = record::decode_nav_mesh_geometry(navm->geometry);
    if (!geometry) {
        ++shared_.stats().parse_errors;
        return;
    }
    if (merged.parent.is_null()) {
        ++shared_.stats().orphan_navmeshes;
        return;
    }
    bool failed = false;
    WorldNavMesh out;
    out.id = merged.form.value;
    out.vertices = std::move(geometry->vertices);
    out.triangles.reserve(geometry->triangles.size());
    for (const auto& t : geometry->triangles) {
        out.triangles.push_back({.vertices = t.vertices, .edges = t.edges,
                                 .flags = static_cast<wfb::NavTriangleFlags>(t.flags),
                                 .cover = t.cover});
    }
    for (const auto& l : geometry->edge_links) {
        out.links.push_back({.type = l.type,
                             .navmesh = shared_.global(merged, l.navmesh, failed),
                             .triangle = l.triangle});
    }
    for (const auto& d : geometry->doors) {
        out.doors.push_back(
            {.triangle = d.triangle, .door = shared_.global(merged, d.door, failed)});
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    navmeshes_[merged.parent.value].push_back(std::move(out));
}

/// LGTM (UESP, LGTM record): DATA as XCLL, DALC 32 bytes.
void PlaceCollector::on_lighting_template(const record::MergedRecord& merged,
                                          io::SpanReader& data) {
    LightingTemplateEntry out;
    bool has_data = false;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"DATA"}) {
                const auto raw = body.bytes(body.remaining());
                if (raw) {
                    if (auto decoded = decode_xcll(*raw)) {
                        const auto ambient = out.lighting.directional_ambient;
                        out.lighting = *decoded;
                        out.lighting.directional_ambient = ambient;
                        out.lighting.inherit = 0;
                        has_data = true;
                    }
                }
            } else if (field.type == FourCC{"DALC"} && body.remaining() >= 24) {
                for (auto& colour : out.lighting.directional_ambient) {
                    colour = body.get<std::uint32_t>().value_or(0);
                }
            }
        });
    if (!walked || !has_data) {
        ++shared_.stats().parse_errors;
        return;
    }
    lighting_templates_[merged.form.value] = std::move(out);
}

std::vector<flatbuffers::Offset<wfb::Cell>> PlaceCollector::write_cells(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Cell>> cells;
    cells.reserve(cells_.size());
    for (auto& [id, cell] : cells_) { // std::map: sorted by id
        auto& refs = refs_[id];
        std::ranges::sort(refs, {}, &WorldRef::id);
        auto& doors = doors_[id];
        std::ranges::sort(doors, {}, &WorldDoor::ref);

        std::vector<wfb::Ref> fb_refs;
        fb_refs.reserve(refs.size());
        for (const auto& r : refs) {
            fb_refs.emplace_back(r.id, r.base, to_fb(r.position), to_fb(r.rotation), r.scale,
                                 r.flags, r.enable_parent);
        }
        std::vector<wfb::DoorLink> fb_doors;
        fb_doors.reserve(doors.size());
        for (const auto& d : doors) {
            fb_doors.emplace_back(d.ref, d.destination, to_fb(d.position), to_fb(d.rotation));
        }
        stats.refs += refs.size();
        stats.doors += doors.size();

        auto& extras = extras_[id];
        std::ranges::stable_sort(extras.scripts, {}, &WorldRefScripts::ref);
        std::ranges::stable_sort(extras.locks, {}, &WorldLock::ref);
        std::ranges::stable_sort(extras.links, {}, &WorldLink::ref);
        std::ranges::stable_sort(extras.activate_parents, {}, &WorldActivateParent::ref);
        std::ranges::stable_sort(extras.primitives, {}, &WorldPrimitive::ref);
        std::ranges::stable_sort(extras.light_overrides, {}, &WorldLightOverride::ref);
        std::vector<flatbuffers::Offset<wfb::RefScripts>> fb_scripts;
        fb_scripts.reserve(extras.scripts.size());
        for (const auto& r : extras.scripts) {
            fb_scripts.push_back(
                wfb::CreateRefScripts(builder, r.ref, write_scripts(builder, r.scripts)));
            stats.scripts += r.scripts.size();
        }
        std::vector<wfb::Lock> fb_locks;
        for (const auto& l : extras.locks) {
            fb_locks.emplace_back(l.ref, l.level, l.flags, l.key);
        }
        std::vector<wfb::LinkedRef> fb_links;
        for (const auto& l : extras.links) {
            fb_links.emplace_back(l.ref, l.keyword, l.target);
        }
        std::vector<wfb::ActivateParent> fb_parents;
        for (const auto& a : extras.activate_parents) {
            fb_parents.emplace_back(a.ref, a.parent, a.delay);
        }
        std::vector<wfb::Primitive> fb_primitives;
        for (const auto& p : extras.primitives) {
            fb_primitives.emplace_back(p.ref, to_fb(p.bounds), p.type);
        }
        stats.locks += fb_locks.size();
        stats.links += fb_links.size();
        stats.activate_parents += fb_parents.size();
        stats.primitives += fb_primitives.size();
        const auto scripts_off = builder.CreateVector(fb_scripts);
        const auto locks_off = builder.CreateVectorOfStructs(fb_locks);
        const auto links_off = builder.CreateVectorOfStructs(fb_links);
        const auto parents_off = builder.CreateVectorOfStructs(fb_parents);
        const auto primitives_off = builder.CreateVectorOfStructs(fb_primitives);
        std::vector<wfb::LightOverride> fb_lights;
        for (const auto& l : extras.light_overrides) {
            fb_lights.emplace_back(l.ref, l.has_radius, l.radius, l.has_light_data, l.fov, l.fade,
                                   l.end_distance_cap, l.shadow_depth_bias);
        }
        const auto light_overrides_off = builder.CreateVectorOfStructs(fb_lights);

        std::vector<flatbuffers::Offset<wfb::NavMesh>> fb_navmeshes;
        if (const auto n = navmeshes_.find(id); n != navmeshes_.end()) {
            fb_navmeshes = write_navmeshes(builder, n->second, stats);
        }
        const auto navmeshes_off = builder.CreateVector(fb_navmeshes);

        const auto editor_id = builder.CreateString(cell.editor_id);
        const auto refs_off = builder.CreateVectorOfStructs(fb_refs);
        const auto doors_off = builder.CreateVectorOfStructs(fb_doors);

        flatbuffers::Offset<wfb::Terrain> terrain_off;
        if (const auto t = terrains_.find(id); t != terrains_.end()) {
            terrain_off = write_terrain(builder, t->second, stats);
        }

        const auto resolved = resolve_lighting(cell, lighting_templates_);
        std::optional<wfb::CellLighting> lighting;
        flatbuffers::Offset<flatbuffers::Vector<std::uint32_t>> ambient_off;
        if (resolved) {
            const auto& l = *resolved;
            if (std::ranges::any_of(l.directional_ambient, [](std::uint32_t c) { return c != 0; })) {
                ambient_off = builder.CreateVector(l.directional_ambient.data(), l.directional_ambient.size());
            }
            lighting = wfb::CellLighting(
                l.ambient, l.directional, l.fog_near_color, l.fog_far_color, l.fog_near,
                l.fog_far, l.fog_power, l.fog_max, l.directional_rotation_xy,
                l.directional_rotation_z, l.directional_fade, l.light_fade_begin,
                l.light_fade_end, l.inherit);
        }

        wfb::CellBuilder cb(builder);
        cb.add_id(cell.id);
        cb.add_editor_id(editor_id);
        cb.add_world(cell.world);
        cb.add_flags(cell.flags);
        if (cell.grid) {
            cb.add_has_grid(true);
            cb.add_grid_x(cell.grid->x);
            cb.add_grid_y(cell.grid->y);
        }
        cb.add_water_height(cell.water_height);
        if (lighting) {
            cb.add_has_lighting(true);
            cb.add_lighting(&*lighting);
        }
        cb.add_lighting_template(cell.lighting_template);
        cb.add_refs(refs_off);
        cb.add_doors(doors_off);
        if (!terrain_off.IsNull()) {
            cb.add_terrain(terrain_off);
        }
        cb.add_persistent(cell.persistent);
        cb.add_water(cell.water);
        cb.add_scripts(scripts_off);
        cb.add_locks(locks_off);
        cb.add_links(links_off);
        cb.add_activate_parents(parents_off);
        cb.add_primitives(primitives_off);
        cb.add_navmeshes(navmeshes_off);
        if (!ambient_off.IsNull()) {
            cb.add_directional_ambient(ambient_off);
        }
        cb.add_image_space(cell.image_space);
        cb.add_light_overrides(light_overrides_off);
        cells.push_back(cb.Finish());

        ++stats.cells;
        if (has_flag(cell.flags, wfb::CellFlags::interior)) {
            ++stats.interior_cells;
        }
    }
    // References whose parent is not a cell record.
    for (const auto& [parent, refs] : refs_) {
        if (!cells_.contains(parent)) {
            stats.orphan_refs += refs.size();
        }
    }
    for (const auto& [parent, navmeshes] : navmeshes_) {
        if (!cells_.contains(parent)) {
            stats.orphan_navmeshes += navmeshes.size();
        }
    }
    stats.lighting_templates = lighting_templates_.size();
    return cells;
}

flatbuffers::Offset<flatbuffers::Vector<const wfb::ActorRef*>> PlaceCollector::write_actors(
    flatbuffers::FlatBufferBuilder& builder) {
    std::ranges::sort(actors_, {}, &WorldActor::ref);
    std::vector<wfb::ActorRef> fb_actors;
    fb_actors.reserve(actors_.size());
    for (const auto& a : actors_) {
        fb_actors.emplace_back(a.ref, a.base, a.cell, to_fb(a.position), to_fb(a.rotation),
                               a.flags);
    }
    shared_.stats().actors += fb_actors.size();
    return builder.CreateVectorOfStructs(fb_actors);
}

} // namespace bethconv::pack::detail
