// SPDX-License-Identifier: GPL-3.0-or-later
//
// WorldData (data/world_data.hpp) over a world.fb written in memory: which
// cell holds a reference, from the index `open` builds.
#include "data/world_data.hpp"

#include "support/world_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using skydot::WorldData;
using skydot::testing::BuiltWorld;
using skydot::testing::CellSpec;
using skydot::testing::WorldSpec;

namespace {

/// The built world.fb in a temp file that is removed with the test, opened as WorldData.
class OpenWorld {
public:
    explicit OpenWorld(const WorldSpec& spec)
        : path_(std::filesystem::temp_directory_path() / "skydot_world_data_test.fb") {
        const BuiltWorld built(spec);
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(built.bytes().data()),
                  static_cast<std::streamsize>(built.bytes().size()));
        out.close();
        result = data.open(path_.string());
    }
    ~OpenWorld() { std::filesystem::remove(path_); }
    OpenWorld(const OpenWorld&) = delete;
    OpenWorld& operator=(const OpenWorld&) = delete;

    WorldData data;
    WorldData::OpenResult result;

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("the cell holding a reference is found by its id", "[world_data]") {
    const OpenWorld world({.cells = {
        {.id = 0x3000, .refs = {0x0100'0800, 0x14, 0x9000'0010}},
        {.id = 0x1000, .refs = {0x0100'0801, 0x15}},
        {.id = 0x2000, .refs = {}},
        {.id = 0x4000, .refs = {0xFFFF'FFFF, 0x0100'0802}}}});
    REQUIRE(world.result.status == WorldData::Status::ok);
    const WorldData& data = world.data;
    CHECK(data.cell_of_ref(0x14) == 0x3000);
    CHECK(data.cell_of_ref(0x15) == 0x1000);
    CHECK(data.cell_of_ref(0x0100'0800) == 0x3000);
    CHECK(data.cell_of_ref(0x0100'0801) == 0x1000);
    CHECK(data.cell_of_ref(0x0100'0802) == 0x4000);
    CHECK(data.cell_of_ref(0x9000'0010) == 0x3000);
    CHECK(data.cell_of_ref(0xFFFF'FFFF) == 0x4000);
    CHECK(data.cell_of_ref(0x13) == 0);
    CHECK(data.cell_of_ref(0x16) == 0);
    CHECK(data.cell_of_ref(0) == 0);
    CHECK(data.cell_of_ref(0x0100'0803) == 0);
    CHECK(data.cell_of_ref(0x2000) == 0); // a cell is not a reference
}

TEST_CASE("many references over many cells are all found", "[world_data]") {
    // Ids spread over all 32 bits, so every digit of the index's sort is used.
    WorldSpec spec;
    std::map<std::uint32_t, std::uint32_t> expected;
    std::uint32_t state = 12345;
    for (std::uint32_t cell = 1; cell <= 300; ++cell) {
        CellSpec c{.id = cell * 16};
        for (int i = 0; i < 100; ++i) {
            state = state * 1664525U + 1013904223U;
            if (expected.emplace(state, c.id).second) {
                c.refs.push_back(state);
            }
        }
        spec.cells.push_back(std::move(c));
    }
    const OpenWorld world(spec);
    REQUIRE(world.result.status == WorldData::Status::ok);
    std::size_t wrong = 0;
    for (const auto& [ref, cell] : expected) {
        const auto next = expected.find(ref + 1);
        if (world.data.cell_of_ref(ref) != cell ||
            world.data.cell_of_ref(ref + 1) != (next != expected.end() ? next->second : 0U)) {
            ++wrong;
        }
    }
    CHECK(wrong == 0);
    CHECK(expected.size() > 25000);
}

TEST_CASE("a world without cells, or a closed one, holds no reference", "[world_data]") {
    const OpenWorld empty(WorldSpec{});
    REQUIRE(empty.result.status == WorldData::Status::ok);
    CHECK(empty.data.cell_of_ref(0x14) == 0);
    CHECK(WorldData().cell_of_ref(0x14) == 0);
}

TEST_CASE("typed queries answer from the indexes, and a closed world answers nothing", "[world_data]") {
    const OpenWorld world({.cells = {{.id = 0x3000, .refs = {0x14, 0x15}}}});
    REQUIRE(world.result.status == WorldData::Status::ok);
    const WorldData& data = world.data;
    CHECK(data.ref_ptr(0x14) != nullptr);
    CHECK(data.ref_ptr(0x16) == nullptr);
    WorldData::RefEnable info;
    CHECK(data.ref_enable(0x15, info));
    CHECK(info.enable_parent == 0);
    CHECK_FALSE(info.initially_disabled);
    CHECK_FALSE(data.ref_enable(0x16, info));
    CHECK(data.keyword_by_editor_id("NoSuchKeyword") == 0);
    CHECK_FALSE(data.base_has_keyword(0x14, 0x100));
    CHECK(data.form_list_ptr(0x100) == nullptr);
    CHECK(data.message_ptr(0x100) == nullptr);
    CHECK(data.explosion_ptr(0x100) == nullptr);
    CHECK(data.destruction_ptr(0x100) == nullptr);

    const WorldData closed;
    CHECK(closed.ref_ptr(0x14) == nullptr);
    CHECK(closed.keyword_by_editor_id("x") == 0);
    CHECK(closed.form_list_ptr(1) == nullptr);
    CHECK(closed.message_ptr(1) == nullptr);
}
