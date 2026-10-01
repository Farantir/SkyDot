// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic NIF fixtures for the mesh tests, written with nifly. The tests
// cover our layer (IR, attribute rules, materials, glTF output), not nifly.
//
// NifFile::Create takes a version, and the same CreateShapeFromData call gives
// a NiTriShape at stream 83 and a BSTriShape at stream 100, so both LE and SE
// geometry can be generated.
#pragma once

#include <NifFile.hpp>
#include <Nodes.hpp>
#include <Geometry.hpp>
#include <Shaders.hpp>

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace bethconv::test {

/// Which encoding to generate.
enum class NifFlavor { le, se };

/// A cube: eight corners, twelve triangles, UVs and normals. Not a plane, whose
/// identical normals would hide winding bugs.
struct CubeMesh {
    std::vector<nifly::Vector3> verts;
    std::vector<nifly::Triangle> tris;
    std::vector<nifly::Vector2> uvs;
    std::vector<nifly::Vector3> normals;
};

[[nodiscard]] inline CubeMesh make_cube(float size = 10.0f) {
    CubeMesh cube;
    const float h = size / 2.0f;
    for (int i = 0; i < 8; ++i) {
        const float x = (i & 1) != 0 ? h : -h;
        const float y = (i & 2) != 0 ? h : -h;
        const float z = (i & 4) != 0 ? h : -h;
        cube.verts.emplace_back(x, y, z);
        cube.uvs.emplace_back((x + h) / size, (y + h) / size);
        // Smooth corner normals: the normalized position.
        const float len = std::sqrt(x * x + y * y + z * z);
        cube.normals.emplace_back(x / len, y / len, z / len);
    }
    static constexpr std::uint16_t kFaces[12][3] = {
        {0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
        {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5},
    };
    for (const auto& f : kFaces) {
        cube.tris.emplace_back(f[0], f[1], f[2]);
    }
    return cube;
}

/// Builds a NIF in memory and hands back its bytes.
class NifBuilder {
public:
    explicit NifBuilder(NifFlavor flavor) {
        nif_.Create(flavor == NifFlavor::le ? nifly::NiVersion::getSK()
                                            : nifly::NiVersion::getSSE());
    }

    /// Adds a shape and returns it, for attaching a shader or transform.
    nifly::NiShape* add_shape(const std::string& name, const CubeMesh& mesh) {
        return nif_.CreateShapeFromData(name, &mesh.verts, &mesh.tris, &mesh.uvs,
                                        &mesh.normals);
    }

    /// Attaches a BSLightingShaderProperty with a diffuse and a normal slot.
    nifly::BSLightingShaderProperty* add_shader(nifly::NiShape* shape,
                                                const std::string& diffuse,
                                                const std::string& normal = {}) {
        auto property = std::make_unique<nifly::BSLightingShaderProperty>();
        property->bslspShaderType = 0;
        auto* raw = property.get();
        const auto index = nif_.GetHeader().AddBlock(std::move(property));

        auto textures = std::make_unique<nifly::BSShaderTextureSet>();
        textures->textures.resize(9);
        textures->textures[0].get() = diffuse;
        if (!normal.empty()) {
            textures->textures[1].get() = normal;
        }
        const auto tex_index = nif_.GetHeader().AddBlock(std::move(textures));
        raw->TextureSetRef()->index = tex_index;

        auto* geometry = dynamic_cast<nifly::NiShape*>(shape);
        geometry->ShaderPropertyRef()->index = index;
        return raw;
    }

    /// Attaches a NiAlphaProperty. `flags` is the raw bitfield: bit 0 blending,
    /// bit 9 alpha testing.
    void add_alpha(nifly::NiShape* shape, std::uint16_t flags, std::uint8_t threshold) {
        auto alpha = std::make_unique<nifly::NiAlphaProperty>();
        alpha->flags = flags;
        alpha->threshold = threshold;
        const auto index = nif_.GetHeader().AddBlock(std::move(alpha));
        shape->AlphaPropertyRef()->index = index;
    }

    /// Add an empty NiNode under `parent` (or the root), to build graphs for the
    /// depth and cycle guards.
    template <typename NodeType = nifly::NiNode>
    NodeType* add_node(const std::string& name, nifly::NiNode* parent = nullptr) {
        auto block = std::make_unique<NodeType>();
        block->name.get() = name;
        auto* raw = block.get();
        const auto index = nif_.GetHeader().AddBlock(std::move(block));
        nifly::NiNode* target = parent != nullptr ? parent : nif_.GetRootNode();
        if (target != nullptr) {
            target->childRefs.AddBlockRef(index);
        }
        return raw;
    }

    /// Add `child` to `parent` again by block index, e.g. to create a cycle.
    void add_child_ref(nifly::NiNode* parent, nifly::NiAVObject* child) {
        parent->childRefs.AddBlockRef(nif_.GetBlockID(child));
    }

    nifly::NifFile& file() { return nif_; }

    /// Serializes; returns empty on failure.
    [[nodiscard]] std::vector<std::byte> bytes() {
        std::ostringstream out(std::ios::binary);
        if (nif_.Save(out) != 0) {
            return {};
        }
        const std::string text = out.str();
        std::vector<std::byte> result(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            result[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
        }
        return result;
    }

private:
    nifly::NifFile nif_;
};

} // namespace bethconv::test
