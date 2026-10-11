// SPDX-License-Identifier: GPL-3.0-or-later
//
// Grass on exterior terrain from world.fb's `Grass` records (GRAS) and the
// grass lists of land textures (LTEX GNAM), as the game grows it: each grass
// on a grid over the cell as fine as its size (the bound radius from its
// OBND) but at least iMinGrassSize (20 units), taking a point where the land
// texture showing there lists it, with its density as the chance,
// within its slope and water distance limits, moved by its position range,
// scaled by its height range, turned at random and, with "fit to slope",
// leaned with the ground. The texture at a point is picked by the layers'
// blended opacity there, as the terrain shader blends them.
//
// One MultiMeshInstance3D per grass type and cell (a second one for the
// blades fully under water, drawn after the water so it hides them), faded out from 100 m
// (fGrassStartFadeDistance, 7000 units). Not done: wind (wave period),
// colour range, vertex lighting.
#pragma once

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace bethconv::pack::wfb {
struct Terrain;
struct Grass;
} // namespace bethconv::pack::wfb

namespace skydot {

/// A grass model ready to instance: its mesh with converted materials and
/// the transform of that mesh inside the model.
struct GrassModel {
    godot::Ref<godot::Mesh> mesh;
    /// The same with materials drawn after the water, for instances that lie
    /// under its surface.
    godot::Ref<godot::Mesh> submerged_mesh;
    godot::Transform3D local;
};

struct GrassInputs {
    const bethconv::pack::wfb::Terrain* terrain = nullptr;
    std::int32_t grid_x = 0;
    std::int32_t grid_y = 0;
    /// Game units; no water if NaN.
    float water_height = 0.0F;
    bool has_water = false;
    /// The Grass records growing on a land texture (0: the default one).
    std::function<std::vector<const bethconv::pack::wfb::Grass*>(std::uint32_t ltex)> grasses_of;
    /// Whether a land texture is snow (INAM bit 0); may be empty.
    std::function<bool(std::uint32_t ltex)> is_snow;
    /// The model of a grass, or a null mesh if it cannot be loaded.
    std::function<GrassModel(const bethconv::pack::wfb::Grass& grass)> model_of;
};

/// A node in the terrain's frame (the cell's south-west corner) holding the
/// cell's grass, or null if none grows there. Counts instances in `placed`.
godot::Node3D* build_grass(const GrassInputs& in, std::int64_t& placed);

} // namespace skydot
