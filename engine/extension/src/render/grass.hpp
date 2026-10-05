// SPDX-License-Identifier: GPL-3.0-or-later
//
// Grass on exterior terrain from world.fb's `Grass` records (GRAS) and the
// grass lists of land textures (LTEX GNAM), as the game grows it: points on
// a grid over the cell (iMinGrassSize, 20 units), each grass of the land
// texture showing there taking a point with its density as the chance,
// within its slope and water distance limits, moved by its position range,
// scaled by its height range, turned at random and, with "fit to slope",
// leaned with the ground. The texture at a point is picked by the layers'
// blended opacity there, as the terrain shader blends them.
//
// One MultiMeshInstance3D per grass type and cell, faded out from 100 m
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
    /// The model of a grass, or a null mesh if it cannot be loaded.
    std::function<GrassModel(const bethconv::pack::wfb::Grass& grass)> model_of;
};

/// A node in the terrain's frame (the cell's south-west corner) holding the
/// cell's grass, or null if none grows there. Counts instances in `placed`.
godot::Node3D* build_grass(const GrassInputs& in, std::int64_t& placed);

} // namespace skydot
