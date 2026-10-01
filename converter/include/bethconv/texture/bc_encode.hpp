// SPDX-License-Identifier: GPL-3.0-or-later
//
// Block-compresses uncompressed DDS textures: BC7 (8 bits per pixel, 4x less
// than RGBA32) or BC1 (4 bits per pixel, 8x less) through bc7enc_rdo
// (extern/bc7enc_rdo: bc7enc for BC7, rgbcx for BC1). Already compressed
// textures are never re-encoded: lossy to lossy can only lose quality
// (TOOLS-REQUIREMENTS.md, section 2.2).
//
// Vanilla SE has 10,047 uncompressed 32-bit textures, most of them terrain LOD
// (docs/format-notes/dds-textures.md); mods ship many more.
//
// Every stored level is encoded on its own, so the file keeps its mip chain;
// levels smaller than a block repeat their edge pixels. Pixels are read with
// the file's channel masks (any byte-aligned 8 to 32-bit layout; luminance as
// grey). Cubemaps, volumes and arrays are left alone.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/texture/dds.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bethconv::texture {

/// What to do with uncompressed textures.
enum class Encoding : std::uint8_t {
    keep,    ///< Pass them through.
    bc7,     ///< Everything to BC7.
    compact, ///< BC1 when opaque and not a normal map, else BC7.
};

[[nodiscard]] std::string_view to_string(Encoding encoding) noexcept;

enum class EncodeOutcome : std::uint8_t {
    encoded,
    already_compressed, ///< Block format already; passed through.
    unsupported,        ///< Cubemap, volume, array or an unusable channel layout.
};

struct Encoded {
    EncodeOutcome outcome{EncodeOutcome::already_compressed};
    std::string_view format;   ///< "BC7" or "BC1" when encoded.
    std::string reason;        ///< For `unsupported`.
    std::vector<std::byte> data; ///< The new file when encoded.
};

/// Encode `file` (described by `info`, from parse_dds() on the same bytes).
/// `normal_map` keeps `compact` from choosing BC1, which bands normals badly.
/// `threads` 0 uses every core.
[[nodiscard]] io::ParseResult<Encoded> encode_uncompressed(std::span<const std::byte> file,
                                                           const DdsInfo& info, Encoding encoding,
                                                           bool normal_map, std::string_view origin,
                                                           unsigned threads = 0);

/// Decode one 4x4 BC7 or BC1 block to RGBA (64 bytes); for tests and quality
/// checks.
[[nodiscard]] bool decode_block(std::string_view format, std::span<const std::byte> block,
                                std::span<std::uint8_t, 64> rgba);

} // namespace bethconv::texture
