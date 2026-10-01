// SPDX-License-Identifier: GPL-3.0-or-later
//
// encode_uncompressed(): uncompressed DDS to BC7 or BC1. Pixels are gradients
// written over the builder's fill, so decoding the blocks back can be checked
// against what went in.
#include "bethconv/texture/bc_encode.hpp"
#include "bethconv/texture/mip_tail.hpp"

#include "../support/dds_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>

using namespace bethconv::texture;
using bethconv::testing::build_dds;
using bethconv::testing::DdsSpec;

namespace {

/// An uncompressed 32-bit (A8R8G8B8 masks, as the builder writes) texture
/// with a full chain; every level a smooth gradient with alpha `alpha`.
std::vector<std::byte> gradient(std::uint32_t size, std::uint8_t alpha, bool chain = true) {
    DdsSpec spec;
    spec.width = size;
    spec.height = size;
    spec.mips = chain ? full_chain_levels(size, size) : 1;
    spec.fourcc = {};
    spec.rgb_bit_count = 32;
    auto bytes = build_dds(spec);
    std::size_t at = k_dds_header_size;
    for (std::uint32_t level = 0; level < spec.mips; ++level) {
        const auto extent = level_extent(size, level);
        for (std::uint32_t y = 0; y < extent; ++y) {
            for (std::uint32_t x = 0; x < extent; ++x) {
                // Little-endian 0xAARRGGBB: B, G, R, A in memory.
                bytes[at++] = std::byte(static_cast<std::uint8_t>(x * 255 / extent));
                bytes[at++] = std::byte(static_cast<std::uint8_t>(y * 255 / extent));
                bytes[at++] = std::byte{0x80};
                bytes[at++] = std::byte{alpha};
            }
        }
    }
    return bytes;
}

DdsInfo parse(std::span<const std::byte> bytes) {
    auto info = parse_dds(bytes, "fixture.dds");
    REQUIRE(info.has_value());
    return *info;
}

Encoded encode(const std::vector<std::byte>& bytes, Encoding encoding, bool normal_map = false) {
    auto result = encode_uncompressed(bytes, parse(bytes), encoding, normal_map, "fixture.dds", 2);
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// Largest per-channel error of the top level's first block row, decoded.
int worst_error(const Encoded& encoded, std::uint32_t size) {
    const auto info = parse(encoded.data);
    const std::size_t block = encoded.format == "BC7" ? 16 : 8;
    int worst = 0;
    for (std::uint32_t bx = 0; bx < size / 4; ++bx) {
        std::array<std::uint8_t, 64> rgba{};
        const auto bytes = std::span(encoded.data).subspan(info.header_bytes + bx * block, block);
        REQUIRE(decode_block(encoded.format, bytes, rgba));
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const auto* p = &rgba[(y * 4 + x) * 4];
                const int px = static_cast<int>((bx * 4 + x) * 255 / size);
                const int py = static_cast<int>(y * 255 / size);
                worst = std::max({worst, std::abs(p[0] - 0x80), std::abs(p[1] - py),
                                  std::abs(p[2] - px)});
            }
        }
    }
    return worst;
}

} // namespace

TEST_CASE("an uncompressed texture becomes BC7 with its whole chain", "[texture][bcencode]") {
    const auto source = gradient(64, 255);
    const auto encoded = encode(source, Encoding::bc7);
    REQUIRE(encoded.outcome == EncodeOutcome::encoded);
    CHECK(encoded.format == "BC7");
    const auto info = parse(encoded.data);
    CHECK(info.dx10);
    CHECK(info.layout.dxgi_format == 98);
    CHECK(info.width == 64);
    CHECK(info.stored_levels == 7);
    CHECK(encoded.data.size() == info.declared_bytes);
    CHECK(encoded.data.size() * 3 < source.size());  // 8 of 32 bits per pixel, plus headers
    CHECK(worst_error(encoded, 64) <= 8);
}

TEST_CASE("compact picks BC1 for opaque colour, BC7 otherwise", "[texture][bcencode]") {
    const auto opaque = encode(gradient(32, 255), Encoding::compact);
    REQUIRE(opaque.outcome == EncodeOutcome::encoded);
    CHECK(opaque.format == "BC1");
    CHECK(parse(opaque.data).layout.fourcc == bethconv::io::FourCC{"DXT1"});
    CHECK(worst_error(opaque, 32) <= 16);

    CHECK(encode(gradient(32, 128), Encoding::compact).format == "BC7");
    CHECK(encode(gradient(32, 255), Encoding::compact, true).format == "BC7");
}

TEST_CASE("levels below one block and odd sizes are padded, not refused", "[texture][bcencode]") {
    // 6x6 has levels 6, 3, 1: none a multiple of 4.
    DdsSpec spec;
    spec.width = 6;
    spec.height = 6;
    spec.mips = 3;
    spec.fourcc = {};
    spec.rgb_bit_count = 32;
    const auto source = build_dds(spec);
    const auto encoded = encode(source, Encoding::bc7);
    REQUIRE(encoded.outcome == EncodeOutcome::encoded);
    const auto info = parse(encoded.data);
    CHECK(info.width == 6);
    CHECK(encoded.data.size() == info.declared_bytes);
}

TEST_CASE("24-bit and luminance layouts are read through their masks", "[texture][bcencode]") {
    DdsSpec rgb;
    rgb.width = rgb.height = 8;
    rgb.fourcc = {};
    rgb.rgb_bit_count = 24;
    CHECK(encode(build_dds(rgb), Encoding::compact).outcome == EncodeOutcome::encoded);

    DdsSpec grey;
    grey.width = grey.height = 8;
    grey.fourcc = {};
    grey.rgb_bit_count = 8;
    grey.pf_flags = 0x20000;  // DDPF_LUMINANCE
    const auto encoded = encode(build_dds(grey), Encoding::bc7);
    REQUIRE(encoded.outcome == EncodeOutcome::encoded);
    std::array<std::uint8_t, 64> rgba{};
    REQUIRE(decode_block("BC7", std::span(encoded.data).subspan(parse(encoded.data).header_bytes, 16),
                         rgba));
    CHECK(rgba[0] == rgba[1]);
    CHECK(rgba[1] == rgba[2]);
}

TEST_CASE("block formats, cubemaps and keep are passed through", "[texture][bcencode]") {
    DdsSpec dxt1;
    dxt1.width = dxt1.height = 16;
    CHECK(encode(build_dds(dxt1), Encoding::bc7).outcome == EncodeOutcome::already_compressed);
    CHECK(encode(gradient(16, 255), Encoding::keep).outcome == EncodeOutcome::already_compressed);

    DdsSpec cube;
    cube.width = cube.height = 16;
    cube.fourcc = {};
    cube.rgb_bit_count = 32;
    cube.cubemap = true;
    const auto encoded = encode(build_dds(cube), Encoding::bc7);
    CHECK(encoded.outcome == EncodeOutcome::unsupported);
    CHECK_FALSE(encoded.reason.empty());
}

TEST_CASE("an encoded short chain is completed by the tail fix", "[texture][bcencode]") {
    DdsSpec spec;
    spec.width = spec.height = 64;
    spec.mips = 5;  // one short of 64 -> 4, as SE writes them
    spec.fourcc = {};
    spec.rgb_bit_count = 32;
    const auto encoded = encode(build_dds(spec), Encoding::bc7);
    REQUIRE(encoded.outcome == EncodeOutcome::encoded);
    const auto info = parse(encoded.data);
    REQUIRE(info.short_chain());
    auto fix = complete_mip_tail(encoded.data, info, "fixture.dds");
    REQUIRE(fix);
    CHECK(fix->outcome == TailOutcome::completed);
}
