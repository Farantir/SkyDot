// SPDX-License-Identifier: GPL-3.0-or-later
//
// world.fb 28: ItemData of every carriable base, the start inventory and
// respawn flag of a CONT, LVSP and LVLG.
#include "bethconv/pack/world.hpp"

#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"

#include "../support/esm_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <fstream>
#include <string>
#include <string_view>

using namespace bethconv;
using bethconv::test::ByteWriter;
using bethconv::test::TempDir;
using pack::wfb::ItemCategory;

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

/// One record in its own top group.
void record(ByteWriter& file, std::string_view type, std::uint32_t id, const ByteWriter& payload,
            std::uint32_t flags = 0) {
    ByteWriter rec;
    bethconv::test::write_record(rec, type, id, payload.span(), flags);
    bethconv::test::write_group(file, tag_label(type), 0, rec.span());
}

ByteWriter value_weight(std::int32_t value, float weight) {
    ByteWriter w;
    w.u32(static_cast<std::uint32_t>(value));
    w.f32(weight);
    return w;
}

ByteWriter body(std::uint32_t slots, std::uint32_t type) {
    ByteWriter w;
    w.u32(slots);
    w.u32(type);
    return w;
}

void make_items(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0, {});

    ByteWriter misc;
    text(misc, "EDID", "TestGold");
    text(misc, "FULL", "Gold");
    bethconv::test::write_field(misc, "DATA", value_weight(1, 0.0F));
    record(file, "MISC", 0x801, misc);

    ByteWriter sword;
    text(sword, "EDID", "TestSword");
    text(sword, "FULL", "Test Sword");
    text(sword, "MODL", "Weapons\\Sword.nif");
    u32(sword, "ETYP", 0x13F44);
    {
        ByteWriter data;
        data.u32(25);
        data.f32(9.0F);
        data.u16(7);
        bethconv::test::write_field(sword, "DATA", data);
        ByteWriter dnam;
        dnam.u8(1); // one-handed sword
        for (int i = 1; i < 100; ++i) {
            dnam.u8(0);
        }
        bethconv::test::write_field(sword, "DNAM", dnam);
    }
    record(file, "WEAP", 0x802, sword);

    ByteWriter bow;
    text(bow, "EDID", "TestBow");
    u32(bow, "ETYP", 0x13F45);
    {
        ByteWriter data;
        data.u32(30);
        data.f32(5.0F);
        data.u16(9);
        bethconv::test::write_field(bow, "DATA", data);
        ByteWriter dnam;
        dnam.u8(7);
        for (int i = 1; i < 100; ++i) {
            dnam.u8(0);
        }
        bethconv::test::write_field(bow, "DNAM", dnam);
    }
    record(file, "WEAP", 0x803, bow, 0x4); // non-playable

    const auto armor = [&](std::uint32_t id, std::string_view edid, std::uint32_t slots,
                           std::uint32_t type, std::uint32_t rating) {
        ByteWriter a;
        text(a, "EDID", edid);
        bethconv::test::write_field(a, "BOD2", body(slots, type));
        u32(a, "ETYP", 0);
        ByteWriter data;
        data.u32(100);
        data.f32(28.0F);
        bethconv::test::write_field(a, "DATA", data);
        u32(a, "DNAM", rating);
        record(file, "ARMO", id, a);
    };
    armor(0x804, "TestCuirass", 0x4, 1, 3600);
    armor(0x805, "TestRing", 1U << 6, 2, 0);
    armor(0x806, "TestRobes", 0x4, 2, 0);
    armor(0x807, "TestHelmet", 1U << 1, 0, 1500);

    const auto ingestible = [&](std::uint32_t id, std::string_view edid, std::uint32_t flags) {
        ByteWriter a;
        text(a, "EDID", edid);
        ByteWriter data;
        data.f32(0.5F);
        bethconv::test::write_field(a, "DATA", data);
        ByteWriter enit;
        enit.u32(40);
        enit.u32(flags);
        enit.u32(0);
        enit.u32(0);
        enit.u32(0);
        bethconv::test::write_field(a, "ENIT", enit);
        record(file, "ALCH", id, a);
    };
    ingestible(0x808, "TestPotion", 0);
    ingestible(0x809, "TestFood", 2);

    ByteWriter book;
    text(book, "EDID", "TestBook");
    {
        ByteWriter data;
        data.u8(0);
        data.u8(0);
        data.u16(0);
        data.u32(0);
        data.u32(15);
        data.f32(1.0F);
        bethconv::test::write_field(book, "DATA", data);
    }
    record(file, "BOOK", 0x80A, book);

    ByteWriter ingr;
    text(ingr, "EDID", "TestIngredient");
    bethconv::test::write_field(ingr, "DATA", value_weight(3, 0.25F));
    record(file, "INGR", 0x80B, ingr);

    ByteWriter slgm;
    text(slgm, "EDID", "TestSoulGem");
    bethconv::test::write_field(slgm, "DATA", value_weight(60, 1.0F));
    {
        ByteWriter b;
        b.u8(3);
        bethconv::test::write_field(slgm, "SOUL", b);
        ByteWriter c;
        c.u8(5);
        bethconv::test::write_field(slgm, "SLCP", c);
    }
    u32(slgm, "NAM0", 0x80D);
    record(file, "SLGM", 0x80C, slgm, 0x20000); // can hold NPC soul

    ByteWriter scrl;
    text(scrl, "EDID", "TestScroll");
    bethconv::test::write_field(scrl, "DATA", value_weight(20, 0.5F));
    record(file, "SCRL", 0x80D, scrl);

    ByteWriter key;
    text(key, "EDID", "TestKey");
    bethconv::test::write_field(key, "DATA", value_weight(0, 0.0F));
    record(file, "KEYM", 0x80E, key);

    ByteWriter ammo;
    text(ammo, "EDID", "TestArrow");
    {
        ByteWriter data;
        data.u32(0);
        data.u32(2); // non-playable
        data.f32(8.0F);
        data.u32(1);
        data.f32(0.1F);
        bethconv::test::write_field(ammo, "DATA", data);
    }
    record(file, "AMMO", 0x80F, ammo);

    ByteWriter torch;
    text(torch, "EDID", "TestTorch");
    text(torch, "FULL", "Torch");
    {
        ByteWriter data;
        data.u32(0xFFFFFFFFU);
        data.u32(256);
        data.u32(0x00FFFFFF);
        data.u32(0x2); // can be carried
        for (int i = 0; i < 6; ++i) {
            data.f32(1.0F);
        }
        data.u32(4);
        data.f32(1.0F);
        bethconv::test::write_field(torch, "DATA", data);
        ByteWriter fnam;
        fnam.f32(1.0F);
        bethconv::test::write_field(torch, "FNAM", fnam);
    }
    record(file, "LIGH", 0x810, torch);

    const auto leveled = [&](std::string_view type, std::uint32_t id, std::string_view edid,
                             std::uint32_t global) {
        ByteWriter l;
        text(l, "EDID", edid);
        ByteWriter chance;
        chance.u8(25);
        bethconv::test::write_field(l, "LVLD", chance);
        ByteWriter flags;
        flags.u8(3);
        bethconv::test::write_field(l, "LVLF", flags);
        if (global != 0) {
            u32(l, "LVLG", global);
        }
        ByteWriter count;
        count.u8(2);
        bethconv::test::write_field(l, "LLCT", count);
        for (const std::uint32_t form : {0x802U, 0x804U}) {
            ByteWriter lvlo;
            lvlo.u16(form == 0x802U ? 1 : 5);
            lvlo.u16(0);
            lvlo.u32(form);
            lvlo.u16(form == 0x802U ? 1 : 2);
            lvlo.u16(0);
            bethconv::test::write_field(l, "LVLO", lvlo);
        }
        record(file, type, id, l);
    };
    leveled("LVLI", 0x811, "TestLoot", 0x813);
    leveled("LVSP", 0x812, "TestSpells", 0);

    ByteWriter cont;
    text(cont, "EDID", "TestChest");
    text(cont, "FULL", "Chest");
    text(cont, "MODL", "Containers\\Chest.nif");
    {
        ByteWriter data;
        data.u8(2); // respawns
        data.f32(0.0F);
        bethconv::test::write_field(cont, "DATA", data);
        u32(cont, "COCT", 2);
        for (const auto& [form, count] : {std::pair<std::uint32_t, std::uint32_t>{0x801, 12},
                                          std::pair<std::uint32_t, std::uint32_t>{0x811, 3}}) {
            ByteWriter cnto;
            cnto.u32(form);
            cnto.u32(count);
            bethconv::test::write_field(cont, "CNTO", cnto);
        }
    }
    record(file, "CONT", 0x814, cont);

    std::ofstream out(dir / "Items.esm", std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

} // namespace

TEST_CASE("world.fb 28 carries item data, containers and leveled list extras", "[pack][world][items]") {
    const TempDir dir;
    make_items(dir);
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "Items.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->unresolved == 0);
    CHECK(stats->items == 15);
    CHECK(stats->containers == 1);
    CHECK(stats->container_items == 2);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());

    const auto item = [&](std::uint32_t id) {
        const auto base = file->base(id);
        REQUIRE(base.has_value());
        REQUIRE(base->item != nullptr);
        return *base->item;
    };

    // A model-less item is a base all the same, with its name.
    const auto gold = file->base(0x801);
    REQUIRE(gold.has_value());
    CHECK(gold->name == "Gold");
    CHECK(gold->editor_id == "TestGold");
    CHECK(item(0x801).category == ItemCategory::misc);
    CHECK(item(0x801).value == 1);

    CHECK(item(0x802).category == ItemCategory::weapon);
    CHECK(item(0x802).weapon_type == 1);
    CHECK(item(0x802).damage == 7.0F);
    CHECK(item(0x802).weight == 9.0F);
    CHECK(item(0x802).equip_slot == 0x13F44);
    CHECK(item(0x802).playable);
    CHECK(item(0x803).weapon_type == 7);
    CHECK_FALSE(item(0x803).playable);

    CHECK(item(0x804).category == ItemCategory::armor);
    CHECK(item(0x804).armor_type == 1);
    CHECK(item(0x804).biped_slots == 0x4);
    CHECK(item(0x804).armor_rating == 36.0F);
    CHECK(item(0x805).category == ItemCategory::jewelry);
    CHECK(item(0x806).category == ItemCategory::clothing);
    CHECK(item(0x807).category == ItemCategory::armor);
    CHECK(item(0x807).armor_type == 0);

    CHECK(item(0x808).category == ItemCategory::potion);
    CHECK(item(0x808).value == 40);
    CHECK_FALSE(item(0x808).food);
    CHECK(item(0x809).category == ItemCategory::food);
    CHECK(item(0x809).food);
    CHECK(item(0x80A).category == ItemCategory::book);
    CHECK(item(0x80A).value == 15);
    CHECK(item(0x80B).category == ItemCategory::ingredient);
    CHECK(item(0x80B).weight == 0.25F);
    CHECK(item(0x80C).category == ItemCategory::soul_gem);
    CHECK(item(0x80C).soul == 3);
    CHECK(item(0x80C).soul_capacity == 5);
    CHECK(item(0x80C).soul_gem_linked == 0x80D);
    CHECK(item(0x80C).holds_npc_souls);
    CHECK_FALSE(item(0x80D).holds_npc_souls);
    CHECK(item(0x80D).category == ItemCategory::scroll);
    CHECK(item(0x80E).category == ItemCategory::key);
    CHECK(item(0x80F).category == ItemCategory::ammo);
    CHECK(item(0x80F).damage == 8.0F);
    CHECK_FALSE(item(0x80F).playable);

    const auto torch = file->base(0x810);
    REQUIRE(torch.has_value());
    CHECK(torch->has_light);
    CHECK(torch->name == "Torch");
    CHECK(item(0x810).category == ItemCategory::light);
    CHECK(item(0x810).value == 4);

    const auto chest = file->base(0x814);
    REQUIRE(chest.has_value());
    CHECK(chest->item == nullptr);
    CHECK((chest->flags & 2U) != 0);
    REQUIRE(chest->items.size() == 2);
    CHECK(chest->items[0].form() == 0x801);
    CHECK(chest->items[0].count() == 12);
    CHECK(chest->items[1].form() == 0x811);
    CHECK(chest->items[1].count() == 3);

    const auto loot = file->leveled_list(0x811);
    REQUIRE(loot.has_value());
    CHECK(loot->type == tag_label("LVLI"));
    CHECK(loot->chance_none == 25);
    CHECK(loot->chance_global == 0x813);
    REQUIRE(loot->entries.size() == 2);
    CHECK(loot->entries[1].level() == 5);
    CHECK(loot->entries[1].count() == 2);
    const auto spells = file->leveled_list(0x812);
    REQUIRE(spells.has_value());
    CHECK(spells->type == tag_label("LVSP"));
    CHECK(spells->chance_global == 0);
}
