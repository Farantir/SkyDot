// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mip-tail fix, including the case the Python prototype gets wrong: it
// appends one tail at the end of the file, which for a cubemap leaves every
// face short and makes dwMipMapCount wrong for all six. The cubemap tests fail
// if the rebuild is ever flattened into one pass.
#include "bethconv/texture/mip_tail.hpp"

#include "../support/dds_builder.hpp"
#include "bethconv/texture/dds.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>

using bethconv::io::FourCC;
using bethconv::testing::DdsSpec;
using bethconv::testing::build_dds;
using bethconv::testing::dds_fill;
using namespace bethconv::texture;

namespace {

struct Fixed {
    std::vector<std::byte> source;
    DdsInfo info;
    TailFix fix;
};

Fixed run(const DdsSpec& spec) {
    Fixed result;
    result.source = build_dds(spec);
    auto info = parse_dds(result.source, "fixture.dds");
    REQUIRE(info.has_value());
    result.info = *info;
    auto fix = complete_mip_tail(result.source, *info, "fixture.dds");
    REQUIRE(fix.has_value());
    result.fix = std::move(*fix);
    return result;
}

/// Parse the rebuilt file from its own bytes, as a consumer would.
DdsInfo reparse(const TailFix& fix) {
    auto info = parse_dds(fix.data, "fixed.dds");
    REQUIRE(info.has_value());
    return *info;
}

std::uint8_t byte_at(std::span<const std::byte> bytes, std::size_t index) {
    REQUIRE(index < bytes.size());
    return static_cast<std::uint8_t>(bytes[index]);
}

} // namespace

TEST_CASE("a complete chain is left exactly alone", "[texture][miptail]") {
    const Fixed fixed = run({.width = 32, .height = 32, .mips = 6});
    CHECK(fixed.fix.outcome == TailOutcome::already_complete);
    CHECK(fixed.fix.data.empty());
    CHECK(fixed.fix.added_bytes == 0);
}

TEST_CASE("a file with no chain is left alone, because Godot accepts it",
          "[texture][miptail]") {
    CHECK(run({.width = 64, .height = 64, .mips = 1}).fix.outcome == TailOutcome::single_level);
    CHECK(run({.width = 64, .height = 64, .mips = 0}).fix.outcome == TailOutcome::single_level);
}

TEST_CASE("a volume texture is refused rather than resized", "[texture][miptail]") {
    const Fixed fixed =
        run({.width = 16, .height = 16, .mips = 3, .volume = true, .depth = 16});
    CHECK(fixed.fix.outcome == TailOutcome::unsupported);
    CHECK(fixed.fix.data.empty());
}

TEST_CASE("a texture array is refused rather than resized", "[texture][miptail]") {
    const Fixed fixed = run({.width = 8,
                             .height = 8,
                             .mips = 2,
                             .dx10 = true,
                             .dxgi_format = 98,
                             .array_size = 4});
    CHECK(fixed.fix.outcome == TailOutcome::unsupported);
}

TEST_CASE("512x512 DXT1 short by one level", "[texture][miptail]") {
    const Fixed fixed = run({.width = 512, .height = 512, .mips = 9});
    REQUIRE(fixed.fix.outcome == TailOutcome::completed);
    CHECK(fixed.fix.from_levels == 9);
    CHECK(fixed.fix.to_levels == 10);
    CHECK(fixed.fix.added_bytes == 8); // one DXT1 block
    CHECK(fixed.fix.data.size() == fixed.source.size() + 8);

    const DdsInfo after = reparse(fixed.fix);
    CHECK(after.declared_mips == 10);
    CHECK_FALSE(after.short_chain());
    // Godot's expected size now equals the file size.
    CHECK(after.declared_bytes == fixed.fix.data.size());
    CHECK(after.declared_bytes == after.full_chain_bytes);
}

TEST_CASE("everything before the new tail is byte-identical", "[texture][miptail]") {
    const Fixed fixed = run({.width = 256, .height = 32, .mips = 5});
    REQUIRE(fixed.fix.outcome == TailOutcome::completed);
    for (std::size_t i = 0; i < fixed.source.size(); ++i) {
        if (i >= k_mipmap_count_offset && i < k_mipmap_count_offset + 4) {
            continue; // the four bytes the fix may change
        }
        REQUIRE(fixed.source[i] == fixed.fix.data[i]);
    }
    CHECK(byte_at(fixed.fix.data, k_mipmap_count_offset) == 9);
}

TEST_CASE("an oblong surface gets every missing level, not just the last",
          "[texture][miptail]") {
    // 256x32 DXT5 with five levels: 16x2, 8x1, 4x1, 2x1, 1x1 are missing; the
    // first is four blocks wide.
    const Fixed fixed = run({.width = 256, .height = 32, .mips = 5, .fourcc = FourCC{"DXT5"}});
    REQUIRE(fixed.fix.outcome == TailOutcome::completed);
    CHECK(fixed.fix.to_levels == 9);
    CHECK(fixed.fix.added_bytes == 16 * (2 + 1 + 1 + 1)); // 8x1 is two blocks
    const DdsInfo after = reparse(fixed.fix);
    CHECK(after.declared_bytes == fixed.fix.data.size());
}

TEST_CASE("the appended levels repeat the last level that was stored",
          "[texture][miptail]") {
    const Fixed fixed = run({.width = 64, .height = 64, .mips = 5});
    REQUIRE(fixed.fix.outcome == TailOutcome::completed);
    // Level 4 (4x4) is the last stored; levels 5 and 6 must repeat its bytes,
    // not zeroes (a black dot at the bottom of every chain).
    const auto expected = static_cast<std::uint8_t>(dds_fill(0, 4));
    for (std::size_t i = fixed.source.size(); i < fixed.fix.data.size(); ++i) {
        REQUIRE(byte_at(fixed.fix.data, i) == expected);
    }
}

TEST_CASE("an uncompressed surface repeats one pixel, not one level",
          "[texture][miptail]") {
    const Fixed fixed = run({.width = 16,
                             .height = 16,
                             .mips = 3,
                             .fourcc = FourCC{static_cast<std::uint32_t>(0)},
                             .rgb_bit_count = 32});
    REQUIRE(fixed.fix.outcome == TailOutcome::completed);
    // Three of five levels stored: 2x2 and 1x1 missing, 4 bytes per pixel.
    CHECK(fixed.fix.added_bytes == 4 * (4 + 1));
    const DdsInfo after = reparse(fixed.fix);
    CHECK(after.declared_bytes == fixed.fix.data.size());
}

TEST_CASE("a cubemap's tail goes in behind its own face", "[texture][miptail]") {
    // Six faces of a 32x32 DXT1 chain ending at 4x4: two levels missing per
    // face.
    const Fixed fixed =
        run({.width = 32, .height = 32, .mips = 4, .cubemap = true, .cube_faces = 6});
    REQUIRE(fixed.fix.outcome == TailOutcome::completed);
    CHECK(fixed.fix.to_levels == 6);
    CHECK(fixed.fix.added_bytes == 6 * (8 + 8));

    const DdsInfo after = reparse(fixed.fix);
    CHECK(after.faces == 6);
    CHECK(after.declared_mips == 6);
    CHECK(after.declared_bytes == fixed.fix.data.size());
    CHECK_FALSE(after.short_chain());

    // Each face's new levels must carry that face's color; a flattened
    // implementation fails this.
    const std::size_t face_stride = (512 + 128 + 32 + 8 + 8 + 8);
    for (std::uint32_t face = 0; face < 6; ++face) {
        const std::size_t face_start = k_dds_header_size + face * face_stride;
        const auto own = static_cast<std::uint8_t>(dds_fill(face, 3));
        // Level 3 (4x4) is the last stored one; levels 4 and 5 follow it.
        const std::size_t tail_start = face_start + 512 + 128 + 32;
        for (std::size_t i = tail_start; i < tail_start + 8 + 8 + 8; ++i) {
            REQUIRE(byte_at(fixed.fix.data, i) == own);
        }
    }
}

TEST_CASE("a partial cubemap is rebuilt with the faces it actually has",
          "[texture][miptail]") {
    const Fixed fixed = run(
        {.width = 16, .height = 16, .mips = 3, .cubemap = true, .cube_faces = 2});
    REQUIRE(fixed.fix.outcome == TailOutcome::completed);
    CHECK(fixed.fix.added_bytes == 2 * (8 + 8)); // two faces, two levels each
    CHECK(reparse(fixed.fix).declared_bytes == fixed.fix.data.size());
}

TEST_CASE("bytes past the declared surfaces are dropped and counted",
          "[texture][miptail]") {
    const Fixed fixed = run({.width = 64, .height = 64, .mips = 5, .trailing_bytes = 32});
    REQUIRE(fixed.fix.outcome == TailOutcome::completed);
    CHECK(fixed.fix.dropped_bytes == 32);
    CHECK(reparse(fixed.fix).declared_bytes == fixed.fix.data.size());
}

TEST_CASE("the fixup is idempotent", "[texture][miptail]") {
    const Fixed once = run({.width = 128, .height = 64, .mips = 6});
    REQUIRE(once.fix.outcome == TailOutcome::completed);
    const DdsInfo after = reparse(once.fix);
    auto twice = complete_mip_tail(once.fix.data, after, "fixed.dds");
    REQUIRE(twice.has_value());
    CHECK(twice->outcome == TailOutcome::already_complete);
}
