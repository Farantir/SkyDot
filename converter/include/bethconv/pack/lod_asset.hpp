// SPDX-License-Identifier: GPL-3.0-or-later
//
// LOD assets: a worldspace's LOD settings, tree types and tree instances as a
// FlatBuffer (schema formats/schema/lod.fbs), so the engine never parses them.
//
// Layouts, all little-endian, checked against every file of the three
// vanilla installs (they tile their files exactly):
//
//   .lod  int16 south-west x, int16 south-west y, int32 stride, int32 lowest
//         level, int32 highest level. 16 bytes.
//   .lst  uint32 count, then per tree type uint32 index, float width, float
//         height, float u0, v0, u1, v1, uint32 unknown.
//   .btt  uint32 block count, then per block uint32 type, uint32 count and
//         per tree float x, y, z, rotation, scale, uint32 reference, two
//         uint32 not understood. Six Solstheim files in SE and VR have 192 to
//         2,112 bytes after their declared blocks that follow no structure;
//         they are skipped and counted.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/io/span_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bethconv::pack {

/// Bumped whenever the meaning of anything in lod.fbs changes.
inline constexpr std::uint32_t k_lod_format_version = 1;

struct LodSettings {
    std::int16_t south_west_x{};
    std::int16_t south_west_y{};
    std::int32_t stride{};
    std::int32_t lowest_level{};
    std::int32_t highest_level{};
};

struct TreeType {
    std::uint32_t index{};
    float width{};
    float height{};
    float u0{};
    float v0{};
    float u1{};
    float v1{};
    std::uint32_t unknown{};
};

struct TreeInstance {
    std::uint32_t type{};
    float x{};
    float y{};
    float z{};
    float rotation{};
    float scale{};
    std::uint32_t ref{};
    std::uint32_t unknown1{};
    std::uint32_t unknown2{};
};

/// One decoded LOD file; exactly one part is set.
struct LodData {
    std::optional<LodSettings> settings;
    std::vector<TreeType> tree_types;
    std::vector<TreeInstance> trees;
    /// Bytes after a `.btt`'s declared blocks, skipped. Not stored.
    std::size_t trailing_bytes{};
};

/// Decode a `.lod`, `.lst` or `.btt` (by `extension`, with the dot).
/// Anything left over (except after a `.btt`'s blocks), a count larger than
/// the file, or levels that are not powers of two from 1 to 256 is an error.
[[nodiscard]] io::ParseResult<LodData> read_lod_source(std::span<const std::byte> bytes,
                                                       std::string_view extension,
                                                       std::string_view origin);

[[nodiscard]] std::vector<std::byte> write_lod_asset(const LodData& data);

/// Verify a LOD asset and read it back.
[[nodiscard]] io::ParseResult<LodData> read_lod_asset(std::span<const std::byte> bytes,
                                                      std::string_view origin);

} // namespace bethconv::pack
