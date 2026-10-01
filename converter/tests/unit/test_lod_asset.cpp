// SPDX-License-Identifier: GPL-3.0-or-later
//
// LOD data files. `convert` covers real data (every .lod, .lst and .btt of
// three installs); these cover the round trip and the error paths.
#include "bethconv/pack/lod_asset.hpp"

#include "../support/lod_builder.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace bethconv;
using namespace bethconv::testing;

TEST_CASE("LOD settings decode and survive the asset", "[pack][lod]") {
    const auto bytes = build_lod_settings(-96, -96, 256, 4, 32);
    const auto data = pack::read_lod_source(bytes, ".lod", "tamriel.lod");
    REQUIRE(data.has_value());
    REQUIRE(data->settings.has_value());
    CHECK(data->settings->south_west_x == -96);
    CHECK(data->settings->stride == 256);
    CHECK(data->settings->highest_level == 32);

    const auto asset = pack::write_lod_asset(*data);
    const auto back = pack::read_lod_asset(asset, "tamriel.lodfb");
    REQUIRE(back.has_value());
    REQUIRE(back->settings.has_value());
    CHECK(back->settings->south_west_y == -96);
    CHECK(back->settings->lowest_level == 4);
    CHECK(back->trees.empty());
}

TEST_CASE("LOD settings of the wrong size or with odd levels are refused", "[pack][lod]") {
    auto bytes = build_lod_settings(0, 0, 64, 4, 32);
    bytes.push_back(std::byte{0});
    CHECK_FALSE(pack::read_lod_source(bytes, ".lod", "x").has_value());
    CHECK_FALSE(pack::read_lod_source(build_lod_settings(0, 0, 64, 3, 32), ".lod", "x").has_value());
    CHECK_FALSE(pack::read_lod_source(build_lod_settings(0, 0, 64, 32, 4), ".lod", "x").has_value());
    CHECK_FALSE(pack::read_lod_source(build_lod_settings(0, 0, 0, 4, 32), ".lod", "x").has_value());
}

TEST_CASE("tree types and trees decode and survive the asset", "[pack][lod]") {
    const auto list = build_tree_list({{.index = 0}, {.index = 1, .width = 100.0F, .u0 = 0.5F}});
    const auto types = pack::read_lod_source(list, ".lst", "t.lst");
    REQUIRE(types.has_value());
    REQUIRE(types->tree_types.size() == 2);
    CHECK(types->tree_types[1].width == 100.0F);
    CHECK(types->tree_types[1].u0 == 0.5F);

    const auto blocks = build_tree_blocks(
        {{3, {{.x = 10.0F, .y = 20.0F, .z = 30.0F, .rotation = 1.5F, .scale = 0.5F, .ref = 0x1234}}},
         {1, {{.x = 1.0F}, {.x = 2.0F}}}});
    const auto trees = pack::read_lod_source(blocks, ".btt", "t.btt");
    REQUIRE(trees.has_value());
    REQUIRE(trees->trees.size() == 3);
    CHECK(trees->trees[0].type == 3);
    CHECK(trees->trees[0].ref == 0x1234u);
    CHECK(trees->trees[2].type == 1);
    CHECK(trees->trees[2].x == 2.0F);
    CHECK(trees->trailing_bytes == 0);

    const auto back = pack::read_lod_asset(pack::write_lod_asset(*trees), "t.lodfb");
    REQUIRE(back.has_value());
    REQUIRE(back->trees.size() == 3);
    CHECK(back->trees[0].scale == 0.5F);
    CHECK(back->trees[1].unknown1 == 1u);
    CHECK_FALSE(back->settings.has_value());
}

TEST_CASE("bytes after a tree file's blocks are skipped and counted", "[pack][lod]") {
    auto blocks = build_tree_blocks({{0, {{.x = 1.0F}}}});
    blocks.resize(blocks.size() + 40, std::byte{0x47});
    const auto trees = pack::read_lod_source(blocks, ".btt", "t.btt");
    REQUIRE(trees.has_value());
    CHECK(trees->trees.size() == 1);
    CHECK(trees->trailing_bytes == 40);
}

TEST_CASE("LOD counts larger than the file fail rather than read past it", "[pack][lod]") {
    auto list = build_tree_list({{.index = 0}});
    list[0] = std::byte{0xFF}; // count 255
    CHECK_FALSE(pack::read_lod_source(list, ".lst", "x").has_value());

    auto blocks = build_tree_blocks({{0, {{.x = 1.0F}}}});
    blocks[8] = std::byte{0x10}; // the block claims 16 trees
    CHECK_FALSE(pack::read_lod_source(blocks, ".btt", "x").has_value());

    // Bytes left over in a tree list are an error.
    list = build_tree_list({{.index = 0}});
    list.push_back(std::byte{0});
    CHECK_FALSE(pack::read_lod_source(list, ".lst", "x").has_value());

    CHECK_FALSE(pack::read_lod_source(list, ".nif", "x").has_value());
    const std::vector<std::byte> junk(32, std::byte{0x5A});
    CHECK_FALSE(pack::read_lod_asset(junk, "junk").has_value());
}
