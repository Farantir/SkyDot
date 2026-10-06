// SPDX-License-Identifier: GPL-3.0-or-later
//
// Large references (world.fb format 11) over a world.fb written in memory:
// the per-square lists, which references are drawn for a camera square (the
// union of the lists in range, each once, minus those standing in a built
// square) and the enable parent's say.
#include "data/large_refs.hpp"
#include "data/world_data.hpp"

#include "support/world_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using skydot::WorldData;
using skydot::testing::BuiltWorld;
using skydot::testing::LargeRefSpec;
using skydot::testing::WorldSpec;
using skydot::testing::WorldspaceSpec;
namespace large_refs = skydot::large_refs;
namespace wfb = bethconv::pack::wfb;

namespace {

constexpr std::uint32_t k_world = 0x3C;

/// The built world.fb in a temp file that is removed with the test, opened as WorldData.
class OpenWorld {
public:
    explicit OpenWorld(const WorldSpec& spec)
        : path_(std::filesystem::temp_directory_path() / "skydot_large_refs_test.fb") {
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

/// The ids of the references at `indices`.
std::vector<std::uint32_t> ids(const WorldData& data, const std::vector<std::uint32_t>& indices) {
    std::vector<std::uint32_t> out;
    for (const auto index : indices) {
        out.push_back(data.large_ref(k_world, index)->id());
    }
    return out;
}

using Ids = std::vector<std::uint32_t>;

/// Three rocks in a row of squares: 0x10 at (0, 0), 0x20 at (3, 0) reaching
/// into (2, 0) and (4, 0), 0x30 at (9, 0) far away, and 0x40 at (0, 1).
WorldSpec rocks() {
    return {.format_version = 11,
            .worlds = {{.id = k_world,
                        .large_refs = {{.id = 0x30, .base = 3, .x = 9, .y = 0},
                                       {.id = 0x10, .base = 1, .x = 0, .y = 0},
                                       {.id = 0x20, .base = 2, .x = 3, .y = 0, .reaches = {{2, 0}, {4, 0}}},
                                       {.id = 0x40, .base = 4, .x = 0, .y = 1}}}}};
}

} // namespace

TEST_CASE("a pack without large references has none", "[large_refs]") {
    const OpenWorld old({.cells = {{.id = 1, .refs = {5}}}});
    REQUIRE(old.result.status == WorldData::Status::ok);
    CHECK(old.data.large_ref_count(k_world) == 0);
    CHECK(old.data.large_ref(k_world, 0) == nullptr);
    std::vector<std::uint32_t> out;
    old.data.large_cell_refs(k_world, 0, 0, out);
    CHECK(out.empty());
    CHECK(large_refs::select(old.data, k_world, 0, 0, 5, {}).empty());

    const OpenWorld empty({.format_version = 11, .worlds = {{.id = k_world}}});
    REQUIRE(empty.result.status == WorldData::Status::ok);
    CHECK(empty.data.large_ref_count(k_world) == 0);
    CHECK(large_refs::select(empty.data, k_world, 0, 0, 5, {}).empty());
}

TEST_CASE("a square's list holds what stands in it and what reaches into it", "[large_refs]") {
    const OpenWorld world(rocks());
    REQUIRE(world.result.status == WorldData::Status::ok);
    const WorldData& data = world.data;
    CHECK(data.large_ref_count(k_world) == 4);
    CHECK(data.large_ref_count(k_world + 1) == 0);
    // Sorted by id: 0x10, 0x20, 0x30, 0x40.
    CHECK(data.large_ref(k_world, 1)->id() == 0x20);
    CHECK(data.large_ref(k_world, 4) == nullptr);

    const auto list = [&](std::int32_t x, std::int32_t y) {
        std::vector<std::uint32_t> out;
        data.large_cell_refs(k_world, x, y, out);
        return ids(data, out);
    };
    CHECK(list(0, 0) == Ids{0x10});
    CHECK(list(2, 0) == Ids{0x20});
    CHECK(list(3, 0) == Ids{0x20});
    CHECK(list(4, 0) == Ids{0x20});
    CHECK(list(0, 1) == Ids{0x40});
    CHECK(list(9, 0) == Ids{0x30});
    CHECK(list(1, 0).empty());
    CHECK(list(5, 0).empty());
    CHECK(list(-3, -3).empty());
}

TEST_CASE("the references drawn are the union of the lists in range, each once", "[large_refs]") {
    const OpenWorld world(rocks());
    REQUIRE(world.result.status == WorldData::Status::ok);
    const WorldData& data = world.data;
    const auto drawn = [&](std::int32_t cx, std::int32_t cy, std::int32_t radius,
                           const large_refs::CellBuilt& built = {}) {
        return ids(data, large_refs::select(data, k_world, cx, cy, radius, built));
    };
    // 0x20 is listed in three squares of the window, once in the result.
    CHECK(drawn(3, 0, 1) == Ids{0x20});
    CHECK(drawn(3, 0, 3) == Ids{0x10, 0x20, 0x40});
    CHECK(drawn(0, 0, 3) == Ids{0x10, 0x20, 0x40});
    // The window is square: (0, 1) is one square from (0, 0), (9, 0) nine.
    CHECK(drawn(0, 0, 1) == Ids{0x10, 0x40});
    CHECK(drawn(0, 0, 9) == Ids{0x10, 0x20, 0x30, 0x40});
    // Reaching into the window is enough: 0x20 stands at 3 but is listed at 2.
    CHECK(drawn(1, 0, 1) == Ids{0x10, 0x20, 0x40});
    CHECK(drawn(-7, 0, 5).empty());
    CHECK(drawn(0, 0, -1).empty());
    CHECK(drawn(0, 0, 0) == Ids{0x10});
}

TEST_CASE("a reference standing in a built square is left to that cell", "[large_refs]") {
    const OpenWorld world(rocks());
    REQUIRE(world.result.status == WorldData::Status::ok);
    const WorldData& data = world.data;
    const auto drawn = [&](const large_refs::CellBuilt& built) {
        return ids(data, large_refs::select(data, k_world, 3, 0, 4, built));
    };
    CHECK(drawn({}) == Ids{0x10, 0x20, 0x40});
    CHECK(drawn([](std::int32_t x, std::int32_t y) { return x == 0 && y == 0; }) == Ids{0x20, 0x40});
    // Its own square decides, not the squares it only reaches into.
    CHECK(drawn([](std::int32_t x, std::int32_t y) { return x == 2 && y == 0; }) == Ids{0x10, 0x20, 0x40});
    CHECK(drawn([](std::int32_t x, std::int32_t y) { return x == 3 && y == 0; }) == Ids{0x10, 0x40});
    CHECK(drawn([](std::int32_t, std::int32_t) { return true; }).empty());
    // A square outside the window being built changes nothing.
    CHECK(drawn([](std::int32_t x, std::int32_t y) { return x == 9 && y == 0; }) == Ids{0x10, 0x20, 0x40});
}

TEST_CASE("the window is measured in squares by the larger of the two distances", "[large_refs]") {
    CHECK(large_refs::within(0, 0, 5, 5, 5));
    CHECK(!large_refs::within(0, 0, 6, 0, 5));
    CHECK(!large_refs::within(0, 0, 0, -6, 5));
    CHECK(large_refs::within(-3, 2, 1, -2, 4));
    CHECK(large_refs::within(7, 7, 7, 7, 0));
    CHECK(!large_refs::within(7, 7, 8, 7, 0));
}

TEST_CASE("a large reference follows its enable parent", "[large_refs]") {
    WorldSpec spec = rocks();
    auto& refs = spec.worlds[0].large_refs;
    // 0x10 follows 0x500 (placed enabled), 0x20 follows 0x501 (placed disabled),
    // 0x30 follows the opposite of 0x501, 0x40 follows a parent nobody knows.
    refs[1].enable_parent = 0x500;
    refs[2].enable_parent = 0x501;
    refs[0].enable_parent = 0x501;
    refs[0].flags = wfb::RefFlags::enable_opposite;
    refs[3].enable_parent = 0x777;
    spec.cells = {{.id = 7, .refs = {0x500}, .disabled_refs = {0x501}}};
    const OpenWorld world(spec);
    REQUIRE(world.result.status == WorldData::Status::ok);
    const WorldData& data = world.data;
    const auto disabled = [&](std::uint32_t id) {
        for (std::uint32_t i = 0; i < data.large_ref_count(k_world); ++i) {
            if (data.large_ref(k_world, i)->id() == id) {
                return data.initially_disabled(*data.large_ref(k_world, i));
            }
        }
        FAIL("no such large reference");
        return false;
    };
    CHECK(!disabled(0x10));
    CHECK(disabled(0x20));
    CHECK(!disabled(0x30));
    CHECK(!disabled(0x40));
}

TEST_CASE("a large reference with no enable parent is disabled by its own flag", "[large_refs]") {
    WorldSpec spec = rocks();
    spec.worlds[0].large_refs[0].flags = wfb::RefFlags::initially_disabled;
    const OpenWorld world(spec);
    REQUIRE(world.result.status == WorldData::Status::ok);
    CHECK(world.data.initially_disabled(*world.data.large_ref(k_world, 2)));
    CHECK(!world.data.initially_disabled(*world.data.large_ref(k_world, 0)));
}
