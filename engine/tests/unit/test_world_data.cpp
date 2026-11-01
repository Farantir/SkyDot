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
using skydot::testing::ConditionSpec;
using skydot::testing::ItemSpec;
using skydot::testing::PerkEffectSpec;
namespace wfb = bethconv::pack::wfb;

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

TEST_CASE("perks, skills, classes, taught books and NPC perks are read (world.fb 38)", "[world_data][skills]") {
    const auto tab = [](std::uint8_t index, std::vector<ConditionSpec> conditions) {
        return PerkEffectSpec::Tab{.tab = index, .conditions = std::move(conditions)};
    };
    WorldSpec spec;
    spec.format_version = WorldData::FORMAT_VERSION;
    spec.perks = {
        {.id = 0x200,
         .editor_id = "Armsman00",
         .name = "Armsman",
         .level = 0,
         .ranks = 5,
         .next_perk = 0x201,
         .conditions = {skydot::testing::condition(448, skydot::testing::eq, 1.0F, 0, 0x300)},
         .effects = {{.priority = 3,
                      .entry_point = 0x23,
                      .function = 3,
                      .tabs = {tab(0, {skydot::testing::condition(448, skydot::testing::eq, 0.0F, 0, 0x201)}),
                               tab(1, {skydot::testing::condition(0x10C, skydot::testing::eq, 1.0F, skydot::testing::k_or, 0x1E714),
                                       skydot::testing::condition(0x10C, skydot::testing::eq, 1.0F, 0, 0x1E711)})},
                      .data_type = 1,
                      .values = {1.2F}},
                     {.priority = 22,
                      .entry_point = 0x42,
                      .function = 14,
                      .data_type = 2,
                      .values = {0.01F},
                      .actor_value = 106},
                     {.kind = wfb::PerkEffectKind::ability, .rank = 1, .spell = 0x500}}},
        {.id = 0x100, .editor_id = "First", .trait = true, .hidden = true}};
    spec.actor_value_infos = {{.id = 0x44C,
                               .editor_id = "AVOneHanded",
                               .actor_value = 6,
                               .name = "One-handed",
                               .has_skill = true,
                               .use_mult = 6.3F,
                               .improve_mult = 2.0F,
                               .perk_tree = {{.index = 0, .links = {7}}, {.index = 7, .perk = 0x200, .grid_x = 2}}},
                              {.id = 0x45B, .editor_id = "AVMysticism", .actor_value = 21}};
    spec.classes = {{.id = 0x400,
                     .editor_id = "TrainerSmithingJourneyman",
                     .teaches = 10,
                     .max_training_level = 50,
                     .skill_weights = {0, 1, 2}}};
    spec.bases = {{.id = 0x600, .item = ItemSpec{.category = wfb::ItemCategory::book,
                                                 .book_flags = 1,
                                                 .book_skill = 8}},
                  {.id = 0x601, .item = ItemSpec{.category = wfb::ItemCategory::book,
                                                 .book_flags = 4,
                                                 .book_spell = 0x500}}};
    spec.npcs = {{.id = 0x700, .class_id = 0x400, .perks = {{0x200, 2}, {0x100}}}};
    const OpenWorld world(spec);
    REQUIRE(world.result.status == WorldData::Status::ok);
    const WorldData& data = world.data;

    CHECK(data.perk_ptr(0x999) == nullptr);
    const auto* first = data.perk_ptr(0x100);
    REQUIRE(first != nullptr);
    CHECK(first->trait());
    CHECK(first->hidden());
    const auto* armsman = data.perk_ptr(0x200);
    REQUIRE(armsman != nullptr);
    CHECK(armsman->editor_id()->str() == "Armsman00");
    CHECK(armsman->ranks() == 5);
    CHECK(armsman->next_perk() == 0x201);
    REQUIRE(armsman->conditions() != nullptr);
    CHECK(armsman->conditions()->size() == 1);
    REQUIRE(armsman->effects() != nullptr);
    REQUIRE(armsman->effects()->size() == 3);
    const auto* damage = armsman->effects()->Get(0);
    CHECK(damage->kind() == wfb::PerkEffectKind::entry_point);
    CHECK(damage->entry_point() == 0x23);
    CHECK(damage->function() == 3);
    CHECK(damage->tab_count() == 2);
    REQUIRE(damage->tabs()->size() == 2);
    CHECK(damage->tabs()->Get(1)->tab() == 1);
    CHECK(damage->tabs()->Get(1)->conditions()->size() == 2);
    CHECK(damage->data_type() == 1);
    CHECK(damage->values()->Get(0) == 1.2F);
    CHECK(damage->actor_value() == -1);
    const auto* boost = armsman->effects()->Get(1);
    CHECK(boost->actor_value() == 106);
    CHECK(boost->values()->Get(0) == 0.01F);
    CHECK(armsman->effects()->Get(2)->kind() == wfb::PerkEffectKind::ability);
    CHECK(armsman->effects()->Get(2)->spell() == 0x500);

    const auto* skill = data.actor_value_info_of(6);
    REQUIRE(skill != nullptr);
    CHECK(skill->id() == 0x44C);
    CHECK(skill->has_skill());
    CHECK(skill->use_mult() == 6.3F);
    CHECK(skill->improve_mult() == 2.0F);
    REQUIRE(skill->perk_tree()->size() == 2);
    CHECK(skill->perk_tree()->Get(0)->links()->Get(0) == 7);
    CHECK(skill->perk_tree()->Get(1)->perk() == 0x200);
    CHECK(skill->perk_tree()->Get(1)->grid_x() == 2);
    CHECK(data.actor_value_info_ptr(0x45B)->actor_value() == 21);
    CHECK(data.actor_value_info_of(21)->id() == 0x45B);
    CHECK(data.actor_value_info_of(7) == nullptr);
    CHECK(data.actor_value_info_of(-1) == nullptr);

    const auto* clas = data.class_ptr(0x400);
    REQUIRE(clas != nullptr);
    CHECK(clas->teaches() == 10);
    CHECK(clas->max_training_level() == 50);
    CHECK(clas->skill_weights()->size() == 3);
    CHECK(data.class_ptr(0x401) == nullptr);

    const auto* skill_book = data.base_ptr(0x600)->item();
    CHECK(skill_book->book_flags() == 1);
    CHECK(skill_book->book_skill() == 8);
    CHECK(skill_book->book_spell() == 0);
    const auto* tome = data.base_ptr(0x601)->item();
    CHECK(tome->book_flags() == 4);
    CHECK(tome->book_skill() == -1);
    CHECK(tome->book_spell() == 0x500);

    const auto* npc = data.root()->npcs()->LookupByKey(0x700);
    REQUIRE(npc != nullptr);
    CHECK(npc->class_id() == 0x400);
    REQUIRE(npc->perks()->size() == 2);
    CHECK(npc->perks()->Get(0)->perk() == 0x200);
    CHECK(npc->perks()->Get(0)->rank() == 2);
    CHECK(npc->perks()->Get(1)->perk() == 0x100);
    CHECK(npc->perks()->Get(1)->rank() == 1);
}
