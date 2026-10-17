// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyrim's voices and music are WMA2 audio in an XWMA RIFF (".xwm"; a ".fuz"
// is a ".lip" in front of one). Godot cannot play it, so the converter decodes
// it with FFmpeg's libavformat and libavcodec (docs/adr/0005-audio-decoding.md)
// and stores Ogg Vorbis, encoded with libvorbis at a fixed quality.
//
// The output is deterministic: same input bytes and same library versions give
// the same Ogg bytes (fixed stream serial number, VBR, no randomness), so the
// blob store dedupes reruns. Every call uses its own libav contexts, so
// conversions may run on several threads at once.
#pragma once

#include "bethconv/io/parse_error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bethconv::audio {

/// libvorbis VBR quality of every converted stream (-0.1 to 1.0).
inline constexpr float k_vorbis_quality = 0.4F;

/// Ogg stream serial number of every converted stream.
inline constexpr std::uint32_t k_ogg_serial = 0x53445643; // "SDVC"

/// Decode an XWMA RIFF held in memory and encode it as Ogg Vorbis with the
/// same channel count and sample rate.
[[nodiscard]] io::ParseResult<std::vector<std::byte>> xwma_to_ogg(
    std::span<const std::byte> xwma, std::string_view origin);

/// A ".fuz": "FUZE", u32 version, u32 lip size, the lip bytes, then the XWMA
/// RIFF up to the end of the file. Both views point into the input.
struct FuzParts {
    std::span<const std::byte> lip;
    std::span<const std::byte> xwma;
};

[[nodiscard]] io::ParseResult<FuzParts> parse_fuz(std::span<const std::byte> fuz,
                                                  std::string_view origin);

/// SkyDot voice container: "SDVC", u32 version (1), u32 ogg offset, u32 ogg
/// size, u32 lip offset, u32 lip size (little endian, offsets from the start of
/// the container), then the data. Fails only if a part exceeds 4 GiB.
inline constexpr std::size_t k_voice_header_size = 24;
inline constexpr std::uint32_t k_voice_version = 1;

[[nodiscard]] std::vector<std::byte> write_voice_container(std::span<const std::byte> ogg,
                                                           std::span<const std::byte> lip);

} // namespace bethconv::audio
