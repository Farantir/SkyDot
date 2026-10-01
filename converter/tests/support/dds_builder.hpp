// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic DDS writer for the texture tests, mainly for malformed files: short
// chains, impossible mip counts, cubemaps missing faces, wrong header sizes.
//
// Level sizes are computed here by hand rather than with
// texture::level_bytes(), so fixture and code under test cannot share a bug.
#pragma once

#include "bethconv/io/byte_writer.hpp"
#include "bethconv/io/span_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bethconv::testing {

struct DdsSpec {
    std::uint32_t width = 4;
    std::uint32_t height = 4;

    /// Written to dwMipMapCount, and the number of levels emitted unless
    /// `stored_levels` says otherwise. 0 and 1 both mean one level.
    std::uint32_t mips = 1;

    /// Levels actually written, for files whose data is shorter than the header
    /// claims.
    std::uint32_t stored_levels = 0; ///< 0 means "match `mips`".

    io::FourCC fourcc{"DXT1"};      ///< Zero for uncompressed.
    std::uint32_t rgb_bit_count = 0;
    std::uint32_t pf_flags = 0;     ///< 0 derives it from the fields above.

    bool cubemap = false;
    std::uint32_t cube_faces = 6;   ///< Face bits to set, 1..6.
    bool volume = false;
    std::uint32_t depth = 1;

    bool dx10 = false;
    std::uint32_t dxgi_format = 98;    ///< DXGI_FORMAT_BC7_UNORM.
    std::uint32_t dimension = 3;       ///< D3D10_RESOURCE_DIMENSION_TEXTURE2D.
    std::uint32_t dx10_misc = 0;
    std::uint32_t array_size = 1;

    /// How the DX10 format is sized. The fixture is told rather than
    /// duplicating the DXGI table under test.
    bool dx10_block = true;
    std::uint32_t dx10_unit_bytes = 16; ///< BC7's block size.

    // Deliberate corruption.
    std::string magic = "DDS ";
    std::uint32_t header_dwsize = 124;
    std::uint32_t pf_dwsize = 32;
    std::size_t truncate_by = 0;    ///< Drop this many bytes off the end.
    std::size_t trailing_bytes = 0; ///< Junk appended after the surfaces.
};

/// Fill byte for a face and level. Unique per face and level, so tests can tell
/// where bytes came from (e.g. that a cubemap tail follows its own face).
[[nodiscard]] inline std::byte dds_fill(std::uint32_t face, std::uint32_t level) noexcept {
    return static_cast<std::byte>(0x10u * (face + 1u) + level + 1u);
}

[[nodiscard]] inline std::vector<std::byte> build_dds(const DdsSpec& spec) {
    const bool block = spec.dx10 ? spec.dx10_block : spec.fourcc.value != 0;
    std::uint32_t unit = spec.rgb_bit_count / 8u;
    if (spec.dx10) {
        unit = spec.dx10_unit_bytes;
    } else if (block) {
        unit = spec.fourcc == io::FourCC{"DXT1"} || spec.fourcc == io::FourCC{"ATI1"} ? 8u : 16u;
    }

    auto level_size = [&](std::uint32_t level) -> std::size_t {
        std::uint32_t w = spec.width >> level;
        std::uint32_t h = spec.height >> level;
        w = w == 0 ? 1 : w;
        h = h == 0 ? 1 : h;
        if (block) {
            return static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4) * unit;
        }
        return static_cast<std::size_t>(w) * h * unit;
    };

    std::uint32_t pf_flags = spec.pf_flags;
    if (pf_flags == 0) {
        pf_flags = block ? 0x4u : 0x41u; // DDPF_FOURCC, or DDPF_RGB|DDPF_ALPHAPIXELS
    }

    std::uint32_t caps2 = 0;
    if (spec.cubemap) {
        caps2 |= 0x200u; // DDSCAPS2_CUBEMAP
        for (std::uint32_t face = 0; face < spec.cube_faces; ++face) {
            caps2 |= 0x400u << face;
        }
    }
    if (spec.volume) {
        caps2 |= 0x200000u; // DDSCAPS2_VOLUME
    }

    std::uint32_t flags = 0x1007u; // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    if (spec.mips > 1) {
        flags |= 0x20000u; // DDSD_MIPMAPCOUNT
    }

    io::ByteWriter out;
    for (char c : spec.magic) {
        out.put(static_cast<std::uint8_t>(c));
    }
    out.put(spec.header_dwsize);
    out.put(flags);
    out.put(spec.height);
    out.put(spec.width);
    out.put(std::uint32_t{0}); // dwPitchOrLinearSize
    out.put(spec.depth);
    out.put(spec.mips);
    for (int i = 0; i < 11; ++i) {
        out.put(std::uint32_t{0}); // dwReserved1
    }
    out.put(spec.pf_dwsize);
    out.put(pf_flags);
    out.put(spec.dx10 ? io::FourCC{"DX10"}.value : spec.fourcc.value);
    out.put(spec.rgb_bit_count);
    out.put(std::uint32_t{0x00FF0000}); // R mask
    out.put(std::uint32_t{0x0000FF00});
    out.put(std::uint32_t{0x000000FF});
    out.put(std::uint32_t{0xFF000000});
    out.put(std::uint32_t{0x1000}); // dwCaps: DDSCAPS_TEXTURE
    out.put(caps2);
    out.put(std::uint32_t{0}); // dwCaps3
    out.put(std::uint32_t{0}); // dwCaps4
    out.put(std::uint32_t{0}); // dwReserved2
    if (spec.dx10) {
        out.put(spec.dxgi_format);
        out.put(spec.dimension);
        out.put(spec.dx10_misc);
        out.put(spec.array_size);
        out.put(std::uint32_t{0}); // miscFlags2
    }

    // DX10 files mark cubemaps via miscFlag; the data is six faces either way.
    const bool cube = spec.cubemap || (spec.dx10 && (spec.dx10_misc & 0x4u) != 0);
    const std::uint32_t faces = cube ? (spec.cubemap ? spec.cube_faces : 6u) : 1u;
    const std::uint32_t levels =
        spec.stored_levels != 0 ? spec.stored_levels : (spec.mips == 0 ? 1u : spec.mips);
    for (std::uint32_t face = 0; face < faces; ++face) {
        for (std::uint32_t level = 0; level < levels; ++level) {
            const std::byte fill = dds_fill(face, level);
            for (std::size_t i = 0, n = level_size(level); i < n; ++i) {
                out.put(fill);
            }
        }
    }
    for (std::size_t i = 0; i < spec.trailing_bytes; ++i) {
        out.put(std::byte{0xEE});
    }

    std::vector<std::byte> bytes = out.take();
    if (spec.truncate_by != 0) {
        bytes.resize(spec.truncate_by >= bytes.size() ? 0 : bytes.size() - spec.truncate_by);
    }
    return bytes;
}

} // namespace bethconv::testing
