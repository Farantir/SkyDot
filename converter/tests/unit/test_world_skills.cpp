// SPDX-License-Identifier: GPL-3.0-or-later
//
// world.fb 38: perks (PERK) with their effects, condition tabs and entry point data, actor values
// (AVIF) with skill data and perk trees, classes (CLAS), the skill or spell a BOOK teaches, and
// the class and perks of an NPC_.
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

void u32(ByteWriter& payload, std::string_view tag, std::uint32_t value) {
    ByteWriter w;
    w.u32(value);
    bethconv::test::write_field(payload, tag, w);
}

void u8(ByteWriter& payload, std::string_view tag, std::uint8_t value) {
    ByteWriter w;
    w.u8(value);
    bethconv::test::write_field(payload, tag, w);
}

void record(ByteWriter& file, std::string_view type, std::uint32_t id, const ByteWriter& payload) {
    ByteWriter rec;
    bethconv::test::write_record(rec, type, id, payload.span());
    bethconv::test::write_group(file, tag_label(type), 0, rec.span());
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
    c.u32(0);
    c.u32(0);
    c.u32(0xFFFF'FFFFU);
    bethconv::test::write_field(payload, "CTDA", c);
}

/// PRKE (type, rank, priority) and the DATA of an entry point effect.
void entry_point(ByteWriter& perk, std::uint8_t priority, std::uint8_t point, std::uint8_t function,
                 std::uint8_t tabs) {
    ByteWriter prke;
    prke.u8(2);
    prke.u8(0);
    prke.u8(priority);
    bethconv::test::write_field(perk, "PRKE", prke);
    ByteWriter d;
    d.u8(point);
    d.u8(function);
    d.u8(tabs);
    bethconv::test::write_field(perk, "DATA", d);
}

void make_skills(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0, {});

    ByteWriter perk;
    text(perk, "EDID", "TestArmsman");
    text(perk, "FULL", "Armsman");
    text(perk, "DESC", "More damage.");
    ctda(perk, 448, 1.0F, 0xB10); // a prerequisite
    {
        ByteWriter d;
        d.u8(0);
        d.u8(30);
        d.u8(2);
        d.u8(1);
        d.u8(0);
        bethconv::test::write_field(perk, "DATA", d);
    }
    u32(perk, "NNAM", 0xB02);
    // Effect 0: multiply by a float, two condition tabs.
    entry_point(perk, 3, 0x23, 3, 2);
    u8(perk, "PRKC", 0);
    ctda(perk, 448, 0.0F, 0xB02);
    u8(perk, "PRKC", 1);
    ctda(perk, 448, 1.0F, 0xB11);
    ctda(perk, 448, 1.0F, 0xB12);
    u8(perk, "EPFT", 1);
    {
        ByteWriter d;
        d.f32(1.2F);
        bethconv::test::write_field(perk, "EPFD", d);
    }
    bethconv::test::write_field(perk, "PRKF", ByteWriter{});
    // Effect 1: an actor value (stored as a float) and a float.
    entry_point(perk, 22, 0x42, 14, 1);
    u8(perk, "EPFT", 2);
    {
        ByteWriter d;
        d.f32(106.0F);
        d.f32(0.01F);
        bethconv::test::write_field(perk, "EPFD", d);
    }
    bethconv::test::write_field(perk, "PRKF", ByteWriter{});
    // Effect 2: select a spell.
    entry_point(perk, 0, 0x33, 10, 3);
    u8(perk, "EPFT", 5);
    u32(perk, "EPFD", 0xB20);
    bethconv::test::write_field(perk, "PRKF", ByteWriter{});
    // Effect 3: an ability.
    {
        ByteWriter prke;
        prke.u8(1);
        prke.u8(1);
        prke.u8(0);
        bethconv::test::write_field(perk, "PRKE", prke);
        u32(perk, "DATA", 0xB21);
        bethconv::test::write_field(perk, "PRKF", ByteWriter{});
    }
    // Effect 4: a quest stage.
    {
        ByteWriter prke;
        prke.u8(0);
        prke.u8(0);
        prke.u8(0);
        bethconv::test::write_field(perk, "PRKE", prke);
        ByteWriter d;
        d.u32(0xB22);
        d.u8(40);
        d.u8(0);
        d.u16(0);
        bethconv::test::write_field(perk, "DATA", d);
        bethconv::test::write_field(perk, "PRKF", ByteWriter{});
    }
    // Effect 5: a string.
    entry_point(perk, 0, 0x36, 11, 1);
    u8(perk, "EPFT", 6);
    text(perk, "EPFD", "PerkShieldCharge");
    bethconv::test::write_field(perk, "PRKF", ByteWriter{});
    record(file, "PERK", 0xB01, perk);

    ByteWriter perk2;
    text(perk2, "EDID", "TestArmsman2");
    {
        ByteWriter d;
        d.u8(1);
        d.u8(0);
        d.u8(1);
        d.u8(0);
        d.u8(1);
        bethconv::test::write_field(perk2, "DATA", d);
    }
    record(file, "PERK", 0xB02, perk2);

    // The skill's node data: a root and one perk node it leads to.
    ByteWriter avif;
    text(avif, "EDID", "AVOneHanded");
    text(avif, "FULL", "One-handed");
    text(avif, "DESC", "Swords.");
    text(avif, "ANAM", "1H");
    u32(avif, "CNAM", 1);
    {
        ByteWriter d;
        d.f32(6.3F);
        d.f32(0.5F);
        d.f32(2.0F);
        d.f32(10.0F);
        bethconv::test::write_field(avif, "AVSK", d);
    }
    const auto node = [&](std::uint32_t perk_id, std::uint32_t index, std::int32_t gx,
                          std::int32_t gy, std::initializer_list<std::uint32_t> links) {
        u32(avif, "PNAM", perk_id);
        u32(avif, "FNAM", perk_id != 0 ? 1U : 0x3F800000U);
        u32(avif, "XNAM", static_cast<std::uint32_t>(gx));
        u32(avif, "YNAM", static_cast<std::uint32_t>(gy));
        ByteWriter h;
        h.f32(0.25F);
        bethconv::test::write_field(avif, "HNAM", h);
        ByteWriter v;
        v.f32(-0.5F);
        bethconv::test::write_field(avif, "VNAM", v);
        u32(avif, "SNAM", 0xB30);
        for (const auto link : links) {
            u32(avif, "CNAM", link);
        }
        u32(avif, "INAM", index);
    };
    node(0, 0, 1, 1, {7});
    node(0xB01, 7, 2, 0, {});
    record(file, "AVIF", 0xB30, avif);

    ByteWriter mysticism;
    text(mysticism, "EDID", "AVMysticism");
    record(file, "AVIF", 0xB31, mysticism);

    ByteWriter clas;
    text(clas, "EDID", "TrainerSmithing");
    text(clas, "FULL", "Blacksmith");
    {
        ByteWriter d;
        d.u32(0xFFFC'0003U);
        d.u8(4); // Smithing, counted from One-Handed
        d.u8(50);
        for (std::uint8_t i = 0; i < 18; ++i) {
            d.u8(i);
        }
        d.f32(0.1F);
        d.u32(7);
        d.u8(2);
        d.u8(1);
        d.u8(3);
        d.u8(0);
        bethconv::test::write_field(clas, "DATA", d);
    }
    record(file, "CLAS", 0xB40, clas);

    ByteWriter vendor;
    text(vendor, "EDID", "VendorSmith");
    {
        ByteWriter d;
        d.u32(0xFFFC'0003U);
        d.u8(0);
        d.u8(0);
        for (std::uint8_t i = 0; i < 18; ++i) {
            d.u8(0);
        }
        d.f32(0.1F);
        d.u32(0);
        d.u8(1);
        d.u8(1);
        d.u8(1);
        d.u8(0);
        bethconv::test::write_field(vendor, "DATA", d);
    }
    record(file, "CLAS", 0xB41, vendor);

    const auto book = [&](std::uint32_t id, std::string_view edid, std::uint8_t flags,
                          std::uint32_t teaches) {
        ByteWriter b;
        text(b, "EDID", edid);
        ByteWriter d;
        d.u8(flags);
        d.u8(0);
        d.u16(0);
        d.u32(teaches);
        d.u32(50);
        d.f32(1.0F);
        bethconv::test::write_field(b, "DATA", d);
        record(file, "BOOK", id, b);
    };
    book(0xB50, "SkillBook", 0x1, 8); // Marksman
    book(0xB51, "SpellTome", 0x4, 0xB20);
    book(0xB52, "PlainBook", 0x0, 0);

    ByteWriter npc;
    text(npc, "EDID", "Smith");
    u32(npc, "CNAM", 0xB40);
    u32(npc, "PRKZ", 2);
    for (const std::uint32_t id : {0xB01U, 0xB02U}) {
        ByteWriter w;
        w.u32(id);
        w.u32(id == 0xB01U ? 1U : 2U);
        bethconv::test::write_field(npc, "PRKR", w);
    }
    record(file, "NPC_", 0xB60, npc);

    std::ofstream out(dir / "Skills.esm", std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

} // namespace

TEST_CASE("world.fb 38 carries perks, actor values, classes, taught books and NPC perks",
          "[pack][world][skills]") {
    const TempDir dir;
    make_skills(dir);
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "Skills.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->perks == 2);
    CHECK(stats->actor_value_infos == 2);
    CHECK(stats->classes == 2);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());

    const auto perk = file->perk(0xB01);
    REQUIRE(perk.has_value());
    CHECK(perk->editor_id == "TestArmsman");
    CHECK(perk->name == "Armsman");
    CHECK(perk->description == "More damage.");
    CHECK_FALSE(perk->trait);
    CHECK(perk->level == 30);
    CHECK(perk->ranks == 2);
    CHECK(perk->playable);
    CHECK_FALSE(perk->hidden);
    CHECK(perk->next_perk == 0xB02);
    REQUIRE(perk->conditions.size() == 1);
    CHECK(perk->conditions[0]->function == 448);
    CHECK(perk->conditions[0]->param1 == 0xB10);
    REQUIRE(perk->effects.size() == 6);

    const auto& e0 = *perk->effects[0];
    CHECK(e0.kind == pack::wfb::PerkEffectKind::entry_point);
    CHECK(e0.priority == 3);
    CHECK(e0.entry_point == 0x23);
    CHECK(e0.function == 3);
    CHECK(e0.tab_count == 2);
    REQUIRE(e0.tabs.size() == 2);
    CHECK(e0.tabs[0]->tab == 0);
    CHECK(e0.tabs[0]->conditions.size() == 1);
    CHECK(e0.tabs[1]->tab == 1);
    REQUIRE(e0.tabs[1]->conditions.size() == 2);
    CHECK(e0.tabs[1]->conditions[1]->param1 == 0xB12);
    CHECK(e0.data_type == 1);
    REQUIRE(e0.values.size() == 1);
    CHECK(e0.values[0] == 1.2F);

    const auto& e1 = *perk->effects[1];
    CHECK(e1.entry_point == 0x42);
    CHECK(e1.function == 14);
    CHECK(e1.data_type == 2);
    CHECK(e1.actor_value == 106);
    REQUIRE(e1.values.size() == 1);
    CHECK(e1.values[0] == 0.01F);
    CHECK(e1.tabs.empty());

    const auto& e2 = *perk->effects[2];
    CHECK(e2.data_type == 5);
    CHECK(e2.form == 0xB20);
    CHECK(e2.actor_value == -1);

    const auto& e3 = *perk->effects[3];
    CHECK(e3.kind == pack::wfb::PerkEffectKind::ability);
    CHECK(e3.rank == 1);
    CHECK(e3.spell == 0xB21);

    const auto& e4 = *perk->effects[4];
    CHECK(e4.kind == pack::wfb::PerkEffectKind::quest);
    CHECK(e4.quest == 0xB22);
    CHECK(e4.stage == 40);

    const auto& e5 = *perk->effects[5];
    CHECK(e5.data_type == 6);
    CHECK(e5.text == "PerkShieldCharge");

    const auto perk2 = file->perk(0xB02);
    REQUIRE(perk2.has_value());
    CHECK(perk2->trait);
    CHECK(perk2->hidden);
    CHECK_FALSE(perk2->playable);
    CHECK(perk2->effects.empty());

    const auto skill = file->actor_value_info_of(6);
    REQUIRE(skill.has_value());
    CHECK(skill->id == 0xB30);
    CHECK(skill->actor_value == 6);
    CHECK(skill->name == "One-handed");
    CHECK(skill->description == "Swords.");
    CHECK(skill->abbreviation == "1H");
    CHECK(skill->category == 1);
    CHECK(skill->has_skill);
    CHECK(skill->use_mult == 6.3F);
    CHECK(skill->use_offset == 0.5F);
    CHECK(skill->improve_mult == 2.0F);
    CHECK(skill->improve_offset == 10.0F);
    REQUIRE(skill->perk_tree.size() == 2);
    CHECK(skill->perk_tree[0]->perk == 0);
    CHECK(skill->perk_tree[0]->flags == 0x3F800000U);
    REQUIRE(skill->perk_tree[0]->links.size() == 1);
    CHECK(skill->perk_tree[0]->links[0] == 7);
    CHECK(skill->perk_tree[1]->index == 7);
    CHECK(skill->perk_tree[1]->perk == 0xB01);
    CHECK(skill->perk_tree[1]->grid_x == 2);
    CHECK(skill->perk_tree[1]->grid_y == 0);
    CHECK(skill->perk_tree[1]->x == 0.25F);
    CHECK(skill->perk_tree[1]->y == -0.5F);
    CHECK(skill->perk_tree[1]->skill == 0xB30);
    CHECK(skill->perk_tree[1]->links.empty());

    const auto illusion = file->actor_value_info_of(21);
    REQUIRE(illusion.has_value());
    CHECK(illusion->id == 0xB31);
    CHECK_FALSE(illusion->has_skill);
    CHECK_FALSE(file->actor_value_info_of(7).has_value());

    const auto trainer = file->actor_class(0xB40);
    REQUIRE(trainer.has_value());
    CHECK(trainer->name == "Blacksmith");
    CHECK(trainer->teaches == 10);
    CHECK(trainer->max_training_level == 50);
    REQUIRE(trainer->skill_weights.size() == 18);
    CHECK(trainer->skill_weights[17] == 17);
    CHECK(trainer->health_weight == 2);
    CHECK(trainer->magicka_weight == 1);
    CHECK(trainer->stamina_weight == 3);
    CHECK(trainer->bleedout_default == 0.1F);
    CHECK(trainer->voice_points == 7);
    const auto vendor = file->actor_class(0xB41);
    REQUIRE(vendor.has_value());
    CHECK(vendor->teaches == -1);
    CHECK(vendor->max_training_level == 0);

    const auto skill_book = file->base(0xB50);
    REQUIRE((skill_book && skill_book->item));
    CHECK(skill_book->item->book_flags == 1);
    CHECK(skill_book->item->book_skill == 8);
    CHECK(skill_book->item->book_spell == 0);
    const auto tome = file->base(0xB51);
    REQUIRE((tome && tome->item));
    CHECK(tome->item->book_flags == 4);
    CHECK(tome->item->book_skill == -1);
    CHECK(tome->item->book_spell == 0xB20);
    const auto plain = file->base(0xB52);
    REQUIRE((plain && plain->item));
    CHECK(plain->item->book_flags == 0);
    CHECK(plain->item->book_skill == -1);

    const auto npc = file->npc(0xB60);
    REQUIRE(npc.has_value());
    CHECK(npc->class_id == 0xB40);
    REQUIRE(npc->perks.size() == 2);
    CHECK(npc->perks[0].perk() == 0xB01);
    CHECK(npc->perks[0].rank() == 1);
    CHECK(npc->perks[1].perk() == 0xB02);
    CHECK(npc->perks[1].rank() == 2);
}
