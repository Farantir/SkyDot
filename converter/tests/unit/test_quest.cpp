// SPDX-License-Identifier: GPL-3.0-or-later
//
// QUST. `bethconv forms` covers real data (every quest in three installs and
// the FUS list); these cover the positional rules, which real data only tests
// in aggregate, and the fragment block of VMAD.
#include "bethconv/record/forms_game.hpp"
#include "bethconv/record/vmad.hpp"

#include "../support/esm_builder.hpp"

#include <catch2/catch_test_macros.hpp>

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

void zstring_field(ByteWriter& payload, std::string_view tag, std::string_view text) {
    ByteWriter w;
    w.zstring(text);
    write_field(payload, tag, w);
}

/// A CTDA whose first byte tells the tests which one it is.
void condition(ByteWriter& payload, std::uint8_t marker) {
    ByteWriter w;
    w.u8(marker);
    for (int i = 1; i < 32; ++i) {
        w.u8(0);
    }
    write_field(payload, "CTDA", w);
}

/// VMAD: one quest script, two fragments, one alias with one script.
ByteWriter quest_vmad() {
    ByteWriter w;
    w.u16(5); // version
    w.u16(2); // object format
    w.u16(1);
    wstring(w, "TestQuestScript");
    w.u8(0);
    w.u16(0);
    // Fragments.
    w.u8(2);
    w.u16(2);
    wstring(w, "QF_TestQuest_01000D62");
    for (const auto& [stage, fn] : {std::pair{10, "Fragment_0"}, std::pair{20, "Fragment_1"}}) {
        w.u16(static_cast<std::uint16_t>(stage));
        w.u16(0);
        w.u32(0);
        w.u8(1);
        wstring(w, "QF_TestQuest_01000D62");
        wstring(w, fn);
    }
    // Aliases.
    w.u16(1);
    w.u16(0);     // unused (format 2)
    w.u16(3);     // alias 3
    w.u32(0);     // this quest
    w.u16(5);
    w.u16(2);
    w.u16(1);
    wstring(w, "TestAliasScript");
    w.u8(0);
    w.u16(0);
    return w;
}

record::FormContext context() { return {.localized = false}; }

} // namespace

TEST_CASE("QUST fields go to the stage, objective or alias they follow", "[record][quest]") {
    ByteWriter p;
    zstring_field(p, "EDID", "TestQuest");
    write_field(p, "VMAD", quest_vmad());
    ByteWriter dnam;
    dnam.u16(0x0101); // start game enabled, run once
    dnam.u8(60);
    dnam.u8(0);
    dnam.u32(0);
    dnam.u32(2);
    write_field(p, "DNAM", dnam);
    condition(p, 1); // dialogue
    write_field(p, "NEXT", ByteWriter{});
    condition(p, 2); // event
    ByteWriter indx;
    indx.u16(10);
    indx.u8(0x02); // start-up stage
    indx.u8(0);
    write_field(p, "INDX", indx);
    ByteWriter qsdt;
    qsdt.u8(0x01); // completes the quest
    write_field(p, "QSDT", qsdt);
    condition(p, 3); // log entry
    zstring_field(p, "CNAM", "Found it.");
    ByteWriter qobj;
    qobj.u16(5);
    write_field(p, "QOBJ", qobj);
    u32_field(p, "FNAM", 1);
    zstring_field(p, "NNAM", "Find it");
    ByteWriter qsta;
    qsta.u32(3);
    qsta.u32(0);
    write_field(p, "QSTA", qsta);
    condition(p, 4); // target
    u32_field(p, "ANAM", 4);
    u32_field(p, "ALST", 3);
    zstring_field(p, "ALID", "Thing");
    u32_field(p, "FNAM", 0x0002); // optional
    u32_field(p, "ALFR", 0x00012345);
    condition(p, 5); // alias
    write_field(p, "ALED", ByteWriter{});
    u32_field(p, "ALLS", 4);
    zstring_field(p, "ALID", "Where");
    write_field(p, "ALED", ByteWriter{});

    io::SpanReader r{p.span(), "test"};
    const auto q = record::parse_quest(r, context());
    REQUIRE(q.has_value());
    CHECK(q->editor_id == "TestQuest");
    CHECK(q->has(record::Quest::Flag::start_game_enabled));
    CHECK(q->has(record::Quest::Flag::run_once));
    CHECK(q->priority == 60);
    CHECK(q->type == 2);
    REQUIRE(q->dialogue_conditions.raw.size() == 1);
    CHECK(q->dialogue_conditions.raw[0][0] == std::byte{1});
    REQUIRE(q->event_conditions.raw.size() == 1);
    CHECK(q->event_conditions.raw[0][0] == std::byte{2});

    REQUIRE(q->stages.size() == 1);
    CHECK(q->stages[0].index == 10);
    CHECK(q->stages[0].has(record::Quest::Stage::Flag::start_up));
    REQUIRE(q->stages[0].log.size() == 1);
    CHECK(q->stages[0].log[0].has(record::Quest::LogEntry::Flag::complete_quest));
    CHECK(q->stages[0].log[0].text.text == "Found it.");
    CHECK(q->stages[0].log[0].conditions.raw[0][0] == std::byte{3});

    REQUIRE(q->objectives.size() == 1);
    CHECK(q->objectives[0].index == 5);
    CHECK(q->objectives[0].flags == 1);
    CHECK(q->objectives[0].text.text == "Find it");
    REQUIRE(q->objectives[0].targets.size() == 1);
    CHECK(q->objectives[0].targets[0].alias == 3);
    CHECK(q->objectives[0].targets[0].conditions.raw[0][0] == std::byte{4});

    CHECK(q->next_alias_id == 4);
    REQUIRE(q->aliases.size() == 2);
    CHECK(q->aliases[0].id == 3);
    CHECK_FALSE(q->aliases[0].location);
    CHECK(q->aliases[0].name == "Thing");
    CHECK(q->aliases[0].has(record::Quest::Alias::Flag::optional));
    CHECK(q->aliases[0].forced_ref.value == 0x00012345u);
    CHECK(q->aliases[0].conditions.raw[0][0] == std::byte{5});
    CHECK(q->aliases[1].location);
    CHECK(q->aliases[1].name == "Where");

    CHECK(q->scripts.scripts.size() == 1);
    CHECK(q->fragments.script == "QF_TestQuest_01000D62");
    REQUIRE(q->fragments.fragments.size() == 2);
    CHECK(q->fragments.fragments[1].stage == 20);
    CHECK(q->fragments.fragments[1].function == "Fragment_1");
    REQUIRE(q->fragments.aliases.size() == 1);
    CHECK(q->fragments.aliases[0].alias.alias == 3);
    REQUIRE(q->fragments.aliases[0].scripts.size() == 1);
    CHECK(q->fragments.aliases[0].scripts[0].name == "TestAliasScript");
}

TEST_CASE("a QUST VMAD may end after its scripts", "[record][quest]") {
    ByteWriter vmad;
    vmad.u16(5);
    vmad.u16(2);
    vmad.u16(0);
    ByteWriter p;
    write_field(p, "VMAD", vmad);
    io::SpanReader r{p.span(), "test"};
    const auto q = record::parse_quest(r, context());
    REQUIRE(q.has_value());
    CHECK(q->fragments.fragments.empty());
    CHECK(q->fragments.aliases.empty());
}

TEST_CASE("quest fragments with no fragments still carry an empty name", "[record][quest]") {
    ByteWriter w;
    w.u8(2);
    w.u16(0);
    wstring(w, "");
    w.u16(0);
    io::SpanReader r{w.span(), "test"};
    record::ScriptData scripts;
    scripts.version = 5;
    scripts.object_format = 2;
    const auto f = record::read_quest_fragments(r, scripts);
    REQUIRE(f.has_value());
    CHECK(r.at_end());
}

TEST_CASE("truncated quest fragments fail rather than read past the field", "[record][quest]") {
    const auto full = quest_vmad();
    // Cut inside the alias block.
    const auto bytes = full.span().first(full.span().size() - 5);
    io::SpanReader r{bytes, "test"};
    auto scripts = record::read_script_data(r);
    REQUIRE(scripts.has_value());
    CHECK_FALSE(record::read_quest_fragments(r, *scripts).has_value());
}

TEST_CASE("an objective's FNAM is not taken for an alias's", "[record][quest]") {
    ByteWriter p;
    ByteWriter qobj;
    qobj.u16(1);
    write_field(p, "QOBJ", qobj);
    u32_field(p, "FNAM", 7);
    u32_field(p, "ALST", 0);
    u32_field(p, "FNAM", 9);
    write_field(p, "ALED", ByteWriter{});
    io::SpanReader r{p.span(), "test"};
    const auto q = record::parse_quest(r, context());
    REQUIRE(q.has_value());
    CHECK(q->objectives[0].flags == 7);
    CHECK(q->aliases[0].flags == 9);
}
