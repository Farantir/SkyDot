// SPDX-License-Identifier: GPL-3.0-or-later
//
// limit_size(): a texture made smaller by dropping its top mip levels. The
// fixture fills every face and level with its own byte (dds_fill), so the
// tests can tell which levels survived and in which order.
#include "bethconv/texture/mip_drop.hpp"
#include "bethconv/texture/mip_tail.hpp"

#include "../support/dds_builder.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace bethconv::texture;
using bethconv::testing::build_dds;
using bethconv::testing::dds_fill;
using bethconv::testing::DdsSpec;

namespace {

struct Limited {
    std::vector<std::byte> source;
    DdsInfo info;
    SizeLimit limit;
};

Limited run(const DdsSpec& spec, std::uint32_t max_extent) {
    Limited out;
    out.source = build_dds(spec);
    auto info = parse_dds(out.source, "fixture.dds");
    REQUIRE(info.has_value());
    out.info = *info;
    auto limit = limit_size(out.source, *info, max_extent, "fixture.dds");
    REQUIRE(limit.has_value());
    out.limit = std::move(*limit);
    return out;
}

DdsInfo reparse(const SizeLimit& limit) {
    auto info = parse_dds(limit.data, "limited.dds");
    REQUIRE(info.has_value());
    return *info;
}

/// A DXT1 chain of `mips` levels; tests change what else they need.
DdsSpec dds(std::uint32_t width, std::uint32_t height, std::uint32_t mips) {
    DdsSpec spec;
    spec.width = width;
    spec.height = height;
    spec.mips = mips;
    return spec;
}

std::uint8_t byte_at(std::span<const std::byte> bytes, std::size_t index) {
    REQUIRE(index < bytes.size());
    return static_cast<std::uint8_t>(bytes[index]);
}

} // namespace

TEST_CASE("a texture within the limit is left alone", "[texture][mipdrop]") {
    const auto r = run(dds(512, 512, 10), 512);
    CHECK(r.limit.outcome == DropOutcome::fits);
    CHECK(r.limit.data.empty());
    CHECK(run(dds(512, 512, 10), 0).limit.outcome == DropOutcome::fits);
}

TEST_CASE("a 1024 DXT1 limited to 256 keeps its 256 level and below", "[texture][mipdrop]") {
    const auto r = run(dds(1024, 1024, 11), 256);
    REQUIRE(r.limit.outcome == DropOutcome::shrunk);
    CHECK(r.limit.dropped_levels == 2);
    const auto info = reparse(r.limit);
    CHECK(info.width == 256);
    CHECK(info.height == 256);
    CHECK(info.declared_mips == 9);
    CHECK_FALSE(info.short_chain());
    CHECK(r.limit.data.size() == info.declared_bytes);
    // The first pixel bytes are the old level 2.
    CHECK(byte_at(r.limit.data, info.header_bytes) == static_cast<std::uint8_t>(dds_fill(0, 2)));
    CHECK(byte_at(r.limit.data, r.limit.data.size() - 1) ==
          static_cast<std::uint8_t>(dds_fill(0, 10)));
    CHECK(r.limit.saved_bytes == r.source.size() - r.limit.data.size());
}

TEST_CASE("an oblong texture is limited by its longer side", "[texture][mipdrop]") {
    const auto r = run(dds(2048, 512, 12), 1024);
    REQUIRE(r.limit.outcome == DropOutcome::shrunk);
    const auto info = reparse(r.limit);
    CHECK(info.width == 1024);
    CHECK(info.height == 256);
}

TEST_CASE("a set linear size is recomputed for the new top level", "[texture][mipdrop]") {
    auto source = build_dds(dds(64, 64, 7));
    source[20] = std::byte{0x00};  // dwPitchOrLinearSize = 2048, as 64x64 DXT1 has
    source[21] = std::byte{0x08};
    auto info = parse_dds(source, "pitch.dds");
    REQUIRE(info);
    auto limit = limit_size(source, *info, 16, "pitch.dds");
    REQUIRE(limit);
    REQUIRE(limit->outcome == DropOutcome::shrunk);
    // 16x16 DXT1: 16 blocks of 8 bytes.
    CHECK(byte_at(limit->data, 20) == 128);
    CHECK(byte_at(limit->data, 21) == 0);
}

TEST_CASE("an uncompressed texture shrinks too", "[texture][mipdrop]") {
    const auto r = run([] { auto s = dds(256, 256, 9); s.fourcc = {}; s.rgb_bit_count = 32; return s; }(),
                       64);
    REQUIRE(r.limit.outcome == DropOutcome::shrunk);
    const auto info = reparse(r.limit);
    CHECK(info.width == 64);
    CHECK(r.limit.data.size() == info.declared_bytes);
}

TEST_CASE("a cubemap is cut face by face", "[texture][mipdrop]") {
    const auto r = run([] { auto s = dds(128, 128, 8); s.cubemap = true; return s; }(), 32);
    REQUIRE(r.limit.outcome == DropOutcome::shrunk);
    const auto info = reparse(r.limit);
    CHECK(info.faces == 6);
    CHECK(info.width == 32);
    // Face 1 starts with its own old level 2.
    const std::size_t face = chain_bytes(info.layout, 32, 32, info.stored_levels);
    CHECK(byte_at(r.limit.data, info.header_bytes + face) ==
          static_cast<std::uint8_t>(dds_fill(1, 2)));
}

TEST_CASE("textures that cannot shrink without decoding are passed through",
          "[texture][mipdrop]") {
    CHECK(run(dds(1024, 1024, 1), 256).limit.outcome ==
          DropOutcome::single_level);
    // The chain stops at 512: no level of 256 or less is stored.
    CHECK(run(dds(1024, 1024, 2), 256)
              .limit.outcome == DropOutcome::too_few_levels);
    CHECK(run([] { auto s = dds(64, 64, 7); s.volume = true; s.depth = 4; return s; }(), 16)
              .limit.outcome == DropOutcome::unsupported);
}

TEST_CASE("a short chain is shrunk, then completed by the tail fix", "[texture][mipdrop]") {
    // As 99% of SE: the chain is declared one level short of 1x1.
    const auto r = run(dds(1024, 1024, 10), 256);
    REQUIRE(r.limit.outcome == DropOutcome::shrunk);
    const auto info = reparse(r.limit);
    CHECK(info.short_chain());
    auto fix = complete_mip_tail(r.limit.data, info, "limited.dds");
    REQUIRE(fix);
    CHECK(fix->outcome == TailOutcome::completed);
    auto done = parse_dds(fix->data, "done.dds");
    REQUIRE(done);
    CHECK(done->width == 256);
    CHECK_FALSE(done->short_chain());
}
