// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/navmesh.hpp"

#include "world/world.hpp"

#include "world_generated.h"

#include <godot_cpp/classes/navigation_link3d.hpp>
#include <godot_cpp/classes/navigation_region3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <optional>

namespace wfb = bethconv::pack::wfb;

namespace skydot {
namespace {

constexpr std::uint16_t k_water = 0x0200;

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
    return SkydotWorld::skyrim_position(godot::Vector3(v->x(), v->y(), v->z()));
}

std::array<std::uint16_t, 3> corners(const wfb::NavTriangle& t) { return {t.v0(), t.v1(), t.v2()}; }

/// The map's cell size and height. A navigation mesh must use the same.
float setting(const char* name, float fallback) {
    const auto value = godot::ProjectSettings::get_singleton()->get_setting(name, fallback);
    return static_cast<float>(static_cast<double>(value));
}

godot::String hex_id(std::uint32_t id) {
    return godot::String("0x") + godot::String::num_uint64(id, 16, true).lpad(8, "0");
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
            if ((t->flags() & (1U << k)) == 0 || edges[static_cast<std::size_t>(k)] < 0 ||
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
            water += (t->flags() & k_water) != 0 ? 1 : 0;
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

} // namespace skydot
