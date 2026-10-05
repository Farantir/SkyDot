// SPDX-License-Identifier: GPL-3.0-or-later
//
// Terrain from world.fb's `Terrain` table (formats/pack-format.md): one
// mesh per cell quadrant, since each quadrant has its own texture layers (a
// base plus up to six in vanilla). Layers are blended per vertex like the
// game: each additional layer is mixed over the result by its VTXT opacity.
//
// Normals are computed from the heights, using the neighbouring cells' edge
// rows where they exist so that cells meet without seams.
#pragma once

#include "assets/asset_cache.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bethconv::pack::wfb {
struct Terrain;
} // namespace bethconv::pack::wfb

namespace skydot {

class TerrainBuilder {
public:
    static constexpr int k_grid = 33;
    static constexpr int k_quadrant_grid = 17;
    /// Vanilla quadrants use at most a base and six additional layers.
    static constexpr int k_max_layers = 7;
    static constexpr const char* k_default_texture = "textures/landscape/dirt02.dds";

    /// Textures come from `assets`; without it terrain is untextured.
    explicit TerrainBuilder(std::shared_ptr<AssetCache> assets) : assets_(std::move(assets)) {}

    /// Heights (game units, row-major from the south-west corner) of the
    /// terrain `dx`, `dy` cells away from the one being built, or empty.
    using NeighbourHeights = std::function<std::vector<float>(int dx, int dy)>;
    /// Diffuse and normal virtual paths of an LTEX; empty if unknown.
    using LandTexturePaths = std::function<std::array<std::string, 2>(std::uint32_t ltex)>;

    /// Decoded heights of a `Terrain` (see world.fbs).
    static std::vector<float> heights(const bethconv::pack::wfb::Terrain& terrain);

    /// A node at the cell's south-west corner (Godot space) holding one
    /// MeshInstance3D per quadrant.
    godot::Node3D* build(const bethconv::pack::wfb::Terrain& terrain,
                         const NeighbourHeights& neighbours, const LandTexturePaths& paths);

    /// Create the shader for every layer count now (see SkydotMaterials::warm_up).
    void warm_up();

    /// The code of every shader terrain can use, by name.
    static godot::Dictionary shader_codes();

    /// Texture repeats per cell side.
    void set_tiling(float repeats) { tiling_ = repeats; }
    /// Whether terrain gets a physics body.
    void set_collision(bool enabled) { collision_ = enabled; }

private:
    /// The stub of the variant for `layers` layers (terrain.gdshaderinc).
    static std::string shader_code(int layers);
    godot::Ref<godot::Shader> shader_for(int layers);
    godot::Ref<godot::Texture> texture(const std::string& vpath);
    godot::Ref<godot::ShaderMaterial> material_for(const std::vector<std::array<std::string, 2>>& layers);

    std::shared_ptr<AssetCache> assets_;
    float tiling_{8.0F};
    bool collision_{true};
    std::unordered_map<int, godot::Ref<godot::Shader>> shaders_;
    std::unordered_map<std::string, godot::Ref<godot::Texture>> textures_;
    std::unordered_map<std::string, godot::Ref<godot::ShaderMaterial>> materials_;
    std::vector<godot::Ref<godot::ShaderMaterial>> warm_;
};

} // namespace skydot
