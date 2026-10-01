// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic LOD data files (.lod, .lst, .btt) in the layouts
// include/bethconv/pack/lod_asset.hpp documents.
#pragma once

#include "esm_builder.hpp"

#include <cstdint>
#include <vector>

namespace bethconv::testing {

inline std::vector<std::byte> build_lod_settings(std::int16_t south_west_x, std::int16_t south_west_y,
                                                 std::int32_t stride, std::int32_t lowest,
                                                 std::int32_t highest) {
    test::ByteWriter w;
    w.u16(static_cast<std::uint16_t>(south_west_x));
    w.u16(static_cast<std::uint16_t>(south_west_y));
    w.u32(static_cast<std::uint32_t>(stride));
    w.u32(static_cast<std::uint32_t>(lowest));
    w.u32(static_cast<std::uint32_t>(highest));
    return w.bytes();
}

struct TreeTypeSpec {
    std::uint32_t index = 0;
    float width = 256.0F;
    float height = 512.0F;
    float u0 = 0.0F;
    float v0 = 0.0F;
    float u1 = 0.5F;
    float v1 = 1.0F;
};

inline std::vector<std::byte> build_tree_list(const std::vector<TreeTypeSpec>& types) {
    test::ByteWriter w;
    w.u32(static_cast<std::uint32_t>(types.size()));
    for (const auto& t : types) {
        w.u32(t.index);
        for (const float f : {t.width, t.height, t.u0, t.v0, t.u1, t.v1}) {
            w.f32(f);
        }
        w.u32(0);
    }
    return w.bytes();
}

struct TreeSpec {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float rotation = 0.0F;
    float scale = 1.0F;
    std::uint32_t ref = 0;
};

/// One block per entry: a type and its trees.
inline std::vector<std::byte> build_tree_blocks(
    const std::vector<std::pair<std::uint32_t, std::vector<TreeSpec>>>& blocks) {
    test::ByteWriter w;
    w.u32(static_cast<std::uint32_t>(blocks.size()));
    std::uint32_t serial = 0;
    for (const auto& [type, trees] : blocks) {
        w.u32(type);
        w.u32(static_cast<std::uint32_t>(trees.size()));
        for (const auto& t : trees) {
            for (const float f : {t.x, t.y, t.z, t.rotation, t.scale}) {
                w.f32(f);
            }
            w.u32(t.ref);
            w.u32(serial++);
            w.u32(0);
        }
    }
    return w.bytes();
}

} // namespace bethconv::testing
