// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/queries.hpp"
#include "data/coordinates.hpp"
#include "data/fb_search.hpp"
#include "nav/navmesh.hpp"
#include "build/refs.hpp"
#include "data/text.hpp"

#include "skydot_formats/flags.hpp"
#include "skydot_formats/units.hpp"
#include "world_generated.h"

#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot::queries {

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

using godot::real_t;

/// The point of triangle (a, b, c) nearest `p` (Ericson, Real-Time Collision
/// Detection, 5.1.5).
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

std::int64_t get_cell_count(const WorldData& data) {
    return data.root() != nullptr && data.root()->cells() != nullptr ? data.root()->cells()->size() : 0;
}

std::int64_t get_base_count(const WorldData& data) {
    return data.root() != nullptr && data.root()->bases() != nullptr ? data.root()->bases()->size() : 0;
}

std::int64_t find_cell(const WorldData& data, const String& editor_id) {
    const auto* cells = data.root() != nullptr ? data.root()->cells() : nullptr;
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

Array list_cells(const WorldData& data, const String& filter, bool interior_only) {
    Array out;
    const auto* cells = data.root() != nullptr ? data.root()->cells() : nullptr;
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

Dictionary get_cell(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* cell = data.cell_ptr(id);
    if (cell == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(cell->id());
    out["editor_id"] = to_godot(cell->editor_id());
    out["interior"] = formats::has_flag(cell->flags(), wfb::CellFlags::interior);
    out["world"] = static_cast<std::int64_t>(cell->world());
    out["grid"] = cell->has_grid() ? godot::Variant(godot::Vector2i(cell->grid_x(), cell->grid_y()))
                                   : godot::Variant();
    out["water_height"] = static_cast<double>(cell->water_height()) * formats::k_metres_per_unit;
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
        lighting["fog_near"] = static_cast<double>(l->fog_near()) * formats::k_metres_per_unit;
        lighting["fog_far"] = static_cast<double>(l->fog_far()) * formats::k_metres_per_unit;
        lighting["fog_power"] = l->fog_power();
        lighting["fog_max"] = l->fog_max();
        lighting["light_fade_begin"] = static_cast<double>(l->light_fade_begin()) * formats::k_metres_per_unit;
        lighting["light_fade_end"] = static_cast<double>(l->light_fade_end()) * formats::k_metres_per_unit;
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

Dictionary get_image_space(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* list = data.root() != nullptr ? data.root()->image_spaces() : nullptr;
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

Array get_refs(const WorldData& data, std::int64_t cell_id) {
    Array out;
    const auto* cell = data.cell_ptr(cell_id);
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
        entry["disabled"] = data.initially_disabled(*ref);
        entry["persistent"] = formats::has_flag(ref->flags(), wfb::RefFlags::persistent);
        entry["enable_parent"] = static_cast<std::int64_t>(ref->enable_parent());
        out.push_back(entry);
    }
    return out;
}

Dictionary get_base(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* base = data.base_ptr(id);
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
        light["radius"] = static_cast<double>(l->radius()) * formats::k_metres_per_unit;
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

godot::PackedInt64Array get_cell_actors(const WorldData& data, std::int64_t cell) {
    godot::PackedInt64Array out;
    if (const auto* actors = data.cell_actors(static_cast<std::uint32_t>(cell))) {
        for (const auto* a : *actors) {
            out.push_back(a->ref());
        }
    }
    return out;
}

godot::Array get_navmeshes(const WorldData& data, std::int64_t cell_id) {
    Array out;
    const auto* cell = data.cell_ptr(cell_id);
    if (cell == nullptr || cell->navmeshes() == nullptr) {
        return out;
    }
    for (const auto* nav : *cell->navmeshes()) {
        out.push_back(navmesh_info(*nav, *cell, data.navmeshes()));
    }
    return out;
}

Dictionary get_navmesh(const WorldData& data, std::int64_t id) {
    const auto& index = data.navmeshes();
    const auto it = index.find(static_cast<std::uint32_t>(id));
    if (it == index.end()) {
        return {};
    }
    return navmesh_info(*it->second.first, *it->second.second, index);
}

Vector3 nearest_nav_point(const WorldData& data, std::int64_t space, const Vector3& position, double reach) {
    std::vector<const wfb::Cell*> cells;
    const auto* s = data.cell_ptr(space);
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
                if (const auto* c = data.exterior_ptr(static_cast<std::uint32_t>(space), x, y)) {
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

Array list_worlds(const WorldData& data) {
    Array out;
    const auto* worlds = data.root() != nullptr ? data.root()->worlds() : nullptr;
    if (worlds == nullptr) {
        return out;
    }
    for (const auto* w : *worlds) {
        Dictionary entry;
        entry["id"] = static_cast<std::int64_t>(w->id());
        entry["editor_id"] = to_godot(w->editor_id());
        entry["parent"] = static_cast<std::int64_t>(w->parent());
        entry["land_world"] = static_cast<std::int64_t>(data.land_world(w->id()));
        entry["default_water_height"] =
            w->has_defaults() ? godot::Variant(static_cast<double>(w->default_water_height()) *
                                               formats::k_metres_per_unit)
                              : godot::Variant();
        entry["bounds"] = godot::Rect2(w->min_x(), w->min_y(), w->max_x() - w->min_x(),
                                       w->max_y() - w->min_y());
        out.push_back(entry);
    }
    return out;
}

std::int64_t find_world(const WorldData& data, const String& editor_id) {
    const auto* worlds = data.root() != nullptr ? data.root()->worlds() : nullptr;
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

std::int64_t get_exterior_cell(const WorldData& data, std::int64_t world, std::int64_t x, std::int64_t y) {
    const auto* cell = data.exterior_ptr(static_cast<std::uint32_t>(world), static_cast<std::int32_t>(x),
                                    static_cast<std::int32_t>(y));
    return cell != nullptr ? cell->id() : 0;
}

} // namespace skydot::queries
