// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/terrain.hpp"
#include "render/materials.hpp"

#include "physics/collision.hpp"

#include "skydot_formats/units.hpp"
#include "world_generated.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <algorithm>
#include <cmath>

using godot::Color;
using godot::Ref;
using godot::String;
using godot::Vector2;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {

namespace {

constexpr float k_spacing = 128.0F; // game units between vertices
constexpr int k_last = TerrainBuilder::k_grid - 1;

String to_godot(const std::string& s) {
    return String::utf8(s.c_str(), static_cast<int>(s.size()));
}

} // namespace

std::vector<float> TerrainBuilder::heights(const wfb::Terrain& terrain) {
    std::vector<float> out(static_cast<std::size_t>(k_grid * k_grid), 0.0F);
    const auto* deltas = terrain.height_deltas();
    if (deltas == nullptr || deltas->size() != out.size()) {
        return out;
    }
    float row = terrain.height_offset();
    for (int y = 0; y < k_grid; ++y) {
        row += static_cast<float>(deltas->Get(static_cast<flatbuffers::uoffset_t>(y * k_grid)));
        float column = row;
        out[static_cast<std::size_t>(y * k_grid)] = column * 8.0F;
        for (int x = 1; x < k_grid; ++x) {
            column += static_cast<float>(
                deltas->Get(static_cast<flatbuffers::uoffset_t>(y * k_grid + x)));
            out[static_cast<std::size_t>(y * k_grid + x)] = column * 8.0F;
        }
    }
    return out;
}

std::string TerrainBuilder::shader_code(int layers) {
    std::string code = R"(shader_type spatial;
render_mode cull_back, diffuse_lambert;

// Game-style layering: each additional layer is mixed over the result by its
// per-vertex opacity (CUSTOM0 holds layers 1-4, CUSTOM1 layers 5-6).
uniform float tiling = 8.0;
varying vec4 weights_a;
varying vec2 weights_b;
)";
    for (int i = 0; i < layers; ++i) {
        const std::string n = std::to_string(i);
        code += "uniform sampler2D albedo_" + n +
                " : filter_linear_mipmap_anisotropic, repeat_enable;\n";
        code += "uniform sampler2D normal_" + n +
                " : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;\n";
    }
    code += R"(
void vertex() {
	weights_a = CUSTOM0;
	weights_b = CUSTOM1.xy;
}

void fragment() {
	vec2 uv = UV * tiling;
	vec3 albedo = texture(albedo_0, uv).rgb;
	vec3 n = texture(normal_0, uv).rgb;
)";
    for (int i = 1; i < layers; ++i) {
        const std::string n = std::to_string(i);
        const std::string w = i <= 4 ? "weights_a[" + std::to_string(i - 1) + "]"
                                     : "weights_b[" + std::to_string(i - 5) + "]";
        code += "\talbedo = mix(albedo, texture(albedo_" + n + ", uv).rgb, " + w + ");\n";
        code += "\tn = mix(n, texture(normal_" + n + ", uv).rgb, " + w + ");\n";
    }
    code += R"(	ALBEDO = albedo * COLOR.rgb;
	// Skyrim normal maps are DirectX-style (green points down).
	NORMAL_MAP = vec3(n.r, 1.0 - n.g, n.b);
	ROUGHNESS = 1.0;
	SPECULAR = 0.2;
}
)";
    return with_game_ambient(with_game_fog(code));
}

godot::Dictionary TerrainBuilder::shader_codes() {
    godot::Dictionary out;
    for (int layers = 1; layers <= k_max_layers; ++layers) {
        out[String("terrain_") + String::num_int64(layers)] = to_godot(shader_code(layers));
    }
    return out;
}

Ref<godot::Shader> TerrainBuilder::shader_for(int layers) {
    if (auto it = shaders_.find(layers); it != shaders_.end()) {
        return it->second;
    }
    Ref<godot::Shader> shader;
    shader.instantiate();
    shader->set_code(to_godot(shader_code(layers)));
    shaders_.emplace(layers, shader);
    return shader;
}

void TerrainBuilder::warm_up() {
    // A shader compiles when a material first uses it.
    for (int layers = 1; layers <= k_max_layers; ++layers) {
        Ref<godot::ShaderMaterial> material;
        material.instantiate();
        material->set_shader(shader_for(layers));
        material->set_shader_parameter("tiling", tiling_);
        warm_.push_back(material);
    }
}

Ref<godot::Texture> TerrainBuilder::texture(const std::string& vpath) {
    if (vpath.empty()) {
        return {};
    }
    if (auto it = textures_.find(vpath); it != textures_.end()) {
        return it->second;
    }
    Ref<godot::Texture> texture = assets_ != nullptr ? assets_->texture(vpath) : Ref<godot::Texture>();
    textures_.emplace(vpath, texture);
    return texture;
}

Ref<godot::ShaderMaterial> TerrainBuilder::material_for(
    const std::vector<std::array<std::string, 2>>& layers) {
    std::string key;
    for (const auto& [diffuse, normal] : layers) {
        key += diffuse + "|" + normal + ";";
    }
    if (auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    Ref<godot::ShaderMaterial> material;
    material.instantiate();
    material->set_shader(shader_for(static_cast<int>(layers.size())));
    material->set_shader_parameter("tiling", tiling_);
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const String n = String::num_int64(static_cast<std::int64_t>(i));
        Ref<godot::Texture> albedo = texture(layers[i][0]);
        if (albedo.is_null()) {
            albedo = texture(k_default_texture);
        }
        material->set_shader_parameter(String("albedo_") + n, albedo);
        if (Ref<godot::Texture> normal = texture(layers[i][1]); normal.is_valid()) {
            material->set_shader_parameter(String("normal_") + n, normal);
        }
    }
    materials_.emplace(key, material);
    return material;
}

godot::Node3D* TerrainBuilder::build(const wfb::Terrain& terrain,
                                     const NeighbourHeights& neighbours,
                                     const LandTexturePaths& paths) {
    const std::vector<float> own = heights(terrain);
    const std::vector<float> west = neighbours(-1, 0);
    const std::vector<float> east = neighbours(1, 0);
    const std::vector<float> south = neighbours(0, -1);
    const std::vector<float> north = neighbours(0, 1);

    // Height at (x, y) with one vertex of margin from the neighbours; a
    // missing neighbour repeats the edge.
    const auto at = [&](int x, int y) -> float {
        const auto get = [](const std::vector<float>& h, int gx, int gy) {
            return h[static_cast<std::size_t>(gy * k_grid + gx)];
        };
        if (x < 0) {
            return west.empty() ? get(own, 0, y) : get(west, k_last - 1, y);
        }
        if (x > k_last) {
            return east.empty() ? get(own, k_last, y) : get(east, 1, y);
        }
        if (y < 0) {
            return south.empty() ? get(own, x, 0) : get(south, x, k_last - 1);
        }
        if (y > k_last) {
            return north.empty() ? get(own, x, k_last) : get(north, x, 1);
        }
        return get(own, x, y);
    };

    const auto* colours = terrain.colours();
    const bool has_colours =
        colours != nullptr && colours->size() == static_cast<flatbuffers::uoffset_t>(k_grid * k_grid * 3);
    const auto scale = static_cast<float>(formats::k_metres_per_unit);

    auto* root = memnew(godot::Node3D);
    root->set_name("Terrain");

    for (int quadrant = 0; quadrant < 4; ++quadrant) {
        // Layers of this quadrant: base first, then additional ones in order.
        std::vector<std::array<std::string, 2>> textures;
        std::vector<const wfb::TerrainLayer*> additional;
        bool has_base = false;
        if (const auto* layers = terrain.layers()) {
            for (const auto* layer : *layers) {
                if (layer->quadrant() != quadrant) {
                    continue;
                }
                if (layer->layer() < 0) {
                    if (!has_base) {
                        textures.insert(textures.begin(), paths(layer->texture()));
                        has_base = true;
                    }
                } else if (additional.size() + 1 < k_max_layers) {
                    additional.push_back(layer);
                    textures.push_back(paths(layer->texture()));
                }
            }
        }
        if (!has_base) {
            textures.insert(textures.begin(), std::array<std::string, 2>{k_default_texture, ""});
        }

        const int ox = (quadrant & 1) * 16;
        const int oy = (quadrant >> 1) * 16;
        const int count = k_quadrant_grid * k_quadrant_grid;
        std::vector<std::array<float, 6>> weights(static_cast<std::size_t>(count),
                                                  std::array<float, 6>{});
        for (std::size_t i = 0; i < additional.size(); ++i) {
            const auto* points = additional[i]->points();
            const auto* opacity = additional[i]->opacity();
            if (points == nullptr || opacity == nullptr) {
                continue;
            }
            const auto n = std::min(points->size(), opacity->size());
            for (flatbuffers::uoffset_t p = 0; p < n; ++p) {
                const auto index = points->Get(p);
                if (index < count) {
                    weights[index][i] = static_cast<float>(opacity->Get(p)) / 255.0F;
                }
            }
        }

        godot::PackedVector3Array vertices;
        godot::PackedVector3Array normals;
        godot::PackedFloat32Array tangents;
        godot::PackedVector2Array uvs;
        godot::PackedColorArray vertex_colours;
        godot::PackedFloat32Array custom0;
        godot::PackedFloat32Array custom1;
        for (int row = 0; row < k_quadrant_grid; ++row) {
            for (int column = 0; column < k_quadrant_grid; ++column) {
                const int x = ox + column;
                const int y = oy + row;
                const float h = at(x, y);
                // Skyrim (x east, y north, z up) to Godot (x, z, -y).
                vertices.push_back(Vector3(static_cast<float>(x) * k_spacing, h,
                                           -static_cast<float>(y) * k_spacing) *
                                   scale);
                const float dx = (at(x + 1, y) - at(x - 1, y)) / (2.0F * k_spacing);
                const float dy = (at(x, y + 1) - at(x, y - 1)) / (2.0F * k_spacing);
                const Vector3 normal = Vector3(-dx, 1.0F, dy).normalized();
                normals.push_back(normal);
                // +U runs east; normal x tangent then points north, up the
                // image, as Godot expects (w = 1).
                const Vector3 tangent =
                    (Vector3(1, 0, 0) - normal * normal.dot(Vector3(1, 0, 0))).normalized();
                tangents.push_back(static_cast<double>(tangent.x));
                tangents.push_back(static_cast<double>(tangent.y));
                tangents.push_back(static_cast<double>(tangent.z));
                tangents.push_back(1.0);
                uvs.push_back(Vector2(static_cast<float>(x), -static_cast<float>(y)) /
                              static_cast<float>(k_last));
                Color colour(1, 1, 1);
                if (has_colours) {
                    const auto base = static_cast<flatbuffers::uoffset_t>((y * k_grid + x) * 3);
                    colour = Color(static_cast<float>(colours->Get(base)) / 255.0F,
                                   static_cast<float>(colours->Get(base + 1)) / 255.0F,
                                   static_cast<float>(colours->Get(base + 2)) / 255.0F);
                }
                vertex_colours.push_back(colour);
                const auto& w = weights[static_cast<std::size_t>(row * k_quadrant_grid + column)];
                for (int i = 0; i < 4; ++i) {
                    custom0.push_back(static_cast<double>(w[static_cast<std::size_t>(i)]));
                }
                custom1.push_back(static_cast<double>(w[4]));
                custom1.push_back(static_cast<double>(w[5]));
                custom1.push_back(0.0);
                custom1.push_back(0.0);
            }
        }

        // Godot's front faces wind clockwise seen from the front.
        godot::PackedInt32Array indices;
        for (int row = 0; row < k_quadrant_grid - 1; ++row) {
            for (int column = 0; column < k_quadrant_grid - 1; ++column) {
                const int a = row * k_quadrant_grid + column;
                const int b = a + 1;
                const int c = a + k_quadrant_grid;
                const int d = c + 1;
                indices.append_array(godot::PackedInt32Array({a, d, b, a, c, d}));
            }
        }

        godot::Array arrays;
        arrays.resize(godot::Mesh::ARRAY_MAX);
        arrays[godot::Mesh::ARRAY_VERTEX] = vertices;
        arrays[godot::Mesh::ARRAY_NORMAL] = normals;
        arrays[godot::Mesh::ARRAY_TANGENT] = tangents;
        arrays[godot::Mesh::ARRAY_TEX_UV] = uvs;
        arrays[godot::Mesh::ARRAY_COLOR] = vertex_colours;
        arrays[godot::Mesh::ARRAY_CUSTOM0] = custom0;
        arrays[godot::Mesh::ARRAY_CUSTOM1] = custom1;
        arrays[godot::Mesh::ARRAY_INDEX] = indices;

        Ref<godot::ArrayMesh> mesh;
        mesh.instantiate();
        const auto flags =
            static_cast<std::int64_t>(godot::Mesh::ARRAY_CUSTOM_RGBA_FLOAT)
                << godot::Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT |
            static_cast<std::int64_t>(godot::Mesh::ARRAY_CUSTOM_RGBA_FLOAT)
                << godot::Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT;
        mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays, godot::Array(),
                                      godot::Dictionary(),
                                      static_cast<godot::BitField<godot::Mesh::ArrayFormat>>(flags));
        mesh->surface_set_material(0, material_for(textures));

        auto* instance = memnew(godot::MeshInstance3D);
        instance->set_name(String("Quadrant") + String::num_int64(quadrant));
        instance->set_mesh(mesh);
        root->add_child(instance);
    }

    if (!collision_) {
        return root;
    }
    // Collision: the same triangles as the quadrants draw.
    godot::PackedVector3Array faces;
    faces.resize(static_cast<std::int64_t>(k_last) * k_last * 6);
    const auto vertex = [&](int x, int y) {
        return Vector3(static_cast<float>(x) * k_spacing, at(x, y), -static_cast<float>(y) * k_spacing) *
               scale;
    };
    std::int64_t next = 0;
    for (int y = 0; y < k_last; ++y) {
        for (int x = 0; x < k_last; ++x) {
            const Vector3 a = vertex(x, y);
            const Vector3 b = vertex(x + 1, y);
            const Vector3 c = vertex(x, y + 1);
            const Vector3 d = vertex(x + 1, y + 1);
            for (const Vector3& v : {a, d, b, a, c, d}) {
                faces.set(next++, v);
            }
        }
    }
    Ref<godot::ConcavePolygonShape3D> ground;
    ground.instantiate();
    ground->set_backface_collision_enabled(true);
    ground->set_faces(faces);
    auto* shape = memnew(godot::CollisionShape3D);
    shape->set_shape(ground);
    auto* body = memnew(godot::StaticBody3D);
    body->set_name("Collision");
    body->set_collision_layer(physics_layer::terrain);
    body->set_collision_mask(0);
    body->add_child(shape);
    root->add_child(body);
    return root;
}

} // namespace skydot
