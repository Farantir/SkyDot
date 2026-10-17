// SPDX-License-Identifier: GPL-3.0-or-later
//
// SCEN (format 27): the field order of a real scene (phases between two HNAMs,
// actors, actions between two ANAMs, the quest PNAM after the last action) and
// its trip through world.fb. The bytes follow RiverwoodSceneGerdurHod and a
// timer scene of Skyrim.esm.
#include "bethconv/pack/world.hpp"
#include "bethconv/record/forms_dialogue.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"
#include "bethconv/record/vmad.hpp"

#include "../support/esm_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string_view>

using namespace bethconv;
using namespace bethconv::test;

namespace {

void wstring(ByteWriter& w, std::string_view s) {
    w.u16(static_cast<std::uint16_t>(s.size()));
    w.raw(s);
}

void u32_field(ByteWriter& payload, std::string_view tag, std::uint32_t value) {
    ByteWriter w;
    w.u32(value);
    write_field(payload, tag, w);
}

void f32_field(ByteWriter& payload, std::string_view tag, float value) {
    ByteWriter w;
    w.f32(value);
    write_field(payload, tag, w);
}

void zstring_field(ByteWriter& payload, std::string_view tag, std::string_view text) {
    ByteWriter w;
    w.zstring(text);
    write_field(payload, tag, w);
}

void empty_field(ByteWriter& payload, std::string_view tag) {
    write_field(payload, tag, ByteWriter{});
}

void condition(ByteWriter& payload, std::uint32_t npc) {
    ByteWriter w;
    w.u8(0);
    w.u8(0);
    w.u16(0);
    w.f32(1.0F);
    w.u16(72);
    w.u16(0);
    w.u32(npc);
    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.u32(0xFFFFFFFF);
    write_field(payload, "CTDA", w);
}

void action(ByteWriter& p, std::uint16_t type, std::uint32_t index, std::int32_t alias,
            std::uint32_t phase) {
    ByteWriter anam;
    anam.u16(type);
    write_field(p, "ANAM", anam);
    zstring_field(p, "NAM0", "act");
    u32_field(p, "ALID", static_cast<std::uint32_t>(alias));
    u32_field(p, "INAM", index);
    u32_field(p, "SNAM", phase);
    u32_field(p, "ENAM", phase);
}

/// The VMAD of a scene: one script, then fragments: begin, no end, and two
/// phase fragments.
ByteWriter scene_vmad() {
    ByteWriter w;
    w.u16(5);
    w.u16(2);
    w.u16(1);
    wstring(w, "SF_Test");
    w.u8(0);
    w.u16(0);
    w.u8(2);  // version
    w.u8(1);  // begin only
    wstring(w, "SF_Test");
    w.u8(1);
    wstring(w, "SF_Test");
    wstring(w, "Fragment_0");
    w.u16(2);
    w.u8(1); // begin of phase 1
    w.u32(1);
    w.u8(1);
    wstring(w, "SF_Test");
    wstring(w, "Fragment_1");
    w.u8(2); // end of phase 2
    w.u32(2);
    w.u8(1);
    wstring(w, "SF_Test");
    wstring(w, "Fragment_2");
    return w;
}

ByteWriter scene_payload() {
    ByteWriter p;
    zstring_field(p, "EDID", "TestScene");
    write_field(p, "VMAD", scene_vmad());
    u32_field(p, "FNAM", 3);
    // Phase 0: a start and a completion condition.
    empty_field(p, "HNAM");
    zstring_field(p, "NAM0", "first");
    condition(p, 0x101);
    empty_field(p, "NEXT");
    condition(p, 0x102);
    empty_field(p, "NEXT");
    empty_field(p, "SCHR");
    u32_field(p, "WNAM", 100);
    empty_field(p, "HNAM");
    // Phase 1: nothing.
    empty_field(p, "HNAM");
    zstring_field(p, "NAM0", "second");
    empty_field(p, "NEXT");
    empty_field(p, "NEXT");
    empty_field(p, "HNAM");
    // Phase 2.
    empty_field(p, "HNAM");
    empty_field(p, "NEXT");
    empty_field(p, "NEXT");
    empty_field(p, "HNAM");
    // Actors.
    u32_field(p, "ALID", 2);
    u32_field(p, "LNAM", 0);
    u32_field(p, "DNAM", 0x1A);
    u32_field(p, "ALID", 1);
    u32_field(p, "LNAM", 4);
    u32_field(p, "DNAM", 0x1A);
    // Dialogue action.
    action(p, 0, 1, 2, 0);
    u32_field(p, "DATA", 0x300);
    u32_field(p, "HTID", 1);
    f32_field(p, "DMAX", 2.5F);
    f32_field(p, "DMIN", 0.5F);
    u32_field(p, "DEMO", 5);
    u32_field(p, "DEVA", 50);
    u32_field(p, "FNAM", 0x10);
    empty_field(p, "ANAM");
    // Package action with two packages.
    action(p, 1, 2, 1, 1);
    u32_field(p, "PNAM", 0x400);
    u32_field(p, "PNAM", 0x401);
    empty_field(p, "ANAM");
    // Timer: the second SNAM is the seconds, then an old-style script.
    ByteWriter anam;
    anam.u16(2);
    write_field(p, "ANAM", anam);
    zstring_field(p, "NAM0", "wait");
    u32_field(p, "INAM", 3);
    u32_field(p, "SNAM", 2);
    f32_field(p, "SNAM", 7.5F);
    u32_field(p, "ENAM", 2);
    empty_field(p, "QNAM");
    empty_field(p, "SCHR");
    empty_field(p, "ANAM");
    u32_field(p, "PNAM", 0x200); // the quest
    u32_field(p, "INAM", 3);
    ByteWriter vnam;
    for (int i = 0; i < 4; ++i) {
        vnam.u32(0);
    }
    write_field(p, "VNAM", vnam);
    condition(p, 0x103); // a scene condition
    return p;
}

record::FormContext context() { return {.localized = false}; }

void save(const TempDir& dir, std::string_view name, const ByteWriter& file) {
    std::ofstream out(dir / name, std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

void top_group(ByteWriter& file, std::string_view type, const ByteWriter& children) {
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        raw |= static_cast<std::uint32_t>(static_cast<unsigned char>(type[i])) << (i * 8);
    }
    write_group(file, raw, 0, children.span());
}

} // namespace

TEST_CASE("SCEN reads phases, actors, all three action types and fragments", "[record][scene]") {
    const auto p = scene_payload();
    io::SpanReader r{p.span(), "test"};
    const auto scene = record::parse_scene(r, context());
    REQUIRE(scene.has_value());
    CHECK(scene->editor_id == "TestScene");
    CHECK(scene->flags == 3);
    CHECK(scene->quest.value == 0x200);
    REQUIRE(scene->conditions.size() == 1);
    CHECK(scene->conditions[0].param1 == 0x103);

    REQUIRE(scene->phases.size() == 3);
    CHECK(scene->phases[0].name == "first");
    REQUIRE(scene->phases[0].start_conditions.size() == 1);
    CHECK(scene->phases[0].start_conditions[0].param1 == 0x101);
    REQUIRE(scene->phases[0].completion_conditions.size() == 1);
    CHECK(scene->phases[0].completion_conditions[0].param1 == 0x102);
    CHECK(scene->phases[1].start_conditions.empty());
    CHECK(scene->phases[2].name.empty());

    REQUIRE(scene->actors.size() == 2);
    CHECK(scene->actors[0].alias == 2);
    CHECK(scene->actors[0].behaviour_flags == 0x1A);
    CHECK(scene->actors[1].alias == 1);
    CHECK(scene->actors[1].flags == 4);

    REQUIRE(scene->actions.size() == 3);
    const auto& dialogue = scene->actions[0];
    CHECK(dialogue.type == 0);
    CHECK(dialogue.alias == 2);
    CHECK(dialogue.index == 1);
    CHECK(dialogue.flags == 0x10);
    CHECK(dialogue.start_phase == 0);
    CHECK(dialogue.end_phase == 0);
    CHECK(dialogue.topic.value == 0x300);
    CHECK(dialogue.head_track_alias == 1);
    CHECK(dialogue.delay_max == 2.5F);
    CHECK(dialogue.delay_min == 0.5F);
    CHECK(dialogue.emotion == 5);
    CHECK(dialogue.emotion_value == 50);
    const auto& package = scene->actions[1];
    CHECK(package.type == 1);
    CHECK(package.start_phase == 1);
    REQUIRE(package.packages.size() == 2);
    CHECK(package.packages[1].value == 0x401);
    const auto& timer = scene->actions[2];
    CHECK(timer.type == 2);
    CHECK(timer.name == "wait");
    CHECK(timer.alias == -1);
    CHECK(timer.start_phase == 2);
    CHECK(timer.timer_seconds == 7.5F);

    REQUIRE(scene->scripts.scripts.size() == 1);
    CHECK(scene->fragments.script == "SF_Test");
    CHECK(scene->fragments.begin_function == "Fragment_0");
    CHECK(scene->fragments.end_function.empty());
    REQUIRE(scene->fragments.phases.size() == 2);
    CHECK(scene->fragments.phases[0].phase == 1);
    CHECK(scene->fragments.phases[1].flags == 2);
    CHECK(scene->fragments.phases[1].function == "Fragment_2");
}

TEST_CASE("a truncated scene fragment block is an error", "[record][vmad][scene]") {
    const auto vmad = scene_vmad();
    const auto bytes = vmad.span();
    io::SpanReader r{bytes.first(bytes.size() - 3), "test"};
    REQUIRE(record::read_script_data(r).has_value());
    CHECK_FALSE(record::read_scene_fragments(r).has_value());
}

TEST_CASE("world.fb carries scenes with global FormIDs and every package", "[pack][world][scene]") {
    const TempDir dir;
    {
        ByteWriter file;
        write_tes4(file, 0, {});
        ByteWriter pack;
        zstring_field(pack, "EDID", "UnplacedPackage");
        ByteWriter packs;
        write_record(packs, "PACK", 0x0000'0400, pack.span());
        top_group(file, "PACK", packs);

        ByteWriter q;
        zstring_field(q, "EDID", "SceneQuest");
        ByteWriter quests;
        write_record(quests, "QUST", 0x0000'0200, q.span());
        top_group(file, "QUST", quests);

        ByteWriter scenes;
        write_record(scenes, "SCEN", 0x0000'0500, scene_payload().span());
        top_group(file, "SCEN", scenes);
        save(dir, "Base.esm", file);
    }
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "Base.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->scenes == 1);
    CHECK(stats->scene_phases == 3);
    CHECK(stats->scene_actors == 2);
    CHECK(stats->scene_dialogue_actions == 1);
    CHECK(stats->scene_package_actions == 1);
    CHECK(stats->scene_timer_actions == 1);
    CHECK(stats->scene_phase_fragments == 2);
    CHECK(stats->packages == 1);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());
    const auto scene = file->scene(0x0000'0500);
    REQUIRE(scene.has_value());
    CHECK(scene->editor_id == "TestScene");
    CHECK(scene->quest == 0x0000'0200);
    CHECK(scene->flags == (pack::wfb::SceneFlags::begin_on_quest_start |
                           pack::wfb::SceneFlags::stop_on_quest_end));
    REQUIRE(scene->phases.size() == 3);
    CHECK(scene->phases[0]->start_conditions.size() == 1);
    REQUIRE(scene->actions.size() == 3);
    CHECK(scene->actions[0]->topic == 0x0000'0300);
    REQUIRE(scene->actions[1]->packages.size() == 2);
    CHECK(scene->actions[1]->packages[0] == 0x0000'0400);
    CHECK(scene->actions[2]->timer_seconds == 7.5F);
    CHECK(scene->fragment_script == "SF_Test");
    REQUIRE(scene->phase_fragments.size() == 2);
    CHECK(scene->phase_fragments[1]->on_end);
    CHECK_FALSE(scene->phase_fragments[0]->on_end);
    CHECK(file->scenes().size() == 1);
}
