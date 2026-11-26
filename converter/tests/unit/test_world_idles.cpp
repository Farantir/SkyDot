// SPDX-License-Identifier: GPL-3.0-or-later
//
// world.fb 40: IDLE records (event, behaviour, parent and previous sibling, conditions, DATA).
#include "bethconv/pack/world.hpp"

#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"

#include "../support/esm_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>
#include <string_view>

using namespace bethconv;
using bethconv::test::ByteWriter;
using bethconv::test::TempDir;

namespace {

std::uint32_t tag_label(std::string_view type) {
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        raw |= static_cast<std::uint32_t>(static_cast<unsigned char>(type[i])) << (i * 8);
    }
    return raw;
}

void text(ByteWriter& payload, std::string_view tag, std::string_view value) {
    ByteWriter w;
    w.zstring(value);
    bethconv::test::write_field(payload, tag, w);
}

void record(ByteWriter& file, std::string_view type, std::uint32_t id, const ByteWriter& payload) {
    ByteWriter rec;
    bethconv::test::write_record(rec, type, id, payload.span());
    bethconv::test::write_group(file, tag_label(type), 0, rec.span());
}

void anam(ByteWriter& payload, std::uint32_t parent, std::uint32_t previous) {
    ByteWriter w;
    w.u32(parent);
    w.u32(previous);
    bethconv::test::write_field(payload, "ANAM", w);
}

void data(ByteWriter& payload, std::uint8_t min, std::uint8_t max, std::uint8_t flags,
          std::uint8_t section, std::uint16_t replay) {
    ByteWriter w;
    w.u8(min);
    w.u8(max);
    w.u8(flags);
    w.u8(section);
    w.u16(replay);
    bethconv::test::write_field(payload, "DATA", w);
}

void ctda(ByteWriter& payload, std::uint16_t function, float value, std::uint32_t param1) {
    ByteWriter c;
    c.u8(0);
    c.u8(0);
    c.u16(0);
    c.f32(value);
    c.u16(function);
    c.u16(0);
    c.u32(param1);
    c.u32(0);
    c.u32(1); // run on the target
    c.u32(0);
    c.u32(0xFFFF'FFFFU);
    bethconv::test::write_field(payload, "CTDA", c);
}

void make_idles(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0x01, {});

    // A root under an action (0x13), a leaf below it and its next sibling.
    ByteWriter root;
    text(root, "EDID", "TestRoot");
    text(root, "DNAM", "Actors\\Character\\Behaviors\\0_Master.hkx");
    anam(root, 0x13, 0);
    data(root, 0, 0, 0, 0, 0);
    record(file, "IDLE", 0xC01, root);

    ByteWriter leaf;
    text(leaf, "EDID", "TestLeaf");
    text(leaf, "ENAM", "idleTestEnter");
    anam(leaf, 0xC01, 0);
    data(leaf, 3, 7, 0x05, 2, 600);
    ctda(leaf, 613, 4.0F, 0);
    ctda(leaf, 631, 1.0F, 0);
    record(file, "IDLE", 0xC02, leaf);

    ByteWriter next;
    text(next, "EDID", "TestNext");
    text(next, "ENAM", "idleTestExit");
    anam(next, 0xC01, 0xC02);
    data(next, 0, 0, 0, 0, 0);
    record(file, "IDLE", 0xC03, next);

    // An idle marker playing the leaf and its sibling for 2.5 s.
    ByteWriter marker;
    text(marker, "EDID", "TestIdleMarker");
    ByteWriter idlf;
    idlf.u8(0x08);
    bethconv::test::write_field(marker, "IDLF", idlf);
    ByteWriter idlc;
    idlc.u8(2);
    bethconv::test::write_field(marker, "IDLC", idlc);
    ByteWriter idlt;
    idlt.f32(2.5F);
    bethconv::test::write_field(marker, "IDLT", idlt);
    ByteWriter idla;
    idla.u32(0xC02);
    idla.u32(0xC03);
    bethconv::test::write_field(marker, "IDLA", idla);
    record(file, "IDLM", 0xC05, marker);

    std::ofstream out(dir / "Idles.esm", std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

} // namespace

TEST_CASE("world.fb 40 carries IDLE records with their tree links and conditions",
          "[pack][world][idles]") {
    const TempDir dir;
    make_idles(dir);
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "Idles.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->idles == 3);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());

    const auto root = file->idle(0xC01);
    REQUIRE(root.has_value());
    CHECK(root->editor_id == "TestRoot");
    CHECK(root->behaviour == "Actors\\Character\\Behaviors\\0_Master.hkx");
    CHECK(root->event.empty());
    CHECK(root->parent == 0x13);
    CHECK(root->previous == 0);

    const auto leaf = file->idle(0xC02);
    REQUIRE(leaf.has_value());
    CHECK(leaf->event == "idleTestEnter");
    CHECK(leaf->parent == 0xC01);
    CHECK(leaf->loop_min == 3);
    CHECK(leaf->loop_max == 7);
    CHECK(leaf->flags == (pack::wfb::IdleFlags::parent | pack::wfb::IdleFlags::no_attacking));
    CHECK(leaf->animation_group_section == 2);
    CHECK(leaf->replay_delay == 600);
    REQUIRE(leaf->conditions.size() == 2);
    CHECK(leaf->conditions[0]->function == 613);
    CHECK(leaf->conditions[0]->value == 4.0F);
    CHECK(leaf->conditions[0]->run_on == 1);
    CHECK(leaf->conditions[1]->function == 631);

    const auto next = file->idle(0xC03);
    REQUIRE(next.has_value());
    CHECK(next->previous == 0xC02);
    CHECK_FALSE(file->idle(0xC04).has_value());

    const auto marker = file->idle_marker(0xC05);
    REQUIRE(marker.has_value());
    CHECK(marker->flags == 0x08);
    CHECK(marker->timer == 2.5F);
    CHECK(marker->idles == std::vector<std::uint32_t>{0xC02, 0xC03});
}
