// SPDX-License-Identifier: GPL-3.0-or-later
//
// Coverage-preserving alpha mips on synthetic textures: mip chains whose upper
// levels thin out (the alpha average falls below the test threshold) and must
// come back to level 0's coverage, in every format the pass handles.
#include "bethconv/texture/alpha_coverage.hpp"

#include "../support/dds_builder.hpp"
#include "bethconv/texture/bc_encode.hpp"
#include "bethconv/texture/dds.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>

using bethconv::testing::DdsSpec;
using bethconv::testing::build_dds;
using namespace bethconv::texture;

namespace {

constexpr std::uint32_t k_threshold = 128;

/// Alpha of texel (x, y) of `level`, for a 16x16 chain: half the texels opaque
/// in level 0; the lower levels as the average thinned them.
std::uint8_t thinned(std::uint32_t level, std::uint32_t x, std::uint32_t y) {
    switch (level) {
    case 0: return x < 8 ? 255 : 0;
    case 1: return y < 4 ? 90 : 0;  // half the texels, all below the threshold
    case 2: return y < 2 ? 60 : 0;
    case 3: return y < 1 ? 40 : 0;
    default: return 0;
    }
}

/// RGBA32 file (the builder's default masks, alpha in byte 3) of a 16x16
/// five-level chain.
std::vector<std::byte> rgba_file() {
    auto file = build_dds({.width = 16, .height = 16, .mips = 5, .fourcc = {}, .rgb_bit_count = 32});
    std::size_t at = bethconv::texture::k_dds_header_size;
    for (std::uint32_t level = 0; level < 5; ++level) {
        const std::uint32_t size = 16U >> level;
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                file[at + 0] = std::byte{200};
                file[at + 1] = std::byte{120};
                file[at + 2] = std::byte{40};
                file[at + 3] = static_cast<std::byte>(thinned(level, x, y));
                at += 4;
            }
        }
    }
    return file;
}

/// DXT5 file of a 16x16 chain down to 4x4 whose blocks are uniform in alpha:
/// level 0 has half its blocks opaque, level 1 half at 90, level 2 one block.
std::vector<std::byte> dxt5_file(std::uint32_t mips) {
    auto file = build_dds({.width = 16, .height = 16, .mips = mips, .fourcc = {"DXT5"}});
    std::size_t at = bethconv::texture::k_dds_header_size;
    for (std::uint32_t level = 0; level < mips; ++level) {
        const std::uint32_t blocks = std::max(1U, (16U >> level) / 4);
        for (std::uint32_t by = 0; by < blocks; ++by) {
            for (std::uint32_t bx = 0; bx < blocks; ++bx) {
                std::uint8_t alpha = 0;
                if (level == 0) {
                    alpha = bx < 2 ? 255 : 0;
                } else if (level == 1) {
                    alpha = by < 1 ? 90 : 0;
                }
                for (std::size_t i = 0; i < 16; ++i) {
                    file[at + i] = std::byte{0};
                }
                file[at + 0] = static_cast<std::byte>(alpha);
                file[at + 1] = static_cast<std::byte>(alpha);
                file[at + 8] = std::byte{0x55}; // some colour block
                at += 16;
            }
        }
    }
    return file;
}

CoverageFix run(const std::vector<std::byte>& file, std::uint32_t threshold = k_threshold,
                CoverageMode mode = CoverageMode::exact) {
    auto info = parse_dds(file, "t.dds");
    REQUIRE(info.has_value());
    auto fix = preserve_alpha_coverage(file, *info, threshold, "t.dds", mode);
    REQUIRE(fix.has_value());
    return std::move(*fix);
}

} // namespace

TEST_CASE("thinned RGBA mips get their level 0 coverage back", "[texture][coverage]") {
    const auto file = rgba_file();
    const CoverageFix fix = run(file);
    REQUIRE(fix.outcome == CoverageOutcome::adjusted);
    REQUIRE(fix.before.size() == 5);
    CHECK(fix.before[0] == Catch::Approx(0.5));
    CHECK(fix.before[1] == Catch::Approx(0.0));
    CHECK(fix.before[2] == Catch::Approx(0.0));
    CHECK(fix.after[1] == Catch::Approx(0.5));
    CHECK(fix.after[2] == Catch::Approx(0.5));
    CHECK(fix.after[3] == Catch::Approx(0.5));
    CHECK(fix.scale[1] == Catch::Approx(128.0 / 90.0).margin(0.01));

    // Re-measured from the bytes of the new file, not from the pass's report.
    auto info = parse_dds(fix.data, "n.dds");
    REQUIRE(info.has_value());
    auto measured = measure_alpha_coverage(fix.data, *info, k_threshold, "n.dds");
    REQUIRE(measured.has_value());
    REQUIRE(measured->size() == 5);
    CHECK((*measured)[1] == Catch::Approx(0.5));
    CHECK((*measured)[2] == Catch::Approx(0.5));
    // The file keeps its size and colour.
    CHECK(fix.data.size() == file.size());
    CHECK(fix.data[bethconv::texture::k_dds_header_size + 16 * 16 * 4] == std::byte{200});
}

TEST_CASE("level 0 and colour are never touched", "[texture][coverage]") {
    const auto file = rgba_file();
    const CoverageFix fix = run(file);
    REQUIRE(fix.outcome == CoverageOutcome::adjusted);
    CHECK(fix.scale[0] == 1.0F);
    CHECK(std::equal(file.begin(), file.begin() + 128 + 16 * 16 * 4, fix.data.begin()));
}

TEST_CASE("a DXT5 chain is fixed by its alpha blocks alone", "[texture][coverage]") {
    const auto file = dxt5_file(3);
    const CoverageFix fix = run(file);
    REQUIRE(fix.outcome == CoverageOutcome::adjusted);
    CHECK(fix.before[0] == Catch::Approx(0.5));
    CHECK(fix.before[1] == Catch::Approx(0.0));
    CHECK(fix.after[1] == Catch::Approx(0.5).margin(0.05));
    REQUIRE(fix.data.size() == file.size());
    // The colour half of every block is as it was.
    std::size_t at = k_dds_header_size;
    for (std::size_t block = 0; block < 16 + 4 + 1; ++block, at += 16) {
        CHECK(std::equal(file.begin() + static_cast<std::ptrdiff_t>(at + 8),
                         file.begin() + static_cast<std::ptrdiff_t>(at + 16),
                         fix.data.begin() + static_cast<std::ptrdiff_t>(at + 8)));
    }
}

TEST_CASE("a DXT3 chain is requantized in place", "[texture][coverage]") {
    auto file = dxt5_file(2);
    // Same pattern as the DXT5 fixture, written as 4-bit alpha: header says DXT3.
    file[84] = std::byte{'D'};
    file[85] = std::byte{'X'};
    file[86] = std::byte{'T'};
    file[87] = std::byte{'3'};
    // Uniform nibbles from the DXT5 byte pair would be wrong; rewrite blocks.
    std::size_t at = k_dds_header_size;
    for (std::uint32_t level = 0; level < 2; ++level) {
        const std::uint32_t blocks = (16U >> level) / 4;
        for (std::uint32_t by = 0; by < blocks; ++by) {
            for (std::uint32_t bx = 0; bx < blocks; ++bx, at += 16) {
                std::uint8_t nibble = 0;
                if (level == 0) {
                    nibble = bx < 2 ? 15 : 0;
                } else {
                    nibble = by < 1 ? 5 : 0; // 85 of 255: below 128
                }
                for (std::size_t i = 0; i < 8; ++i) {
                    file[at + i] = static_cast<std::byte>(nibble | (nibble << 4));
                }
            }
        }
    }
    const CoverageFix fix = run(file);
    REQUIRE(fix.outcome == CoverageOutcome::adjusted);
    CHECK(fix.before[1] == Catch::Approx(0.0));
    CHECK(fix.after[1] == Catch::Approx(0.5));
}

TEST_CASE("a BC7 chain is decoded, scaled and encoded again", "[texture][coverage]") {
    const auto source = rgba_file();
    auto info = parse_dds(source, "s.dds");
    REQUIRE(info.has_value());
    auto encoded = encode_uncompressed(source, *info, Encoding::bc7, false, "s.dds", 1);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->outcome == EncodeOutcome::encoded);
    const CoverageFix fix = run(encoded->data);
    REQUIRE(fix.outcome == CoverageOutcome::adjusted);
    CHECK(fix.before[1] < 0.05);
    CHECK(fix.after[1] == Catch::Approx(0.5).margin(0.1));
    CHECK(fix.data.size() == encoded->data.size());
}

TEST_CASE("coverage that holds, or cannot be kept, leaves the file alone", "[texture][coverage]") {
    SECTION("an opaque texture") {
        auto file = rgba_file();
        std::size_t at = k_dds_header_size;
        for (std::uint32_t level = 0; level < 5; ++level) {
            for (std::uint32_t i = 0; i < (16U >> level) * (16U >> level); ++i, at += 4) {
                file[at + 3] = std::byte{255};
            }
        }
        CHECK(run(file).outcome == CoverageOutcome::unchanged);
    }
    SECTION("a single level") {
        const auto file = build_dds({.width = 16, .height = 16, .mips = 1, .fourcc = {}, .rgb_bit_count = 32});
        CHECK(run(file).outcome == CoverageOutcome::single_level);
    }
    SECTION("one-bit alpha") {
        const auto file = build_dds({.width = 16, .height = 16, .mips = 5, .fourcc = {"DXT1"}});
        CHECK(run(file).outcome == CoverageOutcome::unsupported);
    }
    SECTION("a cubemap") {
        const auto file = build_dds({.width = 16, .height = 16, .mips = 5, .fourcc = {"DXT5"}, .cubemap = true});
        CHECK(run(file).outcome == CoverageOutcome::unsupported);
    }
    SECTION("no threshold") {
        CHECK(run(rgba_file(), 0).outcome == CoverageOutcome::unsupported);
    }
}

TEST_CASE("a truncated file is an error, not a crash", "[texture][coverage]") {
    auto file = rgba_file();
    auto info = parse_dds(file, "t.dds");
    REQUIRE(info.has_value());
    file.resize(file.size() - 40);
    auto fix = preserve_alpha_coverage(file, *info, k_threshold, "t.dds");
    CHECK_FALSE(fix.has_value());
}

namespace {

/// Like `rgba_file`, but level 1 is thicker than level 0: three quarters of its
/// texels are at or above the threshold, against half in level 0.
std::vector<std::byte> thickened_rgba_file() {
    auto file = rgba_file();
    std::size_t at = k_dds_header_size + 16 * 16 * 4;
    for (std::uint32_t i = 0; i < 8 * 8; ++i, at += 4) {
        constexpr std::uint8_t alphas[4] = {255, 200, 150, 0};
        file[at + 3] = static_cast<std::byte>(alphas[i % 4]);
    }
    return file;
}

} // namespace

TEST_CASE("exact lowers a thickened level, floor leaves it", "[texture][coverage]") {
    const auto file = thickened_rgba_file();
    const CoverageFix exact = run(file);
    REQUIRE(exact.outcome == CoverageOutcome::adjusted);
    CHECK(exact.before[1] == Catch::Approx(0.75));
    CHECK(exact.lowered);
    CHECK(exact.after[1] == Catch::Approx(0.5).margin(0.02));

    const CoverageFix floor = run(file, k_threshold, CoverageMode::floor);
    // Level 1 stays; the thinned levels below it are still raised.
    CHECK(floor.scale[1] == 1.0F);
    CHECK(floor.after[1] == Catch::Approx(0.75));
    CHECK_FALSE(floor.lowered);
}

TEST_CASE("floor raises a thinned level like exact does", "[texture][coverage]") {
    const auto file = rgba_file();
    const CoverageFix floor = run(file, k_threshold, CoverageMode::floor);
    REQUIRE(floor.outcome == CoverageOutcome::adjusted);
    CHECK(floor.raised);
    CHECK_FALSE(floor.lowered);
    CHECK(floor.after[1] == Catch::Approx(0.5));
    CHECK(floor.after[2] == Catch::Approx(0.5));
}

TEST_CASE("floor on chains that only thicken changes nothing", "[texture][coverage]") {
    auto file = rgba_file();
    // Levels 2..4 clear as well: nothing thins, level 1 thickens.
    std::size_t at = k_dds_header_size + 16 * 16 * 4;
    for (std::uint32_t i = 0; i < 8 * 8; ++i, at += 4) {
        file[at + 3] = std::byte{255};
    }
    for (std::uint32_t level = 2; level < 5; ++level) {
        for (std::uint32_t i = 0; i < (16U >> level) * (16U >> level); ++i, at += 4) {
            file[at + 3] = i % 2 == 0 ? std::byte{255} : std::byte{0};
        }
    }
    const CoverageFix floor = run(file, k_threshold, CoverageMode::floor);
    CHECK(floor.outcome == CoverageOutcome::unchanged);
    CHECK(floor.data.empty());
}
