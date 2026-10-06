// SPDX-License-Identifier: GPL-3.0-or-later
//
// Coverage-preserving mip levels for alpha-tested textures.
//
// A cut-out texture (leaves, grass, hair) is drawn with an alpha test at some
// threshold. Averaging alpha down the mip chain thins it: the fraction of
// texels at or above the threshold falls with every level, so distant foliage
// turns sparse and, as the level changes with distance, shimmers. The fix is
// Castano's ("Computing alpha mipmaps", NVIDIA, 2010; the same as NVTT's
// alpha coverage): measure the fraction of texels >= threshold in level 0 and
// scale the alpha of every lower level by the factor that restores it.
//
// Only alpha is touched; colour is kept as the file has it. Per format:
//   DXT5/BC3  the 8-byte alpha block is re-encoded on its own; colour blocks
//             are not read.
//   DXT3/BC2  the 4-bit alpha is requantized in place.
//   BC7       decoded, alpha scaled, re-encoded (a lossy second pass over the
//             levels that needed a different scale).
//   RGBA with an 8-bit alpha mask (uncompressed) in place.
//   DXT1/BC1  one-bit alpha cannot be scaled: reported unsupported (coverage
//             is still measured).
// Cubemaps, volumes and arrays are left alone.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/texture/dds.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::texture {

enum class CoverageOutcome : std::uint8_t {
    adjusted,     ///< At least one level got new alpha; `data` is the new file.
    unchanged,    ///< Coverage already holds (or is 0 or 1 everywhere).
    single_level, ///< No chain to fix.
    unsupported,  ///< Format or surface kind; `reason` says which.
};

[[nodiscard]] std::string_view to_string(CoverageOutcome outcome) noexcept;

struct CoverageFix {
    CoverageOutcome outcome{CoverageOutcome::unchanged};
    std::string reason;
    /// Fraction of texels with alpha >= threshold, per stored level, before and
    /// after. Empty when the alpha could not be read.
    std::vector<double> before;
    std::vector<double> after;
    /// Alpha scale applied per level (1 = untouched).
    std::vector<float> scale;
    /// The new file; only for `adjusted`.
    std::vector<std::byte> data;
};

/// Fraction of texels of every stored level with alpha >= `threshold` (1..255),
/// or an empty vector if the format's alpha cannot be read.
[[nodiscard]] io::ParseResult<std::vector<double>> measure_alpha_coverage(
    std::span<const std::byte> file, const DdsInfo& info, std::uint32_t threshold,
    std::string_view origin);

/// Rescale the alpha of levels 1.. so each has level 0's coverage at
/// `threshold`. `info` must come from parse_dds() on the same bytes.
[[nodiscard]] io::ParseResult<CoverageFix> preserve_alpha_coverage(std::span<const std::byte> file,
                                                                   const DdsInfo& info,
                                                                   std::uint32_t threshold,
                                                                   std::string_view origin);

} // namespace bethconv::texture
