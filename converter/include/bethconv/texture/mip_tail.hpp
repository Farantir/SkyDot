// SPDX-License-Identifier: GPL-3.0-or-later
//
// Completes a truncated DDS mip chain without decoding.
//
// Godot requires mipmapped DDS chains to reach 1x1 and rejects the whole file
// otherwise; 1,988 of 2,000 sampled SE textures stop early (usually missing only
// the last level). LE has none. The files are valid DDS; Godot is stricter than
// the format.
//
// Fix: correct dwMipMapCount and append the missing levels, nothing else. Each
// missing level is one block (a few for oblong surfaces), filled by repeating
// the last stored level, which already averages its area.
//
// Unlike the prototype (docs/spikes/nif-gltf/fix_mip_tail.py), this rebuilds
// cubemaps face by face, since their levels are stored per face.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/texture/dds.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bethconv::texture {

/// Result of the fix. Only `completed` produces new bytes; otherwise pass the
/// original file through.
enum class TailOutcome : std::uint8_t {
    already_complete, ///< Chain reaches 1x1 (nearly all of LE).
    single_level,     ///< No chain declared. Legal, and Godot accepts it.
    completed,        ///< Levels appended and dwMipMapCount corrected.
    unsupported,      ///< Volume or texture array; not resized.
};

[[nodiscard]] std::string_view to_string(TailOutcome outcome) noexcept;

struct TailFix {
    TailOutcome outcome{TailOutcome::already_complete};
    std::uint32_t from_levels{};
    std::uint32_t to_levels{};
    std::size_t added_bytes{};

    /// Bytes past the declared surfaces. Dropped (their offsets would be wrong)
    /// but counted for the report.
    std::size_t dropped_bytes{};

    /// The completed file. Empty unless `outcome == completed`.
    std::vector<std::byte> data;
};

/// Complete `file`'s mip chain. `info` must come from parse_dds() on the same
/// bytes. Never throws; never rewrites a file it does not fully understand.
[[nodiscard]] io::ParseResult<TailFix> complete_mip_tail(std::span<const std::byte> file,
                                                         const DdsInfo& info,
                                                         std::string_view origin);

} // namespace bethconv::texture
