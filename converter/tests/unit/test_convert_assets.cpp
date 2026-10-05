// SPDX-License-Identifier: GPL-3.0-or-later
//
// What `convert` does with one input of each kind: the texture size limit and
// encoding (their counters and warnings), a file the converter cannot read, and
// what ends up in the asset store. test_convert.cpp covers the work list and
// the pack; this covers each conversion's own outcomes. Files are synthetic and
// the load order is empty, so no records are written.
#include "bethconv/archive/archive_set.hpp"
#include "bethconv/pack/asset_store.hpp"
#include "bethconv/pack/convert.hpp"
#include "bethconv/pack/vpath_index.hpp"
#include "bethconv/record/load_order.hpp"

#include "../support/dds_builder.hpp"
#include "../support/lod_builder.hpp"
#include "../support/pex_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using bethconv::test::TempDir;
using bethconv::testing::DdsSpec;
using namespace bethconv::pack;

namespace {

using Bytes = std::vector<std::byte>;

[[nodiscard]] Bytes junk() {
    const std::string_view text = "this is not a game file";
    Bytes bytes;
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

/// Loose files under one directory, mounted, converted without the merge.
struct Run {
    TempDir dir;
    TempDir out;
    ConvertOptions options;

    Run() {
        options.out = out / "pack";
        options.converter = "bethconv-test";
        options.write_world = false;
    }

    void add(const std::string& vpath, const Bytes& bytes) const {
        const auto path = dir / vpath;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] ConvertResult go() const {
        bethconv::archive::ArchiveSet set;
        REQUIRE(set.mount_loose(dir.path(), 0).has_value());
        auto result = convert(set, bethconv::record::LoadOrder{}, options);
        REQUIRE(result.has_value());
        return std::move(*result);
    }

    [[nodiscard]] std::filesystem::path pack() const { return options.out; }

    [[nodiscard]] nlohmann::json report() const {
        std::ifstream in(pack() / "report.json");
        REQUIRE(in.good());
        return nlohmann::json::parse(in);
    }

    /// The bytes stored for `vpath`.
    [[nodiscard]] Bytes stored(std::string_view vpath) const {
        const auto index = VpathIndex::read(pack() / "vpath.idx");
        REQUIRE(index.has_value());
        const auto* entry = index->find(vpath);
        REQUIRE(entry != nullptr);
        const auto assets = AssetReader::open(pack());
        REQUIRE(assets.has_value());
        const auto bytes = assets->read(*entry);
        REQUIRE(bytes.has_value());
        return {bytes->data.begin(), bytes->data.end()};
    }
};

/// The warnings the report holds for `vpath`.
[[nodiscard]] std::vector<std::string> warnings_for(const nlohmann::json& report,
                                                    std::string_view vpath) {
    std::vector<std::string> details;
    for (const auto& warning : report["warnings"]) {
        if (warning["vpath"] == vpath) {
            details.push_back(warning["detail"]);
        }
    }
    return details;
}

constexpr std::size_t k_dds_header = 128; // magic and the 124-byte header

} // namespace

TEST_CASE("a texture over the size limit loses its top levels, the others pass through",
          "[convert][texture]") {
    Run run;
    // DXT1: 8 bytes per 4x4 block. 64x64 with a full chain of seven levels.
    const auto big = bethconv::testing::build_dds(DdsSpec{.width = 64, .height = 64, .mips = 7});
    const auto small = bethconv::testing::build_dds(DdsSpec{.width = 8, .height = 8, .mips = 4});
    // Over the limit and no smaller level to take.
    const auto single = bethconv::testing::build_dds(DdsSpec{.width = 64, .height = 64, .mips = 1});
    run.add("textures/big.dds", big);
    run.add("textures/small.dds", small);
    run.add("textures/single.dds", single);
    run.options.max_texture_size = 16;

    const auto result = run.go();
    CHECK(result.pack.failed == 0);
    CHECK(result.textures_shrunk == 1);
    CHECK(result.textures_kept_large == 1);
    // The 64 and 32 px levels: 16 * 16 blocks and 8 * 8 blocks, 8 bytes each.
    CHECK(result.texture_bytes_saved == 16 * 16 * 8 + 8 * 8 * 8);
    CHECK(result.textures_encoded == 0);
    CHECK(result.textures_not_encoded == 0);

    // 16, 8, 4, 2 and 1 px: 16 + 4 + 1 + 1 + 1 blocks.
    CHECK(run.stored("textures/big.dds").size() == k_dds_header + (16 + 4 + 1 + 1 + 1) * 8);
    CHECK(run.stored("textures/small.dds") == small);
    CHECK(run.stored("textures/single.dds") == single);

    const auto report = run.report();
    CHECK(report["warnings"].size() == 1);
    const auto kept = warnings_for(report, "textures/single.dds");
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].starts_with("kept at 64x64, over the 16 px limit ("));
}

TEST_CASE("a short mip chain is completed and the added bytes are what is stored",
          "[convert][texture]") {
    Run run;
    // 8x8 DXT1 with levels 8 and 4 only: 2 and 1 px are missing.
    const auto short_chain = bethconv::testing::build_dds(DdsSpec{.width = 8, .height = 8, .mips = 2});
    run.add("textures/short.dds", short_chain);

    const auto result = run.go();
    CHECK(result.pack.failed == 0);
    const auto completed = run.stored("textures/short.dds");
    // Levels 8, 4, 2 and 1 px: 4 + 1 + 1 + 1 blocks of 8 bytes.
    CHECK(completed.size() == k_dds_header + (4 + 1 + 1 + 1) * 8);
    CHECK(completed.size() > short_chain.size());

    SECTION("with the fix off the file is stored as it came") {
        Run control;
        control.add("textures/short.dds", short_chain);
        control.options.fix_mip_tail = false;
        const auto untouched = control.go();
        CHECK(untouched.pack.failed == 0);
        CHECK(control.stored("textures/short.dds") == short_chain);
    }
}

TEST_CASE("bytes past the declared surfaces are dropped with the chain completed, and reported",
          "[convert][texture]") {
    Run run;
    // Levels 8 and 4 of an 8x8 texture, then five bytes that belong to nothing.
    run.add("textures/extra.dds",
            bethconv::testing::build_dds(DdsSpec{.width = 8, .height = 8, .mips = 2,
                                                  .trailing_bytes = 5}));
    const auto result = run.go();
    CHECK(result.pack.failed == 0);
    CHECK(run.stored("textures/extra.dds").size() == k_dds_header + (4 + 1 + 1 + 1) * 8);
    const auto dropped = warnings_for(run.report(), "textures/extra.dds");
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0] == "5 bytes past the declared surfaces were dropped");
}

TEST_CASE("uncompressed textures are block-compressed on request, others are left and reported",
          "[convert][texture]") {
    Run run;
    // 32-bit RGBA with a full chain; a cubemap of the same, which is not encoded.
    const auto plain = bethconv::testing::build_dds(
        DdsSpec{.width = 16, .height = 16, .mips = 5, .fourcc = bethconv::io::FourCC{},
                .rgb_bit_count = 32});
    const auto cube = bethconv::testing::build_dds(
        DdsSpec{.width = 8, .height = 8, .mips = 4, .fourcc = bethconv::io::FourCC{},
                .rgb_bit_count = 32, .cubemap = true});
    const auto compressed = bethconv::testing::build_dds(DdsSpec{.width = 8, .height = 8, .mips = 4});
    run.add("textures/plain.dds", plain);
    run.add("textures/cube.dds", cube);
    run.add("textures/compressed.dds", compressed);
    run.options.texture_encoding = bethconv::texture::Encoding::bc7;

    const auto result = run.go();
    CHECK(result.pack.failed == 0);
    CHECK(result.textures_encoded == 1);
    CHECK(result.textures_not_encoded == 1);
    CHECK(result.texture_bytes_saved > 0);

    // BC7: 16 bytes per 4x4 block, and a DX10 header (20 bytes) after the 128.
    CHECK(run.stored("textures/plain.dds").size() == k_dds_header + 20 + (16 + 4 + 1 + 1 + 1) * 16);
    CHECK(run.stored("textures/cube.dds") == cube);
    CHECK(run.stored("textures/compressed.dds") == compressed);

    const auto left = warnings_for(run.report(), "textures/cube.dds");
    REQUIRE(left.size() == 1);
    CHECK(left[0].starts_with("left uncompressed: "));
    CHECK(warnings_for(run.report(), "textures/plain.dds").empty());
}

TEST_CASE("a file that cannot be converted is a failure with its stage, not an abort",
          "[convert]") {
    Run run;
    run.add("meshes/bad.nif", junk());
    run.add("textures/bad.dds", junk());
    run.add("scripts/bad.pex", junk());
    run.add("meshes/actors/bad.hkx", junk());
    run.add("lodsettings/bad.lod", junk());
    // The good ones around them are converted.
    run.add("textures/good.dds", bethconv::testing::build_dds(DdsSpec{.width = 8, .height = 8, .mips = 4}));
    run.add("scripts/good.pex", bethconv::testing::build_script(bethconv::testing::empty_script("Good")));

    const auto result = run.go();
    CHECK(result.pack.failed == 5);
    CHECK(result.pack.converted == 2);
    CHECK(result.first_failures.size() == 5);

    std::vector<std::pair<std::string, std::string>> failed; // vpath, stage
    for (const auto& failure : run.report()["failures"]) {
        failed.emplace_back(failure["vpath"], failure["stage"]);
    }
    CHECK(failed == std::vector<std::pair<std::string, std::string>>{
                        {"lodsettings/bad.lod", "lod"},
                        {"meshes/actors/bad.hkx", "animation"},
                        {"meshes/bad.nif", "mesh"},
                        {"scripts/bad.pex", "script"},
                        {"textures/bad.dds", "texture"}});
}

TEST_CASE("a script compiled for another game is reported, not packed", "[convert][script]") {
    Run run;
    bethconv::testing::PexSpec spec;
    spec.little_endian = true;
    spec.game_id = 2;
    run.add("scripts/fallout.pex", bethconv::testing::build_pex(spec));

    const auto result = run.go();
    CHECK(result.pack.scripts == 0);
    REQUIRE(result.pack.failed == 1);
    REQUIRE(result.first_failures.size() == 1);
    const auto& failure = result.first_failures[0];
    CHECK(failure.vpath == "scripts/fallout.pex");
    CHECK(failure.stage == "script");
    CHECK(failure.kind == "unsupported");
    CHECK(failure.detail == "compiled for fallout4 (gameID 2), not skyrim");
}

TEST_CASE("bytes after a tree LOD file's blocks are skipped and reported", "[convert][lod]") {
    Run run;
    auto blocks = bethconv::testing::build_tree_blocks({{0, {bethconv::testing::TreeSpec{}}}});
    blocks.insert(blocks.end(), 3, std::byte{0});
    run.add("meshes/terrain/world/trees/world.4.0.0.btt", blocks);

    const auto result = run.go();
    CHECK(result.pack.failed == 0);
    CHECK(result.pack.lod == 1);
    const auto skipped = warnings_for(run.report(), "meshes/terrain/world/trees/world.4.0.0.btt");
    REQUIRE(skipped.size() == 1);
    CHECK(skipped[0] == "3 bytes after the declared tree blocks skipped");
}
