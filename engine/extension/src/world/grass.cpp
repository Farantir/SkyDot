// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/grass.hpp"

#include "world/terrain.hpp"
#include "world/world.hpp"

#include "skydot_formats/flags.hpp"
#include "world_generated.h"

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace wfb = bethconv::pack::wfb;

using godot::Basis;
using godot::Transform3D;
using godot::Vector3;

namespace skydot {

namespace {

constexpr float k_cell_units = 4096.0F;
constexpr float k_spacing = k_cell_units / 32.0F; // between height samples
constexpr float k_grass_grid = 20.0F;             // iMinGrassSize
constexpr float k_fade_start = 7000.0F;           // fGrassStartFadeDistance
constexpr float k_fade_range = 1000.0F;

/// A deterministic random number in [0, 1) per cell, point and purpose.
float random01(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    std::uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du ^
                      (d + 0x27D4EB2Fu) * 0x94D049BBu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return static_cast<float>(h >> 8) / 16777216.0F;
}

/// The texture layers of one quadrant: the base and, mixed over it in order,
/// the additional ones with their opacity per vertex (17 x 17).
struct Quadrant {
    std::uint32_t base = 0;
    std::vector<std::pair<std::uint32_t, std::vector<float>>> layers;
};

std::array<Quadrant, 4> quadrants(const wfb::Terrain& terrain) {
    std::array<Quadrant, 4> out;
    constexpr int count = TerrainBuilder::k_quadrant_grid * TerrainBuilder::k_quadrant_grid;
    if (const auto* layers = terrain.layers()) {
        std::array<bool, 4> has_base{};
        for (const auto* layer : *layers) {
            const int q = layer->quadrant();
            if (q < 0 || q > 3) {
                continue;
            }
            auto& quadrant = out[static_cast<std::size_t>(q)];
            if (layer->layer() < 0) {
                if (!has_base[static_cast<std::size_t>(q)]) {
                    quadrant.base = layer->texture();
                    has_base[static_cast<std::size_t>(q)] = true;
                }
                continue;
            }
            if (quadrant.layers.size() + 1 >= TerrainBuilder::k_max_layers) {
                continue;
            }
            std::vector<float> opacity(count, 0.0F);
            const auto* points = layer->points();
            const auto* values = layer->opacity();
            if (points != nullptr && values != nullptr) {
                const auto n = std::min(points->size(), values->size());
                for (flatbuffers::uoffset_t p = 0; p < n; ++p) {
                    if (points->Get(p) < count) {
                        opacity[points->Get(p)] = static_cast<float>(values->Get(p)) / 255.0F;
                    }
                }
            }
            quadrant.layers.emplace_back(layer->texture(), std::move(opacity));
        }
    }
    return out;
}

/// Whether a grass may grow `above` units above the water (negative below).
bool water_allows(std::uint32_t type, float units, bool has_water, float above) {
    if (!has_water) {
        return type == 0 || type == 4 || type == 6; // no water near: "at least" holds
    }
    switch (type) {
    case 0: return above >= units;                         // above, at least
    case 1: return above >= 0.0F && above <= units;        // above, at most
    case 2: return -above >= units;                        // below, at least
    case 3: return above <= 0.0F && -above <= units;       // below, at most
    case 4: return std::abs(above) >= units;               // either, at least
    case 5: return std::abs(above) <= units;               // either, at most
    case 6: return above <= units;                         // either, at most above
    case 7: return -above <= units;                        // either, at most below
    default: return true;
    }
}

} // namespace

godot::Node3D* build_grass(const GrassInputs& in, std::int64_t& placed) {
    if (in.terrain == nullptr || !in.grasses_of || !in.model_of) {
        return nullptr;
    }
    const std::vector<float> heights = TerrainBuilder::heights(*in.terrain);
    constexpr int grid = TerrainBuilder::k_grid;
    if (heights.size() != static_cast<std::size_t>(grid * grid)) {
        return nullptr;
    }
    const auto height_at = [&](float x, float y) { // cell units
        const float fx = std::clamp(x / k_spacing, 0.0F, 31.999F);
        const float fy = std::clamp(y / k_spacing, 0.0F, 31.999F);
        const int ix = static_cast<int>(fx);
        const int iy = static_cast<int>(fy);
        const float tx = fx - static_cast<float>(ix);
        const float ty = fy - static_cast<float>(iy);
        const auto h = [&](int gx, int gy) { return heights[static_cast<std::size_t>(gy * grid + gx)]; };
        return (h(ix, iy) * (1 - tx) + h(ix + 1, iy) * tx) * (1 - ty) +
               (h(ix, iy + 1) * (1 - tx) + h(ix + 1, iy + 1) * tx) * ty;
    };
    const std::array<Quadrant, 4> layers = quadrants(*in.terrain);
    // The texture showing at a point: each layer's share of the blend.
    const auto texture_at = [&](float x, float y, float pick) {
        const int q = (x >= k_cell_units / 2 ? 1 : 0) + (y >= k_cell_units / 2 ? 2 : 0);
        const Quadrant& quadrant = layers[static_cast<std::size_t>(q)];
        const float lx = std::clamp((x - static_cast<float>(q & 1) * k_cell_units / 2) / k_spacing, 0.0F, 15.999F);
        const float ly = std::clamp((y - static_cast<float>(q >> 1) * k_cell_units / 2) / k_spacing, 0.0F, 15.999F);
        const int ix = static_cast<int>(lx);
        const int iy = static_cast<int>(ly);
        const float tx = lx - static_cast<float>(ix);
        const float ty = ly - static_cast<float>(iy);
        constexpr int side = TerrainBuilder::k_quadrant_grid;
        std::vector<float> share(quadrant.layers.size() + 1, 0.0F);
        float rest = 1.0F; // of what lies under the layers above
        for (std::size_t i = quadrant.layers.size(); i-- > 0;) {
            const auto& o = quadrant.layers[i].second;
            const auto at = [&](int gx, int gy) { return o[static_cast<std::size_t>(gy * side + gx)]; };
            const float w = (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) +
                            (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
            share[i + 1] = rest * w;
            rest *= 1.0F - w;
        }
        share[0] = rest;
        for (std::size_t i = 0; i < share.size(); ++i) {
            pick -= share[i];
            if (pick < 0.0F) {
                return i == 0 ? quadrant.base : quadrant.layers[i - 1].first;
            }
        }
        return quadrant.base;
    };

    const auto scale = static_cast<float>(SkydotWorld::UNIT_SCALE);
    const auto cx = static_cast<std::uint32_t>(in.grid_x);
    const auto cy = static_cast<std::uint32_t>(in.grid_y);
    std::unordered_map<const wfb::Grass*, std::vector<Transform3D>> instances;
    std::unordered_map<std::uint32_t, std::vector<const wfb::Grass*>> grasses;
    const int points = static_cast<int>(k_cell_units / k_grass_grid);
    for (int py = 0; py < points; ++py) {
        for (int px = 0; px < points; ++px) {
            const auto ux = static_cast<std::uint32_t>(px);
            const auto uy = static_cast<std::uint32_t>(py);
            const float gx = (static_cast<float>(px) + 0.5F) * k_grass_grid;
            const float gy = (static_cast<float>(py) + 0.5F) * k_grass_grid;
            const std::uint32_t ltex = texture_at(gx, gy, random01(cx, cy, ux * 4096 + uy, 1));
            auto found = grasses.find(ltex);
            if (found == grasses.end()) {
                found = grasses.emplace(ltex, in.grasses_of(ltex)).first;
            }
            for (std::size_t g = 0; g < found->second.size(); ++g) {
                const wfb::Grass* grass = found->second[g];
                const auto salt = static_cast<std::uint32_t>(g) * 16 + 2;
                if (random01(cx, cy, ux * 4096 + uy, salt) * 100.0F >= static_cast<float>(grass->density())) {
                    continue;
                }
                const float range = grass->position_range();
                const float x = std::clamp(gx + (random01(cx, cy, ux * 4096 + uy, salt + 1) * 2 - 1) * range, 0.0F,
                                           k_cell_units);
                const float y = std::clamp(gy + (random01(cx, cy, ux * 4096 + uy, salt + 2) * 2 - 1) * range, 0.0F,
                                           k_cell_units);
                const float h = height_at(x, y);
                const float dx = (height_at(x + 16, y) - height_at(x - 16, y)) / 32.0F;
                const float dy = (height_at(x, y + 16) - height_at(x, y - 16)) / 32.0F;
                const Vector3 normal = Vector3(-dx, 1.0F, dy).normalized(); // Godot axes
                const float slope = std::acos(std::clamp(normal.y, -1.0F, 1.0F)) * 180.0F / std::numbers::pi_v<float>;
                if (slope < static_cast<float>(grass->min_slope()) || slope > static_cast<float>(grass->max_slope())) {
                    continue;
                }
                if (!water_allows(grass->water_type(), static_cast<float>(grass->units_from_water()), in.has_water,
                                  h - in.water_height)) {
                    continue;
                }
                const float size = std::max(
                    1.0F + (random01(cx, cy, ux * 4096 + uy, salt + 3) * 2 - 1) * grass->height_range(), 0.1F);
                const bool uniform =
                    formats::has_flag(grass->flags(), wfb::GrassFlags::uniform_scaling);
                const float yaw = random01(cx, cy, ux * 4096 + uy, salt + 4) * 2 * std::numbers::pi_v<float>;
                Basis basis = Basis(Vector3(0, 1, 0), yaw).scaled(uniform ? Vector3(size, size, size)
                                                                          : Vector3(1, size, 1));
                if (formats::has_flag(grass->flags(), wfb::GrassFlags::fit_to_slope)) {
                    // Lean with the ground: turn up onto the normal.
                    const Vector3 axis = Vector3(0, 1, 0).cross(normal);
                    if (axis.length() > 0.0001F) {
                        basis = Basis(axis.normalized(), std::acos(std::clamp(normal.y, -1.0F, 1.0F))) * basis;
                    }
                }
                instances[grass].push_back(Transform3D(basis, Vector3(x, h, -y) * scale));
            }
        }
    }
    if (instances.empty()) {
        return nullptr;
    }

    auto* root = memnew(godot::Node3D);
    root->set_name("Grass");
    for (const auto& [grass, transforms] : instances) {
        const GrassModel model = in.model_of(*grass);
        if (model.mesh.is_null()) {
            continue;
        }
        godot::Ref<godot::MultiMesh> multimesh;
        multimesh.instantiate();
        multimesh->set_transform_format(godot::MultiMesh::TRANSFORM_3D);
        multimesh->set_mesh(model.mesh);
        multimesh->set_instance_count(static_cast<int32_t>(transforms.size()));
        for (std::size_t i = 0; i < transforms.size(); ++i) {
            multimesh->set_instance_transform(static_cast<int32_t>(i), transforms[i] * model.local);
        }
        auto* node = memnew(godot::MultiMeshInstance3D);
        node->set_name(godot::String::utf8(grass->editor_id() != nullptr ? grass->editor_id()->c_str() : "grass"));
        node->set_multimesh(multimesh);
        node->set_cast_shadows_setting(godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
        // Per cell: the fade starts where the cell's far edge is that far.
        node->set_visibility_range_end((k_fade_start + k_cell_units * 0.7F) * scale);
        node->set_visibility_range_end_margin(k_fade_range * scale);
        node->set_visibility_range_fade_mode(godot::GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
        root->add_child(node);
        placed += static_cast<std::int64_t>(transforms.size());
    }
    if (root->get_child_count() == 0) {
        memdelete(root);
        return nullptr;
    }
    return root;
}

} // namespace skydot
