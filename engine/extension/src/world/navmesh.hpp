// SPDX-License-Identifier: GPL-3.0-or-later
//
// Navmeshes from world.fb (NAVM) as Godot navigation regions, one per
// navmesh, so Godot's navigation map can path across them. See
// docs/navigation.md.
#pragma once

#include "world/world_data.hpp"

#include <godot_cpp/classes/navigation_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <unordered_map>
#include <utility>

namespace bethconv::pack::wfb {
struct Cell;
struct NavMesh;
} // namespace bethconv::pack::wfb

namespace skydot {

/// The navigation mesh of one navmesh in Godot space. Triangles keep their
/// order: Skyrim's face up, as Godot's do.
godot::Ref<godot::NavigationMesh> navigation_mesh(const bethconv::pack::wfb::NavMesh& nav);

/// Centre of triangle `index` in Godot space; (0, 0, 0) if out of range.
godot::Vector3 triangle_centre(const bethconv::pack::wfb::NavMesh& nav, std::int64_t index);

/// Midpoint of edge `edge` (0-2) of triangle `index` in Godot space.
godot::Vector3 edge_middle(const bethconv::pack::wfb::NavMesh& nav, std::int64_t index, int edge);

/// The cell's navmeshes as NavigationRegion3D nodes under a Node3D named
/// "Navmesh", with a one-way NavigationLink3D for each ledge link; null if
/// the cell has none. Portals across cell borders need no link: Godot joins
/// regions whose free edges lie within the map's edge connection margin.
godot::Node3D* build_navmeshes(const bethconv::pack::wfb::Cell& cell, const NavIndex& index);

/// id, cell, vertices, triangles, water (triangle count), links (Array of
/// type, navmesh, cell, triangle, from, to; positions in Godot space at the
/// edge's middle and the target triangle's centre) and doors (Array of door,
/// triangle, position).
godot::Dictionary navmesh_info(const bethconv::pack::wfb::NavMesh& nav,
                               const bethconv::pack::wfb::Cell& cell, const NavIndex& index);

} // namespace skydot
