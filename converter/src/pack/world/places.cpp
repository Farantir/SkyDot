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
#include <memory>
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
std::optional<Lighting> resolve_lighting(
    const CellEntry& entry, const std::map<std::uint32_t, LightingTemplateEntry>& templates) {
    const auto it = templates.find(entry.cell.lighting_template);
    if (!has_flag(entry.cell.flags, wfb::CellFlags::interior) || it == templates.end()) {
        return entry.xcll;
    }
    const auto& t = it->second.lighting;
    if (!entry.xcll) {
        return t;
    }
    Lighting out = *entry.xcll;
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

wfb::CellLighting to_fb(const Lighting& l) {
    return wfb::CellLighting(l.ambient, l.directional, l.fog_near_color, l.fog_far_color,
                             l.fog_near, l.fog_far, l.fog_power, l.fog_max,
                             l.directional_rotation_xy, l.directional_rotation_z,
                             l.directional_fade, l.light_fade_begin, l.light_fade_end, l.inherit);
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
        on_lighting_template(merged, data, form_ctx);
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
    auto& entry = cells_[merged.form.value];
    entry.has_record = true;
    entry.xcll.reset();
    if (!cell->lighting.empty()) {
        if (auto decoded = record::decode_cell_lighting(cell->lighting)) {
            entry.xcll = *decoded;
        } else {
            ++shared_.stats().parse_errors; // an XCLL of no known size
        }
    }
    auto& out = entry.cell;
    out.id = merged.form.value;
    out.editor_id = cell->editor_id;
    out.name = cell->name.text;
    out.world = cell->is_interior() ? 0 : merged.parent.value;
    out.flags = static_cast<wfb::CellFlags>(cell->flags);
    if (cell->grid) {
        out.has_grid = true;
        out.grid_x = cell->grid->x;
        out.grid_y = cell->grid->y;
    }
    out.water_height = cell->water_height;
    out.lighting_template = shared_.global(merged, cell->lighting_template, failed);
    out.image_space = shared_.global(merged, cell->image_space, failed);
    out.sky_region = shared_.global(merged, cell->sky_region, failed);
    out.location = shared_.global(merged, cell->location, failed);
    out.persistent = record::has_flag(merged.flags, record::RecordFlag::persistent);
    out.water = shared_.global(merged, cell->water, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
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
    auto& cell = cells_[merged.parent.value].cell;
    const std::uint32_t id = merged.form.value;
    wfb::RefFlags flags{};
    std::uint32_t enable_parent = 0;
    if (ref->initially_disabled) {
        flags |= wfb::RefFlags::initially_disabled;
    }
    if (ref->persistent) {
        flags |= wfb::RefFlags::persistent;
    }
    if (ref->enable_parent) {
        enable_parent = shared_.global(merged, ref->enable_parent->parent, failed);
        if (ref->enable_parent->set_enable_state_opposite()) {
            flags |= wfb::RefFlags::enable_opposite;
        }
    }
    if ((ref->activate_parent_flags & 0x01u) != 0) {
        flags |= wfb::RefFlags::parent_activate_only;
    }
    if (record::has_flag(merged.flags, record::RecordFlag::visible_when_distant)) {
        flags |= wfb::RefFlags::visible_when_distant;
    }
    cell.refs.emplace_back(id, shared_.global(merged, ref->base, failed), to_fb(ref->position),
                           to_fb(ref->rotation), ref->scale, flags, enable_parent);
    if (!ref->scripts.empty()) {
        auto& scripts = cell.scripts.emplace_back(std::make_unique<wfb::RefScriptsT>());
        scripts->ref = id;
        scripts->scripts = shared_.global_scripts(merged, ref->scripts, failed);
    }
    if (ref->has_radius || ref->light_data.size() >= record::LightOverride::k_size) {
        bool has_light_data = false;
        float fov = 0.0F;
        float fade = 0.0F;
        float end_distance_cap = 0.0F;
        float shadow_depth_bias = 0.0F;
        if (ref->light_data.size() >= record::LightOverride::k_size) {
            if (const auto light = record::decode_light_override(ref->light_data)) {
                has_light_data = true;
                fov = light->fov;
                fade = light->fade;
                end_distance_cap = light->end_distance_cap;
                shadow_depth_bias = light->shadow_depth_bias;
            }
        }
        cell.light_overrides.emplace_back(id, ref->has_radius, ref->radius, has_light_data, fov,
                                          fade, end_distance_cap, shadow_depth_bias);
    }
    if (!ref->emittance.is_null()) {
        cell.light_emitters.emplace_back(id, shared_.global(merged, ref->emittance, failed));
    }
    if (ref->lock) {
        cell.locks.emplace_back(id, ref->lock->level, ref->lock->flags,
                                shared_.global(merged, ref->lock->key, failed));
    }
    for (const auto& link : ref->linked_references) {
        cell.links.emplace_back(id, shared_.global(merged, link.keyword, failed),
                                shared_.global(merged, link.target, failed));
    }
    for (const auto& parent : ref->activate_parents) {
        cell.activate_parents.emplace_back(id, shared_.global(merged, parent.ref, failed),
                                           parent.delay);
    }
    if (ref->primitive) {
        cell.primitives.emplace_back(id, to_fb(ref->primitive->bounds), ref->primitive->type);
    }
    if (ref->teleport) {
        cell.doors.emplace_back(id,
                                shared_.global(merged, ref->teleport->destination_door, failed),
                                to_fb(ref->teleport->position), to_fb(ref->teleport->rotation));
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
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
    auto heights = record::decode_height_map(land->heights);
    if (!heights && !land->heights.empty()) {
        ++shared_.stats().parse_errors; // a VHGT too short for the grid; no VHGT at all is legitimate
    }
    if (!heights || merged.parent.is_null()) {
        return;
    }
    bool failed = false;
    auto terrain = std::make_unique<wfb::TerrainT>();
    terrain->height_offset = heights->offset;
    terrain->height_deltas = std::move(heights->deltas);
    if (land->vertex_colours.size() == record::Landscape::k_vnml_size) {
        terrain->colours.reserve(land->vertex_colours.size());
        for (const std::byte b : land->vertex_colours) {
            terrain->colours.push_back(static_cast<std::uint8_t>(b));
        }
    }
    for (const auto& base : land->base_layers) {
        auto& layer = terrain->layers.emplace_back(std::make_unique<wfb::TerrainLayerT>());
        layer->texture = shared_.global(merged, base.texture, failed);
        layer->quadrant = base.quadrant;
        layer->layer = -1;
    }
    for (const auto& extra : land->additional_layers) {
        auto layer = std::make_unique<wfb::TerrainLayerT>();
        layer->texture = shared_.global(merged, extra.texture, failed);
        layer->quadrant = extra.quadrant;
        layer->layer = extra.layer;
        // VTXT, 8 bytes a point: vertex index, an unknown word, opacity.
        if (const auto points = record::decode_alpha_points(extra.alpha_map)) {
            for (const auto& point : *points) {
                const float opacity = std::clamp(point.opacity, 0.0F, 1.0F);
                layer->points.push_back(point.position);
                layer->opacity.push_back(static_cast<std::uint8_t>(opacity * 255.0F + 0.5F));
            }
        }
        terrain->layers.push_back(std::move(layer));
    }
    std::ranges::stable_sort(terrain->layers, [](const auto& a, const auto& b) {
        return std::pair{a->quadrant, a->layer} < std::pair{b->quadrant, b->layer};
    });
    if (failed) {
        ++shared_.stats().unresolved;
    }
    cells_[merged.parent.value].cell.terrain = std::move(terrain);
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
    const std::uint32_t id = merged.form.value;
    wfb::RefFlags flags{};
    if (actor->initially_disabled) {
        flags |= wfb::RefFlags::initially_disabled;
    }
    if (actor->persistent) {
        flags |= wfb::RefFlags::persistent;
    }
    // ACHR record flag 0x200 is "Starts Dead" (xEdit; the same bit is ESL on a TES4).
    constexpr std::uint32_t k_starts_dead = 0x200;
    if ((merged.flags & k_starts_dead) != 0) {
        flags |= wfb::RefFlags::starts_dead;
    }
    actors_.emplace_back(id, shared_.global(merged, actor->base, failed), merged.parent.value,
                         to_fb(actor->position), to_fb(actor->rotation), flags);
    // Packages walk to linked references (beds, work markers, patrols).
    for (const auto& link : actor->linked_references) {
        cells_[merged.parent.value].cell.links.emplace_back(
            id, shared_.global(merged, link.keyword, failed),
            shared_.global(merged, link.target, failed));
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
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
    auto out = std::make_unique<wfb::NavMeshT>();
    out->id = merged.form.value;
    out->vertices.reserve(geometry->vertices.size());
    for (const auto& v : geometry->vertices) {
        out->vertices.push_back(to_fb(v));
    }
    out->triangles.reserve(geometry->triangles.size());
    for (const auto& t : geometry->triangles) {
        out->triangles.emplace_back(t.vertices[0], t.vertices[1], t.vertices[2], t.edges[0],
                                    t.edges[1], t.edges[2],
                                    static_cast<wfb::NavTriangleFlags>(t.flags), t.cover);
    }
    for (const auto& l : geometry->edge_links) {
        out->links.emplace_back(l.type, shared_.global(merged, l.navmesh, failed), l.triangle);
    }
    for (const auto& d : geometry->doors) {
        out->doors.emplace_back(d.triangle, shared_.global(merged, d.door, failed));
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    cells_[merged.parent.value].cell.navmeshes.push_back(std::move(out));
}

/// LGTM (UESP, LGTM record): DATA as XCLL, in any of its sizes (see
/// decode_xcll); DALC is 32 bytes (the six directional ambient colours, a
/// specular colour and a fresnel power; xEdit, wbAmbientColors) on 92 of the 97
/// in Skyrim.esm and 24 (the colours alone, xEdit marks the rest optional) on
/// the 5 whose DATA is short.
void PlaceCollector::on_lighting_template(const record::MergedRecord& merged,
                                          io::SpanReader& data,
                                          const record::FormContext& form_ctx) {
    auto lgtm = record::parse_lighting_template(data, form_ctx);
    if (!lgtm || !lgtm->data) {
        ++shared_.stats().parse_errors;
        return;
    }
    LightingTemplateEntry out;
    out.lighting = *lgtm->data;
    out.lighting.directional_ambient = lgtm->directional_ambient;
    out.lighting.inherit = 0;
    lighting_templates_[merged.form.value] = std::move(out);
}

void PlaceCollector::finish(wfb::WorldT& world) {
    auto& stats = shared_.stats();
    world.cells.reserve(cells_.size());
    for (auto& [id, entry] : cells_) { // std::map: sorted by id
        auto& cell = entry.cell;
        if (!entry.has_record) {
            // References and navmeshes whose parent is not a cell record.
            stats.orphan_refs += cell.refs.size();
            stats.orphan_navmeshes += cell.navmeshes.size();
            continue;
        }
        std::ranges::sort(cell.refs, {}, &wfb::Ref::id);
        std::ranges::sort(cell.doors, {}, &wfb::DoorLink::ref);
        std::ranges::stable_sort(cell.scripts, {}, [](const auto& s) { return s->ref; });
        std::ranges::stable_sort(cell.locks, {}, &wfb::Lock::ref);
        std::ranges::stable_sort(cell.links, {}, &wfb::LinkedRef::ref);
        std::ranges::stable_sort(cell.activate_parents, {}, &wfb::ActivateParent::ref);
        std::ranges::stable_sort(cell.primitives, {}, &wfb::Primitive::ref);
        std::ranges::stable_sort(cell.light_overrides, {}, &wfb::LightOverride::ref);
        std::ranges::stable_sort(cell.light_emitters, {}, &wfb::LightEmitter::ref);
        std::ranges::sort(cell.navmeshes, {}, [](const auto& n) { return n->id; });

        stats.refs += cell.refs.size();
        stats.doors += cell.doors.size();
        for (const auto& r : cell.scripts) {
            stats.scripts += r->scripts.size();
        }
        stats.locks += cell.locks.size();
        stats.links += cell.links.size();
        stats.activate_parents += cell.activate_parents.size();
        stats.primitives += cell.primitives.size();
        for (const auto& nav : cell.navmeshes) {
            ++stats.navmeshes;
            stats.nav_triangles += nav->triangles.size();
        }
        if (cell.terrain) {
            ++stats.terrains;
            stats.terrain_layers += cell.terrain->layers.size();
        }

        if (const auto resolved = resolve_lighting(entry, lighting_templates_)) {
            cell.has_lighting = true;
            cell.lighting = std::make_unique<wfb::CellLighting>(to_fb(*resolved));
            if (std::ranges::any_of(resolved->directional_ambient,
                                    [](std::uint32_t c) { return c != 0; })) {
                cell.directional_ambient.assign(resolved->directional_ambient.begin(),
                                                resolved->directional_ambient.end());
            }
        }
        ++stats.cells;
        if (has_flag(cell.flags, wfb::CellFlags::interior)) {
            ++stats.interior_cells;
        }
        world.cells.push_back(std::make_unique<wfb::CellT>(std::move(cell)));
    }
    stats.lighting_templates = lighting_templates_.size();
    std::ranges::sort(actors_, {}, &wfb::ActorRef::ref);
    stats.actors += actors_.size();
    world.actors = std::move(actors_);
}

} // namespace bethconv::pack::detail
