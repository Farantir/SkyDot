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
#include "../support/nif_builder.hpp"
#include "../support/pex_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
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
        if (warning["vpath"].get<std::string>() == vpath) {
            details.push_back(warning["detail"]);
        }
    }
    return details;
}

/// Every file under `root` by relative path, with its bytes.
[[nodiscard]] std::map<std::string, std::string> files_under(const std::filesystem::path& root) {
    std::map<std::string, std::string> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::ifstream in(entry.path(), std::ios::binary);
            files[std::filesystem::relative(entry.path(), root).generic_string()] =
                std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    return files;
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
    // Named: a range-for over a member of a temporary only keeps the
    // temporary alive from C++23 on (GCC 14 frees it first).
    const auto report = run.report();
    for (const auto& failure : report["failures"]) {
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

TEST_CASE("the pack is the same bytes on one thread, three and eight", "[convert][threads]") {
    Run run;
    const auto nif = [](int n) {
        bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
        auto* shape = builder.add_shape("Cube" + std::to_string(n), bethconv::test::make_cube());
        builder.add_shader(shape, "textures/t" + std::to_string(n) + ".dds",
                           "textures/t" + std::to_string(n) + "_n.dds");
        return builder.bytes();
    };
    const auto script = [](int n) {
        return bethconv::testing::build_script(
            bethconv::testing::empty_script("S" + std::to_string(n)));
    };
    const auto rgba = [](std::uint32_t side, bool cube) {
        return bethconv::testing::build_dds(
            DdsSpec{.width = side, .height = side, .mips = 3, .fourcc = bethconv::io::FourCC{},
                    .rgb_bit_count = 32, .cubemap = cube});
    };

    // More tiny scripts than the pool's window, so its slots are reused.
    for (int n = 0; n < 1100; ++n) {
        run.add("scripts/gen" + std::to_string(n) + ".pex", script(n));
    }
    for (int n = 0; n < 60; ++n) {
        const std::string id = std::to_string(n);
        run.add("meshes/m" + id + ".nif", nif(n));
        // The same bytes under names sorting before and after the first.
        run.add("meshes/a_copy" + id + ".nif", nif(n));
        run.add("meshes/z_copy" + id + ".nif", nif(n));
        run.add("textures/t" + id + ".dds",
                bethconv::testing::build_dds(
                    DdsSpec{.width = 8 + 4 * static_cast<std::uint32_t>(n % 7), .height = 8,
                            .mips = 2}));
        run.add("textures/u" + id + ".dds", rgba(16, false));
        run.add("textures/u" + id + "_n.dds", rgba(16, false));
        // Warnings name the path of the input that was converted.
        run.add("textures/cube" + id + ".dds", rgba(8, true));
        run.add("textures/cube" + id + "_n.dds", rgba(8, true));
    }
    // Inputs that fail, twice with the same bytes: each is converted and
    // reported, as on one thread.
    run.add("meshes/bad.nif", junk());
    run.add("meshes/bad_copy.nif", junk());
    run.add("scripts/bad.pex", junk());
    run.add("sound/voice.wav", junk());
    run.options.texture_encoding = bethconv::texture::Encoding::bc7;

    for (const auto layout : {StoreLayout::blob, StoreLayout::loose}) {
        run.options.layout = layout;
        std::map<std::string, std::string> first;
        std::uint64_t first_converted = 0;
        int attempt = 0;
        for (const unsigned jobs : {1U, 3U, 8U, 8U}) {
            run.options.jobs = jobs;
            run.options.out = run.out / (std::string(to_string(layout)) + std::to_string(++attempt));
            const auto result = run.go();
            CHECK(result.jobs == jobs);
            CHECK(result.pack.failed == 3);
            CHECK(result.pack.deduped > 0);
            auto files = files_under(run.pack());
            if (first.empty()) {
                first = std::move(files);
                first_converted = result.pack.converted;
                continue;
            }
            CHECK(result.pack.converted == first_converted);
            CHECK(files.size() == first.size());
            for (const auto& [name, bytes] : first) {
                const bool same = files.contains(name) && files.at(name) == bytes;
                INFO(name << " with " << jobs << " jobs, " << to_string(layout));
                CHECK(same);
            }
        }

        // Run again over the last pack: it has every asset, so nothing is
        // converted, and the assets and the index do not change (the report
        // and manifest count what this run wrote).
        const auto again = run.go();
        CHECK(again.pack.converted == 0);
        CHECK(again.pack.failed == 3);
        const auto after = files_under(run.pack());
        CHECK(after.size() == first.size());
        for (const auto& [name, bytes] : first) {
            if (name == "report.json" || name == "manifest.json") {
                continue;
            }
            const bool same = after.contains(name) && after.at(name) == bytes;
            INFO(name << " after a second run, " << to_string(layout));
            CHECK(same);
        }
    }
}

namespace {

/// A complete 16x16 DXT5 chain whose upper levels thin out: half the
/// blocks opaque in level 0, a row of blocks at alpha 90 (under a threshold of
/// 128) in level 1.
[[nodiscard]] Bytes thinning_dxt5() {
    auto file = bethconv::testing::build_dds(
        DdsSpec{.width = 16, .height = 16, .mips = 5, .fourcc = bethconv::io::FourCC{"DXT5"}});
    std::size_t at = 128;
    for (std::uint32_t level = 0; level < 5; ++level) {
        const std::uint32_t blocks = std::max(1U, (16U >> level) / 4);
        for (std::uint32_t by = 0; by < blocks; ++by) {
            for (std::uint32_t bx = 0; bx < blocks; ++bx, at += 16) {
                std::uint8_t alpha = 0;
                if (level == 0) {
                    alpha = bx < 2 ? 255 : 0;
                } else if (level == 1) {
                    alpha = by < 1 ? 90 : 0;
                }
                for (std::size_t i = 0; i < 8; ++i) {
                    file[at + i] = std::byte{0};
                }
                file[at] = static_cast<std::byte>(alpha);
                file[at + 1] = static_cast<std::byte>(alpha);
            }
        }
    }
    return file;
}

[[nodiscard]] Bytes foliage_nif() {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    auto* leaf = builder.add_shape("Leaf", bethconv::test::make_cube());
    builder.add_shader(leaf, "Textures\\Leaf.dds", "textures/leaf_n.dds");
    builder.add_alpha(leaf, 0x0200 | 0x0001, 128); // test and blend: a cut-out
    auto* glass = builder.add_shape("Glass", bethconv::test::make_cube());
    builder.add_shader(glass, "textures/glass.dds");
    builder.add_alpha(glass, 0x0001, 0); // blend only
    auto* gloss = builder.add_shape("Shared", bethconv::test::make_cube());
    builder.add_shader(gloss, "textures/shared.dds");
    builder.add_alpha(gloss, 0x0200, 128);
    auto* other = builder.add_shape("Other", bethconv::test::make_cube());
    builder.add_shader(other, "textures/other.dds", "textures/shared.dds"); // as a normal map
    return builder.bytes();
}

} // namespace

TEST_CASE("only textures a material alpha-tests keep their coverage through the mips",
          "[convert][texture][coverage]") {
    Run run;
    const Bytes thin = thinning_dxt5();
    run.add("meshes/foliage.nif", foliage_nif());
    run.add("textures/leaf.dds", thin);
    run.add("textures/leaf_n.dds", thin);
    run.add("textures/glass.dds", thin);
    run.add("textures/shared.dds", thin);
    run.add("textures/other.dds", thin);
    run.add("textures/unused.dds", thin);

    const auto result = run.go();
    const auto& a = result.alpha_coverage;
    CHECK(a.meshes_scanned == 1);
    CHECK(a.textures_alpha_tested == 2);   // leaf and shared
    CHECK(a.textures_blend_only == 1);     // glass
    CHECK(a.textures_shared_slot == 1);    // shared is also a normal map
    CHECK(a.textures_treated == 1);
    CHECK(a.textures_conflicting == 0);
    CHECK(a.adjusted == 1);

    CHECK(run.stored("textures/leaf.dds") != thin);
    CHECK(run.stored("textures/leaf.dds").size() == thin.size());
    for (const char* untouched : {"textures/leaf_n.dds", "textures/glass.dds", "textures/shared.dds",
                                  "textures/other.dds", "textures/unused.dds"}) {
        INFO(untouched);
        CHECK(run.stored(untouched) == thin);
    }
    const auto report = run.report();
    CHECK(report["alpha_coverage"]["treated"]["textures/leaf.dds"] == 128);
    CHECK(report["alpha_coverage"]["textures_treated"] == 1);
}

TEST_CASE("the coverage treatment is part of the asset's recipe", "[convert][texture][coverage]") {
    Run run;
    run.add("meshes/foliage.nif", foliage_nif());
    run.add("textures/leaf.dds", thinning_dxt5());
    (void)run.go();
    const auto treated = VpathIndex::read(run.pack() / "vpath.idx");
    REQUIRE(treated.has_value());
    const std::string with = treated->find("textures/leaf.dds")->hex;

    // Off, over the same pack: a different recipe, so a new asset, and the
    // texture is the source's bytes again.
    run.options.alpha_coverage = false;
    const auto off = run.go();
    CHECK(off.pack.converted >= 1);
    CHECK(off.alpha_coverage.textures_treated == 0);
    const auto plain = VpathIndex::read(run.pack() / "vpath.idx");
    REQUIRE(plain.has_value());
    CHECK(plain->find("textures/leaf.dds")->hex != with);
    CHECK(run.stored("textures/leaf.dds") == thinning_dxt5());

    // On again: the first asset is still there and is reused.
    run.options.alpha_coverage = true;
    const auto again = run.go();
    CHECK(again.pack.converted == 0);
    const auto back = VpathIndex::read(run.pack() / "vpath.idx");
    REQUIRE(back.has_value());
    CHECK(back->find("textures/leaf.dds")->hex == with);
}
