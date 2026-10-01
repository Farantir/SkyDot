// SPDX-License-Identifier: GPL-3.0-or-later
//
// Makes a DDS texture smaller by dropping its largest mip levels, without
// decoding: the smaller levels are already in the file, so a texture limited to
// 1024 px keeps its 1024 px level and everything below it, byte for byte. Each
// halving cuts the size by four. This is the first texture profile setting
// (TOOLS-REQUIREMENTS.md, section 2), for low-end and standalone VR targets.
//
// Only the header fields that describe the top level change: dwHeight,
// dwWidth, dwPitchOrLinearSize (when the file sets it) and dwMipMapCount.
// Cubemaps are cut face by face, since their levels are stored per face.
//
// A texture without a chain, or whose chain stops before a level that fits,
// cannot shrink this way and is passed through; the caller reports it.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/texture/dds.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bethconv::texture {

/// File offsets in the DDS header (magic included).
inline constexpr std::size_t k_height_offset = 12;
inline constexpr std::size_t k_width_offset = 16;
inline constexpr std::size_t k_pitch_offset = 20;

enum class DropOutcome : std::uint8_t {
    fits,           ///< Already within the limit.
    shrunk,         ///< Leading levels dropped.
    single_level,   ///< Too large, but no chain to take a smaller level from.
    too_few_levels, ///< Too large, and the chain stops before a level that fits.
    unsupported,    ///< Volume or texture array; never resized.
};

[[nodiscard]] std::string_view to_string(DropOutcome outcome) noexcept;

struct SizeLimit {
    DropOutcome outcome{DropOutcome::fits};
    std::uint32_t dropped_levels{};
    std::uint32_t width{};   ///< After the drop.
    std::uint32_t height{};
    std::size_t saved_bytes{};
    /// The smaller file. Empty unless `outcome == shrunk`.
    std::vector<std::byte> data;
};

/// Drop leading levels of `file` until its larger side is at most
/// `max_extent` (0: no limit). `info` must come from parse_dds() on the same
/// bytes. Bytes past the declared surfaces are not carried over.
[[nodiscard]] io::ParseResult<SizeLimit> limit_size(std::span<const std::byte> file,
                                                    const DdsInfo& info, std::uint32_t max_extent,
                                                    std::string_view origin);

} // namespace bethconv::texture
