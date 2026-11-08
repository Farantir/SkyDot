// SPDX-License-Identifier: GPL-3.0-or-later
#include "nav/navmesh.hpp"

#include "data/coordinates.hpp"
#include "data/text.hpp"

#include "skydot_formats/flags.hpp"
#include "skydot_formats/units.hpp"
#include "world_generated.h"

#include <godot_cpp/classes/navigation_link3d.hpp>
#include <godot_cpp/classes/navigation_region3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace wfb = bethconv::pack::wfb;

namespace skydot {
namespace {

/// The flag of each edge that says it leads into another navmesh.
constexpr std::array<wfb::NavTriangleFlags, 3> k_edge_link{wfb::NavTriangleFlags::edge0_link,
                                                           wfb::NavTriangleFlags::edge1_link,
                                                           wfb::NavTriangleFlags::edge2_link};

using Vertices = flatbuffers::Vector<const wfb::Vec3f*>;
using Triangles = flatbuffers::Vector<const wfb::NavTriangle*>;

/// A navigation mesh's vertex and triangle lists, when it has both and at
/// least one triangle. Read once: the accessors read the buffer on every call,
/// so a check and a later use would be two reads, and the compiler cannot
/// tell that the second finds what the first did.
struct Lists {
    const Vertices& vertices;
    const Triangles& triangles;
};

std::optional<Lists> lists(const wfb::NavMesh& nav) {
    const auto* vertices = nav.vertices();
    const auto* triangles = nav.triangles();
    if (vertices == nullptr || triangles == nullptr || triangles->size() == 0) {
        return std::nullopt;
    }
    return Lists{*vertices, *triangles};
}

godot::Vector3 vertex(const Vertices& vertices, std::uint32_t i) {
    const auto* v = vertices.Get(i);
    return skyrim_position(godot::Vector3(v->x(), v->y(), v->z()));
}

std::array<std::uint16_t, 3> corners(const wfb::NavTriangle& t) { return {t.v0(), t.v1(), t.v2()}; }

/// The map's cell size and height. A navigation mesh must use the same.
float setting(const char* name, float fallback) {
    const auto value = godot::ProjectSettings::get_singleton()->get_setting(name, fallback);
    return static_cast<float>(static_cast<double>(value));
}

} // namespace

godot::Ref<godot::NavigationMesh> navigation_mesh(const wfb::NavMesh& nav) {
    godot::Ref<godot::NavigationMesh> mesh;
    mesh.instantiate();
    mesh->set_cell_size(setting("navigation/3d/default_cell_size", 0.25F));
    mesh->set_cell_height(setting("navigation/3d/default_cell_height", 0.25F));
    const auto in = lists(nav);
    if (!in) {
        return mesh;
    }
    godot::PackedVector3Array vertices;
    vertices.resize(in->vertices.size());
    for (flatbuffers::uoffset_t i = 0; i < in->vertices.size(); ++i) {
        vertices.set(i, vertex(in->vertices, i));
    }
    mesh->set_vertices(vertices);
    for (const auto* t : in->triangles) {
        godot::PackedInt32Array polygon;
        for (const auto v : corners(*t)) {
            polygon.push_back(v);
        }
        mesh->add_polygon(polygon);
    }
    return mesh;
}

godot::Vector3 triangle_centre(const wfb::NavMesh& nav, std::int64_t index) {
    const auto in = lists(nav);
    if (!in || index < 0 || index >= static_cast<std::int64_t>(in->triangles.size())) {
        return {};
    }
    const auto c = corners(*in->triangles.Get(static_cast<flatbuffers::uoffset_t>(index)));
    return (vertex(in->vertices, c[0]) + vertex(in->vertices, c[1]) + vertex(in->vertices, c[2])) / 3.0F;
}

godot::Vector3 edge_middle(const wfb::NavMesh& nav, std::int64_t index, int edge) {
    const auto in = lists(nav);
    if (!in || index < 0 || index >= static_cast<std::int64_t>(in->triangles.size())) {
        return {};
    }
    const auto c = corners(*in->triangles.Get(static_cast<flatbuffers::uoffset_t>(index)));
    const auto k = static_cast<std::size_t>(edge % 3);
    return (vertex(in->vertices, c[k]) + vertex(in->vertices, c[(k + 1) % 3])) / 2.0F;
}

namespace {

/// Each link of `nav`: the edge it leaves from and the triangle it reaches.
template <typename Visit>
void for_each_link(const wfb::NavMesh& nav, const NavIndex& index, Visit&& visit) {
    const auto in = lists(nav);
    const auto* links = nav.links();
    if (!in || links == nullptr) {
        return;
    }
    for (flatbuffers::uoffset_t i = 0; i < in->triangles.size(); ++i) {
        const auto* t = in->triangles.Get(i);
        const std::array<std::int16_t, 3> edges{t->e0(), t->e1(), t->e2()};
        for (int k = 0; k < 3; ++k) {
            if (!formats::has_flag(t->flags(), k_edge_link[static_cast<std::size_t>(k)]) ||
                edges[static_cast<std::size_t>(k)] < 0 ||
                edges[static_cast<std::size_t>(k)] >= static_cast<std::int32_t>(links->size())) {
                continue;
            }
            const auto* link = links->Get(static_cast<flatbuffers::uoffset_t>(edges[static_cast<std::size_t>(k)]));
            const auto target = index.find(link->navmesh());
            const wfb::NavMesh* other = target != index.end() ? target->second.first : nullptr;
            const wfb::Cell* other_cell = target != index.end() ? target->second.second : nullptr;
            visit(*link, edge_middle(nav, i, k),
                  other != nullptr ? triangle_centre(*other, link->triangle()) : godot::Vector3(),
                  other, other_cell);
        }
    }
}

} // namespace

godot::Node3D* build_navmeshes(const wfb::Cell& cell, const NavIndex& index) {
    const auto* navmeshes = cell.navmeshes();
    if (navmeshes == nullptr || navmeshes->size() == 0) {
        return nullptr;
    }
    auto* root = memnew(godot::Node3D);
    root->set_name("Navmesh");
    for (const auto* nav : *navmeshes) {
        if (!lists(*nav)) {
            continue;
        }
        auto* region = memnew(godot::NavigationRegion3D);
        region->set_name(hex_id(nav->id()));
        region->set_navigation_mesh(navigation_mesh(*nav));
        region->set_meta("skydot_navmesh", static_cast<std::int64_t>(nav->id()));
        root->add_child(region);
        for_each_link(*nav, index,
                      [&](const wfb::NavLink& link, const godot::Vector3& from,
                          const godot::Vector3& to, const wfb::NavMesh* other, const wfb::Cell*) {
                          if (link.type() == 0 || other == nullptr) {
                              return;
                          }
                          auto* ledge = memnew(godot::NavigationLink3D);
                          ledge->set_bidirectional(false);
                          ledge->set_start_position(from);
                          ledge->set_end_position(to);
                          ledge->set_meta("skydot_link_type", static_cast<std::int64_t>(link.type()));
                          region->add_child(ledge);
                      });
    }
    return root;
}

godot::Dictionary navmesh_info(const wfb::NavMesh& nav, const wfb::Cell& cell,
                               const NavIndex& index) {
    godot::Dictionary out;
    out["id"] = static_cast<std::int64_t>(nav.id());
    out["cell"] = static_cast<std::int64_t>(cell.id());
    out["vertices"] = static_cast<std::int64_t>(nav.vertices() != nullptr ? nav.vertices()->size() : 0);
    std::int64_t triangles = 0;
    std::int64_t water = 0;
    if (nav.triangles() != nullptr) {
        for (const auto* t : *nav.triangles()) {
            ++triangles;
            water += formats::has_flag(t->flags(), wfb::NavTriangleFlags::water) ? 1 : 0;
        }
    }
    out["triangles"] = triangles;
    out["water"] = water;
    godot::Array links;
    for_each_link(nav, index,
                  [&](const wfb::NavLink& link, const godot::Vector3& from, const godot::Vector3& to,
                      const wfb::NavMesh* other, const wfb::Cell* other_cell) {
                      godot::Dictionary entry;
                      entry["type"] = static_cast<std::int64_t>(link.type());
                      entry["navmesh"] = static_cast<std::int64_t>(link.navmesh());
                      entry["cell"] = static_cast<std::int64_t>(other_cell != nullptr ? other_cell->id() : 0);
                      entry["triangle"] = static_cast<std::int64_t>(link.triangle());
                      entry["from"] = from;
                      entry["to"] = other != nullptr ? godot::Variant(to) : godot::Variant();
                      links.push_back(entry);
                  });
    out["links"] = links;
    godot::Array doors;
    if (nav.doors() != nullptr) {
        for (const auto* d : *nav.doors()) {
            godot::Dictionary entry;
            entry["door"] = static_cast<std::int64_t>(d->door());
            entry["triangle"] = static_cast<std::int64_t>(d->triangle());
            entry["position"] = triangle_centre(nav, d->triangle());
            doors.push_back(entry);
        }
    }
    out["doors"] = doors;
    return out;
}

namespace {

constexpr auto k_nav_cell_units = static_cast<float>(formats::k_cell_units);

using godot::real_t;
using godot::Vector3;

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

std::optional<Vector3> nearest_nav_point(const WorldData& data, std::int64_t space, const Vector3& position,
                                         double reach) {
    std::vector<const wfb::Cell*> cells;
    const auto* s = data.cell_ptr(space);
    if (s != nullptr &&
        (formats::has_flag(s->flags(), wfb::CellFlags::interior) || s->world() == 0)) {
        cells.push_back(s);
    } else {
        // The cell the point is in, and its neighbours only as far as `reach`.
        const auto r = static_cast<float>(reach);
        const auto x0 = static_cast<std::int32_t>(std::floor((position.x - r) / k_nav_cell_units));
        const auto x1 = static_cast<std::int32_t>(std::floor((position.x + r) / k_nav_cell_units));
        const auto y0 = static_cast<std::int32_t>(std::floor((position.y - r) / k_nav_cell_units));
        const auto y1 = static_cast<std::int32_t>(std::floor((position.y + r) / k_nav_cell_units));
        for (auto y = y0; y <= y1; ++y) {
            for (auto x = x0; x <= x1; ++x) {
                if (const auto* c = data.exterior_ptr(static_cast<std::uint32_t>(space), x, y)) {
                    cells.push_back(c);
                }
            }
        }
    }
    std::optional<Vector3> best;
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

} // namespace skydot
