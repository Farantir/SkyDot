// SPDX-License-Identifier: GPL-3.0-or-later
//
// NIF -> IR -> GLB.
//
// nifly parses the bytes itself and fails by returning non-zero or throwing;
// read_nif must contain both. The writer runs too, because an out-of-range
// reference in the IR is not a crash but becomes a corrupt GLB.
#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/mesh/nif_reader.hpp"

#include "fuzz_support.hpp"


extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    const auto bytes = bethconv::fuzz::as_bytes(data, size);

    const auto model = bethconv::mesh::read_nif(bytes, "fuzz.nif");
    if (!model) {
        return 0;
    }

    // Internal references must be in range before anything indexes with them.
    for (const auto& prim : model->primitives) {
        BETHCONV_FUZZ_CHECK(prim.material < model->materials.size() || model->materials.empty());
        if (prim.skin) {
            BETHCONV_FUZZ_CHECK(*prim.skin < model->skins.size());
        }
        // Indices refer to existing vertices and form whole triangles.
        BETHCONV_FUZZ_CHECK(prim.indices.size() % 3 == 0);
        for (const auto index : prim.indices) {
            BETHCONV_FUZZ_CHECK(index < prim.positions.size());
        }
        // Per-vertex attributes are absent or exactly as long as positions.
        BETHCONV_FUZZ_CHECK(prim.normals.empty() || prim.normals.size() == prim.positions.size());
        BETHCONV_FUZZ_CHECK(prim.tangents.empty() || prim.tangents.size() == prim.positions.size());
        BETHCONV_FUZZ_CHECK(prim.uvs.empty() || prim.uvs.size() == prim.positions.size());
        BETHCONV_FUZZ_CHECK(prim.colors.empty() || prim.colors.size() == prim.positions.size());
        BETHCONV_FUZZ_CHECK(prim.joints.empty() || prim.joints.size() == prim.positions.size());
        BETHCONV_FUZZ_CHECK(prim.weights.size() == prim.joints.size());
    }
    for (const auto& node : model->nodes) {
        for (const auto child : node.children) {
            BETHCONV_FUZZ_CHECK(child < model->nodes.size());
        }
        for (const auto prim : node.primitives) {
            BETHCONV_FUZZ_CHECK(prim < model->primitives.size());
        }
    }
    for (const auto root : model->roots) {
        BETHCONV_FUZZ_CHECK(root < model->nodes.size());
    }
    for (const auto& skin : model->skins) {
        BETHCONV_FUZZ_CHECK(skin.inverse_bind_matrices.size() == skin.joints.size());
        for (const auto joint : skin.joints) {
            BETHCONV_FUZZ_CHECK(joint < model->nodes.size());
        }
    }

    (void)bethconv::mesh::write_glb(*model);
    return 0;
}
