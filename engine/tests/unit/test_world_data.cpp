// SPDX-License-Identifier: GPL-3.0-or-later
//
// WorldData (data/world_data.hpp) over a world.fb written in memory: which
// cell holds a reference, from the index `open` builds.
#include "data/world_data.hpp"

#include "support/world_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using skydot::WorldData;
using skydot::testing::BuiltWorld;
using skydot::testing::ActorRefSpec;
using skydot::testing::BaseSpec;
using skydot::testing::CellSpec;
using skydot::testing::DoorSpec;
using skydot::testing::LinkSpec;
using skydot::testing::LockSpec;
using skydot::testing::PlacedRefSpec;
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

TEST_CASE("a reference is a typed record with its lock, door and links", "[world_data]") {
    constexpr std::uint32_t k_door = 0x524F'4F44; // "DOOR"
    namespace wfb = skydot::testing::wfb;
    const OpenWorld world({
        .cells = {{.id = 0x3000,
                   .interior = true,
                   .placed = {{.id = 0x100,
                               .base = 0x50,
                               .position = {1.0F, 2.0F, 3.0F},
                               .rotation = {0.5F, 0.0F, 1.5F},
                               .scale = 0.5F,
                               .flags = wfb::RefFlags::parent_activate_only},
                              {.id = 0x101, .base = 0x51, .flags = wfb::RefFlags::initially_disabled}},
                   .locks = {{.ref = 0x100, .level = 75, .key = 0x60}},
                   .links = {{.ref = 0x100, .keyword = 0x70, .target = 0x101}, {.ref = 0x100, .keyword = 0, .target = 0x102}},
                   .doors = {{.ref = 0x100, .destination = 0x200, .position = {10.0F, 20.0F, 30.0F}, .rotation = {0.0F, 0.0F, 3.0F}}}},
                  {.id = 0x4000,
                   .world = 0x7,
                   .placed = {{.id = 0x200, .base = 0x50}}}},
        .actors = {{.ref = 0x300, .base = 0x900, .cell = 0x3000, .position = {4.0F, 5.0F, 6.0F}, .rotation = {0.0F, 0.0F, 2.0F}}},
        .bases = {{.id = 0x50, .type = k_door, .editor_id = "TestDoor"}},
    });
    REQUIRE(world.result.status == WorldData::Status::ok);
    const WorldData& data = world.data;
    // The door side is only in the cell that holds the link: 0x200 is in 0x4000 with a link back.
    WorldData::RefRecord r;
    REQUIRE(data.ref_record(0x100, r));
    CHECK(r.id == 0x100);
    CHECK(r.cell == 0x3000);
    CHECK(r.base == 0x50);
    CHECK(r.type == "DOOR");
    CHECK(r.editor_id == "TestDoor");
    CHECK_FALSE(r.actor);
    CHECK(r.parent_activate_only);
    CHECK_FALSE(r.initially_disabled);
    CHECK(r.position == std::array<float, 3>{1.0F, 2.0F, 3.0F});
    CHECK(r.rotation == std::array<float, 3>{0.5F, 0.0F, 1.5F});
    CHECK(r.scale == 0.5F);
    CHECK(r.locked);
    CHECK(r.lock_level == 75);
    CHECK(r.lock_key == 0x60);
    CHECK(r.load_door);
    CHECK(r.door_destination == 0x200);
    REQUIRE(r.links.size() == 2);
    CHECK(r.links[0].keyword == 0x70);
    CHECK(r.links[0].target == 0x101);
    CHECK(r.links[1].keyword == 0);
    CHECK(r.links[1].target == 0x102);

    REQUIRE(data.ref_record(0x101, r));
    CHECK(r.initially_disabled);
    CHECK(r.type.empty()); // its base is unknown
    CHECK_FALSE(r.locked);
    CHECK_FALSE(r.load_door);
    CHECK(r.links.empty());

    REQUIRE(data.ref_record(0x300, r));
    CHECK(r.actor);
    CHECK(r.type == "NPC_");
    CHECK(r.editor_id.empty());
    CHECK(r.base == 0x900);
    CHECK(r.cell == 0x3000);
    CHECK(r.position == std::array<float, 3>{4.0F, 5.0F, 6.0F});
    CHECK(r.rotation == std::array<float, 3>{0.0F, 0.0F, 2.0F});
    CHECK(r.scale == 1.0F);
    CHECK_FALSE(r.locked);

    CHECK_FALSE(data.ref_record(0x999, r));
    CHECK_FALSE(WorldData().ref_record(0x100, r));
}

TEST_CASE("a load door's link is a typed record", "[world_data]") {
    const OpenWorld world({.cells = {
        {.id = 0x3000, .interior = true, .placed = {{.id = 0x100}},
         .doors = {{.ref = 0x100, .destination = 0x200, .position = {10.0F, 20.0F, 30.0F}, .rotation = {0.0F, 0.0F, 3.0F}}}},
        {.id = 0x4000, .world = 0x7, .placed = {{.id = 0x200}},
         .doors = {{.ref = 0x200, .destination = 0x100}}}}});
    REQUIRE(world.result.status == WorldData::Status::ok);
    WorldData::DoorRecord d;
    REQUIRE(world.data.door_record(0x100, d));
    CHECK(d.ref == 0x100);
    CHECK(d.cell == 0x3000);
    CHECK(d.destination == 0x200);
    CHECK(d.destination_cell == 0x4000);
    CHECK(d.destination_world == 0x7);
    CHECK_FALSE(d.destination_interior);
    CHECK(d.arrival_position == std::array<float, 3>{10.0F, 20.0F, 30.0F});
    CHECK(d.arrival_rotation == std::array<float, 3>{0.0F, 0.0F, 3.0F});
    REQUIRE(world.data.door_record(0x200, d));
    CHECK(d.destination_cell == 0x3000);
    CHECK(d.destination_interior);
    CHECK_FALSE(world.data.door_record(0x999, d));
}
