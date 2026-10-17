// SPDX-License-Identifier: GPL-3.0-or-later
//
// DLBR, DIAL, INFO, VTYP and RELA, and the dialogue part of world.fb (format
// 26). `bethconv forms` covers every real record; these cover the positional
// rules, the INFO fragment layout (checked against Skyrim.esm bytes) and the
// order of a topic's INFOs when a later plugin overrides and adds some.
#include "bethconv/pack/world.hpp"
#include "bethconv/record/forms_dialogue.hpp"
#include "bethconv/record/forms_game.hpp"
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

void zstring_field(ByteWriter& payload, std::string_view tag, std::string_view text) {
    ByteWriter w;
    w.zstring(text);
    write_field(payload, tag, w);
}

/// A CTDA: GetIsID (72) on `npc`, == 1.
void condition(ByteWriter& payload, std::uint32_t npc, std::uint8_t type = 0) {
    ByteWriter w;
    w.u8(type);
    w.u8(0);
    w.u16(0);
    w.f32(1.0F);
    w.u16(72);
    w.u16(0);
    w.u32(npc);
    w.u32(0);
    w.u32(0); // run on subject
    w.u32(0);
    w.u32(0xFFFFFFFF);
    write_field(payload, "CTDA", w);
}

/// The bytes of a real Skyrim.esm INFO VMAD (0x0001362F) without its
/// properties: version 5, one script, then the fragment block with an end
/// fragment only.
ByteWriter info_vmad(std::uint8_t flags) {
    ByteWriter w;
    w.u16(5);
    w.u16(2);
    w.u16(1);
    wstring(w, "TIF__0001362F");
    w.u8(0);
    w.u16(0);
    w.u8(2);
    w.u8(flags);
    wstring(w, "TIF__0001362F");
    if ((flags & 1) != 0) {
        w.u8(1);
        wstring(w, "TIF__0001362F");
        wstring(w, "Fragment_0");
    }
    if ((flags & 2) != 0) {
        w.u8(1);
        wstring(w, "TIF__0001362F");
        wstring(w, "Fragment_1");
    }
    return w;
}

ByteWriter trdt(std::uint8_t number, std::uint32_t sound) {
    ByteWriter w;
    w.u32(5);  // emotion
    w.u32(50); // value
    w.u32(0);
    w.u8(number);
    w.u8(0);
    w.u16(0);
    w.u32(sound);
    w.u8(1); // use emotion animation
    w.u8(0);
    w.u16(0);
    return w;
}

record::FormContext context() { return {.localized = false}; }

} // namespace

TEST_CASE("DLBR and DIAL are read field by field", "[record][dialogue]") {
    ByteWriter b;
    zstring_field(b, "EDID", "WIAddItem03SayBranch");
    u32_field(b, "QNAM", 0x00035B5A);
    u32_field(b, "TNAM", 1);
    u32_field(b, "DNAM", 3);
    u32_field(b, "SNAM", 0x0010FDFD);
    io::SpanReader br{b.span(), "test"};
    const auto branch = record::parse_dialogue_branch(br, context());
    REQUIRE(branch.has_value());
    CHECK(branch->quest.value == 0x00035B5A);
    CHECK(branch->start_topic.value == 0x0010FDFD);
    CHECK(branch->flags == 3);
    CHECK(branch->category == 1);

    ByteWriter t;
    ByteWriter pnam;
    pnam.f32(50.0F);
    write_field(t, "PNAM", pnam);
    u32_field(t, "QNAM", 0x0003372B);
    ByteWriter data;
    data.u8(1);
    data.u8(3);
    data.u16(0x17);
    write_field(t, "DATA", data);
    u32_field(t, "SNAM", 0x5F544948); // "HIT_"
    u32_field(t, "TIFC", 4);
    zstring_field(t, "FULL", "What do you sell?");
    io::SpanReader tr{t.span(), "test"};
    const auto topic = record::parse_dialogue_topic(tr, context());
    REQUIRE(topic.has_value());
    CHECK(topic->priority == 50.0F);
    CHECK(topic->flags == 1);
    CHECK(topic->category == 3);
    CHECK(topic->subtype == 0x17);
    CHECK(topic->subtype_code == io::FourCC{"HIT_"});
    CHECK(topic->text.text == "What do you sell?");
    CHECK(topic->info_count == 4);
}

TEST_CASE("INFO reads flags, conditions, responses and fragments", "[record][dialogue]") {
    ByteWriter p;
    zstring_field(p, "EDID", "TestInfo");
    ByteWriter enam;
    enam.u16(0x0005); // goodbye, say once
    enam.u16(48);     // reset hours
    write_field(p, "ENAM", enam);
    ByteWriter cnam;
    cnam.u8(2);
    write_field(p, "CNAM", cnam);
    write_field(p, "TRDT", trdt(1, 0x00ABCDEF));
    zstring_field(p, "NAM1", "Welcome to Riverwood.");
    zstring_field(p, "NAM2", "friendly");
    write_field(p, "NAM3", ByteWriter{});
    write_field(p, "TRDT", trdt(2, 0));
    zstring_field(p, "NAM1", "Second line.");
    zstring_field(p, "NAM2", "");
    write_field(p, "NAM3", ByteWriter{});
    condition(p, 0x00013475, 0x01);
    condition(p, 0x00013476);
    u32_field(p, "TCLT", 0x100);
    u32_field(p, "TCLT", 0x200);
    u32_field(p, "ANAM", 0x13475);
    u32_field(p, "DNAM", 0x300);
    u32_field(p, "TWAT", 0x400);
    u32_field(p, "ONAM", 0x500);
    zstring_field(p, "RNAM", "Prompt");
    write_field(p, "VMAD", info_vmad(3));

    io::SpanReader r{p.span(), "test"};
    const auto info = record::parse_dialogue_info(r, context());
    REQUIRE(info.has_value());
    CHECK(info->flags == 5);
    CHECK(info->reset_hours == 48);
    CHECK(info->favor_level == 2);
    CHECK(info->prompt.text == "Prompt");
    REQUIRE(info->responses.size() == 2);
    CHECK(info->responses[0].text.text == "Welcome to Riverwood.");
    CHECK(info->responses[0].notes == "friendly");
    CHECK(info->responses[0].emotion == 5);
    CHECK(info->responses[0].emotion_value == 50);
    CHECK(info->responses[0].number == 1);
    CHECK(info->responses[0].sound.value == 0x00ABCDEF);
    CHECK(info->responses[0].flags == 1);
    CHECK(info->responses[1].number == 2);
    REQUIRE(info->conditions.size() == 2);
    CHECK(info->conditions[0].function == 72);
    CHECK(info->conditions[0].type == 1);
    CHECK(info->conditions[1].param1 == 0x00013476);
    REQUIRE(info->links.size() == 2);
    CHECK(info->links[1].value == 0x200);
    CHECK(info->speaker.value == 0x13475);
    CHECK(info->shared_info.value == 0x300);
    CHECK(info->walk_away_topic.value == 0x400);
    CHECK(info->audio_output.value == 0x500);
    REQUIRE(info->scripts.scripts.size() == 1);
    CHECK(info->fragments.script == "TIF__0001362F");
    CHECK(info->fragments.begin_function == "Fragment_0");
    CHECK(info->fragments.end_function == "Fragment_1");
}

TEST_CASE("INFO fragments follow the flags byte", "[record][vmad][dialogue]") {
    for (const std::uint8_t flags : {std::uint8_t{1}, std::uint8_t{2}}) {
        const auto vmad = info_vmad(flags);
        io::SpanReader r{vmad.span(), "test"};
        const auto scripts = record::read_script_data(r);
        REQUIRE(scripts.has_value());
        const auto fragments = record::read_info_fragments(r);
        REQUIRE(fragments.has_value());
        CHECK(r.at_end());
        CHECK(fragments->begin_function == (flags == 1 ? "Fragment_0" : ""));
        CHECK(fragments->end_function == (flags == 2 ? "Fragment_1" : ""));
    }
}

TEST_CASE("VTYP and RELA", "[record][dialogue]") {
    ByteWriter v;
    zstring_field(v, "EDID", "MaleNord");
    ByteWriter dnam;
    dnam.u8(1);
    write_field(v, "DNAM", dnam);
    io::SpanReader vr{v.span(), "test"};
    const auto voice = record::parse_voice_type(vr, context());
    REQUIRE(voice.has_value());
    CHECK(voice->editor_id == "MaleNord");
    CHECK(voice->flags == 1);

    // Bytes of a real RELA (IngeAtAfAlan): parent, child, rank 2, association.
    ByteWriter r;
    zstring_field(r, "EDID", "IngeAtAfAlan");
    ByteWriter data;
    data.u32(0x0001328A);
    data.u32(0x00013295);
    data.u32(2);
    data.u32(0x0001E74F);
    write_field(r, "DATA", data);
    io::SpanReader rr{r.span(), "test"};
    const auto rela = record::parse_relationship(rr, context());
    REQUIRE(rela.has_value());
    CHECK(rela->parent.value == 0x0001328A);
    CHECK(rela->child.value == 0x00013295);
    CHECK(rela->rank == 2);
    CHECK(rela->association.value == 0x0001E74F);
}

namespace {

void save(const TempDir& dir, std::string_view name, const ByteWriter& file) {
    std::ofstream out(dir / name, std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

std::uint32_t label(std::string_view type) {
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        raw |= static_cast<std::uint32_t>(static_cast<unsigned char>(type[i])) << (i * 8);
    }
    return raw;
}

void top_group(ByteWriter& file, std::string_view type, const ByteWriter& children) {
    write_group(file, label(type), 0, children.span());
}

ByteWriter info_payload(std::string_view text, std::uint32_t npc) {
    ByteWriter p;
    ByteWriter enam;
    enam.u32(0);
    write_field(p, "ENAM", enam);
    write_field(p, "TRDT", trdt(1, 0));
    zstring_field(p, "NAM1", text);
    write_field(p, "NAM2", ByteWriter{});
    condition(p, npc);
    return p;
}

} // namespace

TEST_CASE("world.fb carries dialogue, keeping the base plugin's INFO order",
          "[pack][world][dialogue]") {
    const TempDir dir;
    // Base.esm: the NPC and its voice type, a quest with CTDAs, a topic with
    // two INFOs in the order A, B.
    {
        ByteWriter file;
        write_tes4(file, 0, {});
        ByteWriter vtyp;
        zstring_field(vtyp, "EDID", "MaleNord");
        ByteWriter vd;
        vd.u8(1);
        write_field(vtyp, "DNAM", vd);
        ByteWriter group;
        write_record(group, "VTYP", 0x0000'0A00, vtyp.span());
        top_group(file, "VTYP", group);

        ByteWriter lctn;
        zstring_field(lctn, "EDID", "RiverwoodLocation");
        zstring_field(lctn, "FULL", "Riverwood");
        ByteWriter locs;
        write_record(locs, "LCTN", 0x0000'0A10, lctn.span());
        top_group(file, "LCTN", locs);

        ByteWriter fact;
        zstring_field(fact, "EDID", "VendorFaction");
        u32_field(fact, "DATA", 0x4000);
        ByteWriter venv;
        venv.u16(8);
        venv.u16(20);
        venv.u32(500);
        venv.u32(0);
        write_field(fact, "VENV", venv);
        u32_field(fact, "VENC", 0x0000'0A20);
        ByteWriter facts;
        write_record(facts, "FACT", 0x0000'0A30, fact.span());
        top_group(file, "FACT", facts);

        ByteWriter npc;
        zstring_field(npc, "EDID", "Lucan");
        u32_field(npc, "VTCK", 0x0000'0A00);
        ByteWriter npcs;
        write_record(npcs, "NPC_", 0x0000'0900, npc.span());
        top_group(file, "NPC_", npcs);

        ByteWriter rela;
        zstring_field(rela, "EDID", "LucanAndCamilla");
        ByteWriter rd;
        rd.u32(0x0000'0900);
        rd.u32(0x0000'0901);
        rd.u32(3);
        rd.u32(0x0000'0A40);
        write_field(rela, "DATA", rd);
        ByteWriter relas;
        write_record(relas, "RELA", 0x0000'0A50, rela.span());
        top_group(file, "RELA", relas);

        ByteWriter q;
        zstring_field(q, "EDID", "TestQuest");
        condition(q, 0x0000'0900); // dialogue
        write_field(q, "NEXT", ByteWriter{});
        condition(q, 0x0000'0901); // event
        ByteWriter indx;
        indx.u16(10);
        indx.u8(0);
        indx.u8(0);
        write_field(q, "INDX", indx);
        ByteWriter qsdt;
        qsdt.u8(0);
        write_field(q, "QSDT", qsdt);
        condition(q, 0x0000'0902); // log entry
        zstring_field(q, "CNAM", "Begun.");
        ByteWriter quests;
        write_record(quests, "QUST", 0x0000'0B00, q.span());
        top_group(file, "QUST", quests);

        ByteWriter br;
        zstring_field(br, "EDID", "TestBranch");
        u32_field(br, "QNAM", 0x0000'0B00);
        u32_field(br, "TNAM", 0);
        u32_field(br, "DNAM", 1);
        u32_field(br, "SNAM", 0x0000'0C00);
        ByteWriter branches;
        write_record(branches, "DLBR", 0x0000'0B80, br.span());
        top_group(file, "DLBR", branches);

        ByteWriter dial;
        zstring_field(dial, "EDID", "TestGreeting");
        u32_field(dial, "QNAM", 0x0000'0B00);
        u32_field(dial, "BNAM", 0x0000'0B80);
        ByteWriter dd;
        dd.u8(0);
        dd.u8(0);
        dd.u16(0);
        write_field(dial, "DATA", dd);
        u32_field(dial, "SNAM", 0x45455247); // "GREE"
        ByteWriter infos;
        write_record(infos, "INFO", 0x0000'0C10, info_payload("A", 0x900).span());
        write_record(infos, "INFO", 0x0000'0C20, info_payload("B", 0x900).span());
        ByteWriter dials;
        write_record(dials, "DIAL", 0x0000'0C00, dial.span());
        write_group(dials, 0x0000'0C00, 7, infos.span());
        top_group(file, "DIAL", dials);
        save(dir, "Base.esm", file);
    }
    // Over.esp: overrides B with other text and adds C and D, written D first.
    {
        ByteWriter file;
        write_tes4(file, 0, {"Base.esm"});
        ByteWriter infos;
        write_record(infos, "INFO", 0x0000'0C20, info_payload("B2", 0x900).span());
        write_record(infos, "INFO", 0x0100'0D20, info_payload("D", 0x900).span());
        write_record(infos, "INFO", 0x0100'0D10, info_payload("C", 0x900).span());
        ByteWriter dials;
        write_group(dials, 0x0000'0C00, 7, infos.span());
        top_group(file, "DIAL", dials);
        save(dir, "Over.esp", file);
    }
    record::PluginList list;
    for (const char* name : {"Base.esm", "Over.esp"}) {
        list.plugins.push_back(record::ListedPlugin{.name = name, .active = true});
    }
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->unresolved == 0);
    CHECK(stats->dialogue_topics == 1);
    CHECK(stats->dialogue_infos == 4);
    CHECK(stats->orphan_infos == 0);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());
    const auto topic = file->dialogue_topic(0x0000'0C00);
    REQUIRE(topic.has_value());
    CHECK(topic->editor_id == "TestGreeting");
    CHECK(topic->branch == 0x0000'0B80);
    CHECK(topic->subtype_code == io::FourCC{"GREE"}.value);
    REQUIRE(topic->infos.size() == 4);
    const char* expected[] = {"A", "B2", "D", "C"}; // base order, then group order
    for (std::size_t i = 0; i < 4; ++i) {
        REQUIRE(topic->infos[i]->responses.size() == 1);
        CHECK(topic->infos[i]->responses[0]->text == expected[i]);
        REQUIRE(topic->infos[i]->conditions.size() == 1);
        CHECK(topic->infos[i]->conditions[0]->param1 == 0x0000'0900);
    }
    CHECK(topic->infos[0]->responses[0]->number == 1);
    const auto branch = file->dialogue_branch(0x0000'0B80);
    REQUIRE(branch.has_value());
    CHECK(branch->quest == 0x0000'0B00);
    CHECK(branch->start_topic == 0x0000'0C00);
    CHECK(branch->flags == pack::wfb::DialogueBranchFlags::top_level);

    const auto quest = file->quest(0x0000'0B00);
    REQUIRE(quest.has_value());
    REQUIRE(quest->dialogue_conditions.size() == 1);
    CHECK(quest->dialogue_conditions[0]->param1 == 0x0000'0900);
    REQUIRE(quest->event_conditions.size() == 1);
    CHECK(quest->event_conditions[0]->param1 == 0x0000'0901);
    REQUIRE(quest->stages.size() == 1);
    CHECK(quest->stages[0]->log[0]->conditions == 1);
    REQUIRE(quest->stages[0]->log[0]->condition_list.size() == 1);
    CHECK(quest->stages[0]->log[0]->condition_list[0]->param1 == 0x0000'0902);

    CHECK(file->voice_type(0x0000'0A00)->editor_id == "MaleNord");
    CHECK(file->location(0x0000'0A10)->name == "Riverwood");
    const auto faction = file->faction(0x0000'0A30);
    REQUIRE(faction.has_value());
    CHECK(faction->vendor_start_hour == 8);
    CHECK(faction->vendor_end_hour == 20);
    CHECK(faction->vendor_radius == 500);
    CHECK(faction->vendor_chest == 0x0000'0A20);
    CHECK(file->npc(0x0000'0900)->voice_type == 0x0000'0A00);
    const auto relationships = file->relationships();
    REQUIRE(relationships.size() == 1);
    CHECK(relationships[0].rank == 3);
    CHECK(relationships[0].parent == 0x0000'0900);
}
