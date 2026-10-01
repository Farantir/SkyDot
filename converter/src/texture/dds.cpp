// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/texture/dds.hpp"

#include <array>
#include <optional>

namespace bethconv::texture {
namespace {

// DDS_PIXELFORMAT dwFlags. Microsoft, DDS_PIXELFORMAT reference.
constexpr std::uint32_t k_ddpf_alphapixels = 0x0000'0001u;
constexpr std::uint32_t k_ddpf_alpha = 0x0000'0002u;
constexpr std::uint32_t k_ddpf_fourcc = 0x0000'0004u;
constexpr std::uint32_t k_ddpf_rgb = 0x0000'0040u;
constexpr std::uint32_t k_ddpf_luminance = 0x0002'0000u;

// Sizes the format mandates; files that disagree are rejected.
constexpr std::uint32_t k_dds_header_dwsize = 124;
constexpr std::uint32_t k_pixelformat_dwsize = 32;

// DDS_HEADER_DXT10 resourceDimension (D3D10_RESOURCE_DIMENSION) and miscFlag.
constexpr std::uint32_t k_resource_dimension_texture3d = 4;
constexpr std::uint32_t k_dx10_misc_texturecube = 0x4u;

struct BlockFormat {
    io::FourCC fourcc;
    std::uint32_t unit_bytes;
    std::string_view name;
};

/// FOURCC block formats found in Bethesda data, plus alternate BC4/BC5 names.
/// DXT1 and BC4 use 8 bytes per 4x4 block, the rest 16. ATI1/ATI2 are the
/// pre-DX10 names for BC4/BC5. Source: Microsoft, "Block Compression" and the
/// DDS programming guide.
constexpr std::array<BlockFormat, 11> k_block_formats{{
    {io::FourCC{"DXT1"}, 8, "DXT1"},
    {io::FourCC{"DXT2"}, 16, "DXT2"},
    {io::FourCC{"DXT3"}, 16, "DXT3"},
    {io::FourCC{"DXT4"}, 16, "DXT4"},
    {io::FourCC{"DXT5"}, 16, "DXT5"},
    {io::FourCC{"ATI1"}, 8, "ATI1"},
    {io::FourCC{"BC4U"}, 8, "BC4U"},
    {io::FourCC{"BC4S"}, 8, "BC4S"},
    {io::FourCC{"ATI2"}, 16, "ATI2"},
    {io::FourCC{"BC5U"}, 16, "BC5U"},
    {io::FourCC{"BC5S"}, 16, "BC5S"},
}};

struct DxgiFormat {
    std::uint32_t first; ///< Inclusive start of a typeless/unorm/srgb run.
    std::uint32_t last;  ///< Inclusive end.
    std::uint32_t unit_bytes;
    bool block;
    std::string_view name;
};

/// DXGI_FORMAT values as ranges, since each format's typeless/unorm/srgb/snorm
/// variants are consecutive and share a size.
///
/// Vanilla has no DX10 headers, but modded setups have plenty of BC7, which
/// needs one. Source: the DXGI_FORMAT enumeration.
constexpr std::array<DxgiFormat, 14> k_dxgi_formats{{
    {2, 4, 16, false, "RGBA32F"},   // R32G32B32A32_TYPELESS..UINT
    {9, 13, 8, false, "RGBA16"},    // R16G16B16A16_TYPELESS..SINT
    {27, 32, 4, false, "RGBA8"},    // R8G8B8A8_TYPELESS..SINT
    {48, 52, 2, false, "RG8"},      // R8G8_TYPELESS..SINT
    {60, 65, 1, false, "R8"},       // R8_TYPELESS..A8_UNORM
    {70, 72, 8, true, "BC1"},       // BC1_TYPELESS..BC1_UNORM_SRGB
    {73, 75, 16, true, "BC2"},      //
    {76, 78, 16, true, "BC3"},      //
    {79, 81, 8, true, "BC4"},       //
    {82, 84, 16, true, "BC5"},      //
    {87, 88, 4, false, "BGRA8"},    // B8G8R8A8_UNORM, B8G8R8X8_UNORM
    {90, 93, 4, false, "BGRA8"},    // B8G8R8A8_TYPELESS..B8G8R8X8_UNORM_SRGB
    {94, 96, 16, true, "BC6H"},     //
    {97, 99, 16, true, "BC7"},      //
}};

[[nodiscard]] std::optional<BlockFormat> find_block_format(io::FourCC fourcc) noexcept {
    for (const BlockFormat& format : k_block_formats) {
        if (format.fourcc == fourcc) {
            return format;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<DxgiFormat> find_dxgi_format(std::uint32_t dxgi) noexcept {
    for (const DxgiFormat& format : k_dxgi_formats) {
        if (dxgi >= format.first && dxgi <= format.last) {
            return format;
        }
    }
    return std::nullopt;
}

/// A name for an uncompressed layout from the channel flags. Channel order is
/// not checked (only bytes per pixel matter), so the name stays coarse.
[[nodiscard]] std::string uncompressed_name(std::uint32_t pf_flags, std::uint32_t bits) {
    const std::string suffix = std::to_string(bits);
    if ((pf_flags & k_ddpf_luminance) != 0) {
        return "L" + suffix;
    }
    if ((pf_flags & k_ddpf_rgb) == 0 && (pf_flags & k_ddpf_alpha) != 0) {
        return "A" + suffix;
    }
    return ((pf_flags & k_ddpf_alphapixels) != 0 ? "RGBA" : "RGB") + suffix;
}

} // namespace

std::string_view to_string(SurfaceKind kind) noexcept {
    switch (kind) {
    case SurfaceKind::texture_2d: return "2d";
    case SurfaceKind::cubemap: return "cubemap";
    case SurfaceKind::volume: return "volume";
    }
    return "?";
}

std::size_t level_bytes(const PixelLayout& layout, std::uint32_t width,
                        std::uint32_t height) noexcept {
    const std::size_t unit = layout.unit_bytes;
    if (layout.block_compressed) {
        const std::size_t blocks_x = (static_cast<std::size_t>(width) + 3) / 4;
        const std::size_t blocks_y = (static_cast<std::size_t>(height) + 3) / 4;
        return (blocks_x == 0 ? 1 : blocks_x) * (blocks_y == 0 ? 1 : blocks_y) * unit;
    }
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * unit;
}

std::size_t chain_bytes(const PixelLayout& layout, std::uint32_t width, std::uint32_t height,
                        std::uint32_t levels) noexcept {
    std::size_t total = 0;
    for (std::uint32_t level = 0; level < levels; ++level) {
        total += level_bytes(layout, level_extent(width, level), level_extent(height, level));
    }
    return total;
}

io::ParseResult<DdsInfo> parse_dds(std::span<const std::byte> bytes, std::string_view origin) {
    io::SpanReader reader(bytes, origin);

    auto magic = reader.tag();
    if (!magic) {
        return std::unexpected(std::move(magic).error());
    }
    if (*magic != io::FourCC{"DDS "}) {
        return reader.fail(io::ErrorKind::bad_magic,
                           "expected \"DDS \", got \"" + magic->to_string() + "\"");
    }

    auto header_size = reader.get<std::uint32_t>();
    if (!header_size) {
        return std::unexpected(std::move(header_size).error());
    }
    if (*header_size != k_dds_header_dwsize) {
        return reader.fail(io::ErrorKind::bad_value,
                           "DDS_HEADER.dwSize is " + std::to_string(*header_size) +
                               ", must be " + std::to_string(k_dds_header_dwsize));
    }

    DdsInfo info;
    info.file_bytes = bytes.size();

    // dwFlags is ignored; writers set it inconsistently.
    if (auto flags = reader.get<std::uint32_t>(); !flags) {
        return std::unexpected(std::move(flags).error());
    }

    auto height = reader.get<std::uint32_t>();
    if (!height) {
        return std::unexpected(std::move(height).error());
    }
    auto width = reader.get<std::uint32_t>();
    if (!width) {
        return std::unexpected(std::move(width).error());
    }
    info.height = *height;
    info.width = *width;

    // dwPitchOrLinearSize is ignored; writers disagree and the fix keeps the
    // top-level size.
    if (auto pitch = reader.get<std::uint32_t>(); !pitch) {
        return std::unexpected(std::move(pitch).error());
    }

    auto depth = reader.get<std::uint32_t>();
    if (!depth) {
        return std::unexpected(std::move(depth).error());
    }
    info.depth = *depth == 0 ? 1 : *depth;

    auto mips = reader.get<std::uint32_t>();
    if (!mips) {
        return std::unexpected(std::move(mips).error());
    }
    info.declared_mips = *mips;

    if (auto skipped = reader.skip(44); !skipped) { // dwReserved1[11]
        return std::unexpected(std::move(skipped).error());
    }

    auto pf_size = reader.get<std::uint32_t>();
    if (!pf_size) {
        return std::unexpected(std::move(pf_size).error());
    }
    if (*pf_size != k_pixelformat_dwsize) {
        return reader.fail(io::ErrorKind::bad_value,
                           "DDS_PIXELFORMAT.dwSize is " + std::to_string(*pf_size) +
                               ", must be " + std::to_string(k_pixelformat_dwsize));
    }
    auto pf_flags = reader.get<std::uint32_t>();
    if (!pf_flags) {
        return std::unexpected(std::move(pf_flags).error());
    }
    auto fourcc = reader.tag();
    if (!fourcc) {
        return std::unexpected(std::move(fourcc).error());
    }
    auto rgb_bits = reader.get<std::uint32_t>();
    if (!rgb_bits) {
        return std::unexpected(std::move(rgb_bits).error());
    }
    std::uint32_t masks[4]{}; // R/G/B/A bit masks
    for (auto& mask : masks) {
        auto value = reader.get<std::uint32_t>();
        if (!value) {
            return std::unexpected(std::move(value).error());
        }
        mask = *value;
    }

    if (auto caps = reader.get<std::uint32_t>(); !caps) { // dwCaps
        return std::unexpected(std::move(caps).error());
    }
    auto caps2 = reader.get<std::uint32_t>();
    if (!caps2) {
        return std::unexpected(std::move(caps2).error());
    }
    info.caps2 = *caps2;
    if (auto tail = reader.skip(12); !tail) { // dwCaps3, dwCaps4, dwReserved2
        return std::unexpected(std::move(tail).error());
    }

    // ---- pixel format -----------------------------------------------------

    info.dx10 = (*pf_flags & k_ddpf_fourcc) != 0 && *fourcc == io::FourCC{"DX10"};
    if (info.dx10) {
        info.header_bytes = k_dds_header_size + k_dx10_header_size;
        auto dxgi = reader.get<std::uint32_t>();
        if (!dxgi) {
            return std::unexpected(std::move(dxgi).error());
        }
        auto dimension = reader.get<std::uint32_t>();
        if (!dimension) {
            return std::unexpected(std::move(dimension).error());
        }
        auto misc = reader.get<std::uint32_t>();
        if (!misc) {
            return std::unexpected(std::move(misc).error());
        }
        auto array_size = reader.get<std::uint32_t>();
        if (!array_size) {
            return std::unexpected(std::move(array_size).error());
        }
        if (auto misc2 = reader.get<std::uint32_t>(); !misc2) {
            return std::unexpected(std::move(misc2).error());
        }

        const auto format = find_dxgi_format(*dxgi);
        if (!format) {
            return reader.fail(io::ErrorKind::unsupported,
                               "DXGI_FORMAT " + std::to_string(*dxgi) + " has no size entry");
        }
        info.layout.dxgi_format = *dxgi;
        info.layout.unit_bytes = format->unit_bytes;
        info.layout.block_compressed = format->block;
        info.layout.name = std::string(format->name);
        info.array_size = *array_size == 0 ? 1 : *array_size;
        if ((*misc & k_dx10_misc_texturecube) != 0) {
            info.caps2 |= k_caps2_cubemap | k_caps2_cubemap_faces;
        }
        if (*dimension == k_resource_dimension_texture3d) {
            info.caps2 |= k_caps2_volume;
        }
    } else if ((*pf_flags & k_ddpf_fourcc) != 0) {
        const auto format = find_block_format(*fourcc);
        if (!format) {
            return reader.fail(io::ErrorKind::unsupported,
                               "FOURCC \"" + fourcc->to_string() + "\" has no size entry");
        }
        info.layout.fourcc = *fourcc;
        info.layout.unit_bytes = format->unit_bytes;
        info.layout.block_compressed = true;
        info.layout.name = std::string(format->name);
    } else if ((*pf_flags & (k_ddpf_rgb | k_ddpf_luminance | k_ddpf_alpha)) != 0) {
        if (*rgb_bits == 0 || *rgb_bits % 8 != 0) {
            return reader.fail(io::ErrorKind::unsupported,
                               "uncompressed dwRGBBitCount " + std::to_string(*rgb_bits) +
                                   " is not a whole number of bytes");
        }
        info.layout.unit_bytes = *rgb_bits / 8;
        info.layout.rgb_bit_count = *rgb_bits;
        for (std::size_t c = 0; c < 4; ++c) {
            info.layout.masks[c] = masks[c];
        }
        info.layout.pf_flags = *pf_flags;
        info.layout.block_compressed = false;
        info.layout.name = uncompressed_name(*pf_flags, *rgb_bits);
    } else {
        return reader.fail(io::ErrorKind::unsupported,
                           "DDS_PIXELFORMAT.dwFlags " + std::to_string(*pf_flags) +
                               " names neither a FOURCC nor a channel layout");
    }

    // ---- surface shape ----------------------------------------------------

    if (info.width == 0 || info.height == 0) {
        return reader.fail(io::ErrorKind::bad_value,
                           "surface is " + std::to_string(info.width) + "x" +
                               std::to_string(info.height));
    }
    if (info.width > k_max_dimension || info.height > k_max_dimension) {
        return reader.fail(io::ErrorKind::too_large,
                           "surface is " + std::to_string(info.width) + "x" +
                               std::to_string(info.height) + ", ceiling is " +
                               std::to_string(k_max_dimension));
    }

    if ((info.caps2 & k_caps2_volume) != 0 || info.depth > 1) {
        info.kind = SurfaceKind::volume;
    } else if ((info.caps2 & k_caps2_cubemap) != 0) {
        info.kind = SurfaceKind::cubemap;
        // Partial cubemaps are allowed; count the face bits.
        const auto faces = static_cast<std::uint32_t>(
            std::popcount(info.caps2 & k_caps2_cubemap_faces));
        info.faces = faces == 0 ? 1 : faces;
    }

    info.stored_levels = info.declared_mips == 0 ? 1 : info.declared_mips;
    info.full_chain_levels = full_chain_levels(info.width, info.height);
    if (info.stored_levels > info.full_chain_levels) {
        return reader.fail(io::ErrorKind::bad_value,
                           "dwMipMapCount " + std::to_string(info.declared_mips) + " exceeds " +
                               std::to_string(info.full_chain_levels) + ", the most a " +
                               std::to_string(info.width) + "x" + std::to_string(info.height) +
                               " surface can hold");
    }

    // Volume chains also halve depth and are never resized, so their byte
    // totals stay zero instead of a wrong 2D figure.
    if (info.kind != SurfaceKind::volume) {
        const std::size_t faces = info.faces;
        info.declared_bytes =
            info.header_bytes +
            faces * chain_bytes(info.layout, info.width, info.height, info.stored_levels);
        info.full_chain_bytes =
            info.header_bytes +
            faces * chain_bytes(info.layout, info.width, info.height, info.full_chain_levels);
    }

    if (info.kind != SurfaceKind::volume && info.file_bytes < info.declared_bytes) {
        return reader.fail(io::ErrorKind::truncated,
                           "file is " + std::to_string(info.file_bytes) + " bytes, its " +
                               std::to_string(info.stored_levels) + " levels x " +
                               std::to_string(info.faces) + " face(s) need " +
                               std::to_string(info.declared_bytes));
    }

    return info;
}

} // namespace bethconv::texture
