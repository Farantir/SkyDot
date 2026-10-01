// SPDX-License-Identifier: GPL-3.0-or-later
//
// DDS header parsing and mip-chain arithmetic.
//
// No pixel decoding: desktop textures pass through, since Godot keeps DDS block
// formats and re-encoding would cost hours and quality. What is needed is the
// arithmetic (levels, bytes per level, faces), because SE textures stop their
// chains before 1x1 and Godot rejects such files.
//
// Source: Microsoft, "Programming Guide for DDS", DDS_HEADER / DDS_PIXELFORMAT /
// DDS_HEADER_DXT10:
// <https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide>
// <https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format>
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/io/span_reader.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bethconv::texture {

/// Magic plus the 124-byte DDS_HEADER. Pixel data follows unless there is a DX10
/// header.
inline constexpr std::size_t k_dds_header_size = 128;

/// DDS_HEADER_DXT10: dxgiFormat, resourceDimension, miscFlag, arraySize,
/// miscFlags2.
inline constexpr std::size_t k_dx10_header_size = 20;

/// File offset of dwMipMapCount, the only header field the mip fix changes.
inline constexpr std::size_t k_mipmap_count_offset = 28;

/// Upper bound on a dimension (vanilla max is 4096), checked before any
/// multiplication to prevent overflow.
inline constexpr std::uint32_t k_max_dimension = 65536;

/// dwCaps2 bits: DDSCAPS2_CUBEMAP plus one bit per face; DDSCAPS2_VOLUME marks
/// a 3D texture, which is never resized here.
inline constexpr std::uint32_t k_caps2_cubemap = 0x0000'0200u;
inline constexpr std::uint32_t k_caps2_cubemap_faces = 0x0000'FC00u;
inline constexpr std::uint32_t k_caps2_volume = 0x0020'0000u;

/// Surface type; decides the byte arithmetic.
///
/// Godot 4.7.2 loads cubemaps natively (see docs/format-notes/dds-textures.md).
/// They must not be treated as one surface: levels are stored per face, so a
/// tail appended at the end would land after face six.
enum class SurfaceKind : std::uint8_t {
    texture_2d,
    cubemap,
    volume,
};

[[nodiscard]] std::string_view to_string(SurfaceKind kind) noexcept;

/// Byte layout of one mip level. `unit_bytes` is per 4x4 block if
/// `block_compressed`, else per pixel. New formats are table entries.
struct PixelLayout {
    io::FourCC fourcc{};            ///< Zero for uncompressed files.
    std::uint32_t dxgi_format{};    ///< Zero unless the file carries DX10.
    std::uint32_t unit_bytes{};     ///< Bytes per block, or per pixel.
    std::uint32_t rgb_bit_count{};  ///< Uncompressed only.
    /// Uncompressed only: DDS_PIXELFORMAT's R, G, B, A bit masks and dwFlags
    /// (DDPF_ALPHAPIXELS 0x1 says whether the A mask is used).
    std::uint32_t masks[4]{};
    std::uint32_t pf_flags{};
    bool block_compressed{};
    std::string name;               ///< "DXT1", "BC7", "RGBA32"; for reports.
};

/// Header facts the texture pass needs; no pixel data.
struct DdsInfo {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{1};
    std::uint32_t declared_mips{};      ///< dwMipMapCount verbatim; 0 means none.
    std::uint32_t stored_levels{1};     ///< Levels actually present: max(1, declared).
    std::uint32_t full_chain_levels{1}; ///< Levels a chain reaching 1x1 needs.
    std::uint32_t faces{1};             ///< 6 for a cubemap with all faces.
    std::uint32_t array_size{1};        ///< DX10 only; >1 is not handled.
    SurfaceKind kind{SurfaceKind::texture_2d};
    PixelLayout layout;
    std::uint32_t caps2{};
    bool dx10{};
    std::size_t header_bytes{k_dds_header_size};
    std::size_t declared_bytes{};   ///< Header + every stored level, all faces.
    std::size_t full_chain_bytes{}; ///< Size with a chain down to 1x1.
    std::size_t file_bytes{};

    /// The chain stops before 1x1; Godot rejects such files if they declare a
    /// chain (1,988 of 2,000 sampled SE textures).
    [[nodiscard]] bool short_chain() const noexcept {
        return declared_mips > 1 && stored_levels < full_chain_levels;
    }

    /// No chain declared. Valid, and accepted by Godot.
    [[nodiscard]] bool single_level() const noexcept { return declared_mips <= 1; }
};

/// Number of levels down to 1x1: std::bit_width of the larger side. Also
/// correct for non-powers of two (192 -> ... -> 1 is 8 levels).
[[nodiscard]] constexpr std::uint32_t full_chain_levels(std::uint32_t width,
                                                        std::uint32_t height) noexcept {
    const std::uint32_t largest = width > height ? width : height;
    return largest == 0 ? 0 : static_cast<std::uint32_t>(std::bit_width(largest));
}

/// Dimensions of mip level `level`, floored at 1 in each axis.
[[nodiscard]] constexpr std::uint32_t level_extent(std::uint32_t extent,
                                                   std::uint32_t level) noexcept {
    const std::uint32_t shifted = level >= 32 ? 0 : extent >> level;
    return shifted == 0 ? 1 : shifted;
}

/// Bytes of one mip level of one face. Block formats round up to whole 4x4
/// blocks, so 2x2 and 1x1 DXT1 levels are one 8-byte block each.
[[nodiscard]] std::size_t level_bytes(const PixelLayout& layout, std::uint32_t width,
                                      std::uint32_t height) noexcept;

/// Bytes `levels` consecutive mip levels of one face occupy.
[[nodiscard]] std::size_t chain_bytes(const PixelLayout& layout, std::uint32_t width,
                                      std::uint32_t height, std::uint32_t levels) noexcept;

/// Parse a DDS header; reads no pixel data.
///
/// Fails when the size arithmetic cannot be trusted: bad magic, header size not
/// 124, zero or huge dimensions, more mips than the surface allows, an unsizable
/// pixel format, or a file shorter than its declared levels. Such textures are
/// reported and passed through unchanged.
[[nodiscard]] io::ParseResult<DdsInfo> parse_dds(std::span<const std::byte> bytes,
                                                 std::string_view origin);

} // namespace bethconv::texture
