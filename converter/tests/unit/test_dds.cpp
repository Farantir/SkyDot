// SPDX-License-Identifier: GPL-3.0-or-later
//
// DDS level, byte and face arithmetic. Mistakes here do not change colors;
// they produce files Godot rejects entirely.
#include "bethconv/texture/dds.hpp"

#include "../support/dds_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>

using bethconv::io::ErrorKind;
using bethconv::io::FourCC;
using bethconv::testing::DdsSpec;
using bethconv::testing::build_dds;
using namespace bethconv::texture;

namespace {
DdsInfo parsed(const DdsSpec& spec) {
    const auto bytes = build_dds(spec);
    auto info = parse_dds(bytes, "fixture.dds");
    REQUIRE(info.has_value());
    return *info;
}

ErrorKind rejected(const DdsSpec& spec) {
    const auto bytes = build_dds(spec);
    auto info = parse_dds(bytes, "fixture.dds");
    REQUIRE_FALSE(info.has_value());
    return info.error().kind;
}
} // namespace

TEST_CASE("a chain reaches 1x1 in floor(log2(max))+1 levels", "[texture][dds]") {
    CHECK(full_chain_levels(1, 1) == 1);
    CHECK(full_chain_levels(2, 2) == 2);
    CHECK(full_chain_levels(512, 512) == 10);
    CHECK(full_chain_levels(256, 32) == 9);   // oblong: the larger side decides
    CHECK(full_chain_levels(4096, 4096) == 13);
    // Non-power-of-two: 192 -> 96 -> 48 -> 24 -> 12 -> 6 -> 3 -> 1.
    CHECK(full_chain_levels(192, 64) == 8);
    CHECK(full_chain_levels(0, 0) == 0);
}

TEST_CASE("a block level is whole 4x4 blocks, never a fraction of one",
          "[texture][dds]") {
    const PixelLayout dxt1{.fourcc = FourCC{"DXT1"},
                           .dxgi_format = 0,
                           .unit_bytes = 8,
                           .rgb_bit_count = 0,
                           .block_compressed = true,
                           .name = "DXT1"};
    CHECK(level_bytes(dxt1, 4, 4) == 8);
    CHECK(level_bytes(dxt1, 2, 2) == 8); // rounded up to one block
    CHECK(level_bytes(dxt1, 1, 1) == 8);
    CHECK(level_bytes(dxt1, 16, 2) == 32); // 4x1 blocks
    // 512x512 DXT1 with ten levels: the size in Godot's error message, versus
    // the 174768 bytes of Bethesda's nine-level file.
    CHECK(chain_bytes(dxt1, 512, 512, 10) == 174776);
    CHECK(chain_bytes(dxt1, 512, 512, 9) == 174768);
}

TEST_CASE("a minimal DXT1 surface parses to its exact declared size",
          "[texture][dds]") {
    const DdsInfo info = parsed({.width = 4, .height = 4, .mips = 1});
    CHECK(info.width == 4);
    CHECK(info.height == 4);
    CHECK(info.faces == 1);
    CHECK(info.kind == SurfaceKind::texture_2d);
    CHECK(info.layout.name == "DXT1");
    CHECK(info.layout.unit_bytes == 8);
    CHECK(info.layout.block_compressed);
    CHECK(info.stored_levels == 1);
    CHECK(info.full_chain_levels == 3);
    CHECK(info.single_level());
    CHECK_FALSE(info.short_chain()); // no chain is not a short chain
    CHECK(info.declared_bytes == k_dds_header_size + 8);
    CHECK(info.header_bytes == k_dds_header_size);
}

TEST_CASE("dwMipMapCount 0 and 1 both mean one level", "[texture][dds]") {
    CHECK(parsed({.mips = 0}).stored_levels == 1);
    CHECK(parsed({.mips = 1}).stored_levels == 1);
    CHECK(parsed({.mips = 0}).single_level());
}

TEST_CASE("a chain that stops before 1x1 is reported as short", "[texture][dds]") {
    // 512x512 DXT1 with nine of ten levels, as in SE.
    const DdsInfo info = parsed({.width = 512, .height = 512, .mips = 9});
    CHECK(info.short_chain());
    CHECK(info.stored_levels == 9);
    CHECK(info.full_chain_levels == 10);
    CHECK(info.full_chain_bytes - info.declared_bytes == 8); // one DXT1 block
}

TEST_CASE("a cubemap is six surfaces, not one", "[texture][dds]") {
    const DdsInfo info = parsed({.width = 32, .height = 32, .mips = 6, .cubemap = true});
    CHECK(info.kind == SurfaceKind::cubemap);
    CHECK(info.faces == 6);
    // Six faces of a complete 32x32 DXT1 chain: 512+128+32+8+8+8 each.
    CHECK(info.declared_bytes == k_dds_header_size + 6 * 696);
    CHECK_FALSE(info.short_chain());
}

TEST_CASE("a partial cubemap is counted by its face bits", "[texture][dds]") {
    const DdsInfo info =
        parsed({.width = 4, .height = 4, .mips = 1, .cubemap = true, .cube_faces = 3});
    CHECK(info.faces == 3);
    CHECK(info.declared_bytes == k_dds_header_size + 3 * 8);
}

TEST_CASE("a volume texture is recognised and left unsized", "[texture][dds]") {
    const DdsInfo info =
        parsed({.width = 8, .height = 8, .mips = 1, .volume = true, .depth = 8});
    CHECK(info.kind == SurfaceKind::volume);
    // Depth halves too, so no 2D size is reported.
    CHECK(info.declared_bytes == 0);
    CHECK(info.full_chain_bytes == 0);
}

TEST_CASE("an uncompressed surface is sized per pixel", "[texture][dds]") {
    const DdsInfo info = parsed({.width = 16,
                                 .height = 16,
                                 .mips = 5,
                                 .fourcc = FourCC{static_cast<std::uint32_t>(0)},
                                 .rgb_bit_count = 32});
    CHECK_FALSE(info.layout.block_compressed);
    CHECK(info.layout.unit_bytes == 4);
    CHECK(info.layout.name == "RGBA32");
    // 256 + 64 + 16 + 4 + 1 pixels, four bytes each.
    CHECK(info.declared_bytes == k_dds_header_size + 4 * (256 + 64 + 16 + 4 + 1));
}

TEST_CASE("a DX10 header names the format the FOURCC cannot", "[texture][dds]") {
    const DdsInfo info =
        parsed({.width = 8, .height = 8, .mips = 1, .dx10 = true, .dxgi_format = 98});
    CHECK(info.dx10);
    CHECK(info.layout.name == "BC7");
    CHECK(info.layout.unit_bytes == 16);
    CHECK(info.layout.block_compressed);
    CHECK(info.header_bytes == k_dds_header_size + k_dx10_header_size);
    CHECK(info.declared_bytes == info.header_bytes + 4 * 16); // 2x2 blocks
}

TEST_CASE("a DX10 cube flag makes a cubemap without any caps2 bit",
          "[texture][dds]") {
    const DdsInfo info = parsed({.width = 4,
                                 .height = 4,
                                 .mips = 1,
                                 .dx10 = true,
                                 .dxgi_format = 71,   // BC1_UNORM
                                 .dx10_misc = 0x4,    // D3D10_RESOURCE_MISC_TEXTURECUBE
                                 .dx10_unit_bytes = 8});
    CHECK(info.kind == SurfaceKind::cubemap);
    CHECK(info.faces == 6);
}

TEST_CASE("a DX10 3D dimension is a volume however caps2 reads", "[texture][dds]") {
    CHECK(parsed({.width = 4, .height = 4, .mips = 1, .dx10 = true, .dimension = 4}).kind ==
          SurfaceKind::volume);
}

// ---- error paths ------------------------------------------------------------

TEST_CASE("a file that is not a DDS is rejected by its magic", "[texture][dds]") {
    CHECK(rejected({.magic = "DDX "}) == ErrorKind::bad_magic);
}

TEST_CASE("a header lying about its own size is rejected", "[texture][dds]") {
    CHECK(rejected({.header_dwsize = 120}) == ErrorKind::bad_value);
    CHECK(rejected({.pf_dwsize = 24}) == ErrorKind::bad_value);
}

TEST_CASE("a zero or absurd dimension is rejected before any multiplication",
          "[texture][dds]") {
    CHECK(rejected({.width = 0}) == ErrorKind::bad_value);
    CHECK(rejected({.height = 0}) == ErrorKind::bad_value);
    CHECK(rejected({.width = k_max_dimension + 1, .height = 4}) == ErrorKind::too_large);
}

TEST_CASE("more mip levels than the surface can hold is rejected", "[texture][dds]") {
    // 4x4 holds three levels; eight would make every later offset wrong.
    CHECK(rejected({.width = 4, .height = 4, .mips = 8}) == ErrorKind::bad_value);
}

TEST_CASE("a format we cannot size is unsupported, not guessed", "[texture][dds]") {
    CHECK(rejected({.fourcc = FourCC{"ZZZZ"}}) == ErrorKind::unsupported);
    CHECK(rejected({.fourcc = FourCC{static_cast<std::uint32_t>(0)}, .rgb_bit_count = 4}) ==
          ErrorKind::unsupported);
    CHECK(rejected({.width = 8, .height = 8, .mips = 1, .dx10 = true, .dxgi_format = 132}) ==
          ErrorKind::unsupported);
}

TEST_CASE("a file shorter than the levels it declares is truncated",
          "[texture][dds]") {
    CHECK(rejected({.width = 64, .height = 64, .mips = 7, .truncate_by = 1}) ==
          ErrorKind::truncated);
    // The reverse: the header claims seven levels, four are stored.
    CHECK(rejected({.width = 64, .height = 64, .mips = 7, .stored_levels = 4}) ==
          ErrorKind::truncated);
    // A cubemap missing its sixth face; as one surface it would look complete.
    CHECK(rejected({.width = 32,
                    .height = 32,
                    .mips = 6,
                    .cubemap = true,
                    .cube_faces = 6,
                    .truncate_by = 696}) == ErrorKind::truncated);
}

TEST_CASE("a header cut off mid-field is truncated, not misread",
          "[texture][dds]") {
    const auto bytes = build_dds({});
    for (std::size_t keep : {std::size_t{0}, std::size_t{4}, std::size_t{40},
                             std::size_t{100}, k_dds_header_size - 1}) {
        auto info = parse_dds(std::span(bytes).first(keep), "cut.dds");
        REQUIRE_FALSE(info.has_value());
        CHECK((info.error().kind == ErrorKind::truncated ||
               info.error().kind == ErrorKind::bad_magic));
    }
}
