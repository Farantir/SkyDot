// SPDX-License-Identifier: GPL-3.0-or-later
//
// world.fb 29: recipes (COBJ), magic effects (MGEF), enchantments (ENCH),
// item effects and enchantments, harvest data (FLOR, TREE) and bench types
// (FURN).
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

void record(ByteWriter& file, std::string_view type, std::uint32_t id, const ByteWriter& payload) {
    ByteWriter rec;
    bethconv::test::write_record(rec, type, id, payload.span());
    bethconv::test::write_group(file, tag_label(type), 0, rec.span());
}

void effect(ByteWriter& payload, std::uint32_t mgef, float magnitude, std::uint32_t duration) {
    u32(payload, "EFID", mgef);
    ByteWriter efit;
    efit.f32(magnitude);
    efit.u32(0);
    efit.u32(duration);
    bethconv::test::write_field(payload, "EFIT", efit);
}

void make_crafting(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0, {});

    ByteWriter keyword;
    text(keyword, "EDID", "CraftingSmithingForge");
    record(file, "KYWD", 0x801, keyword);

    ByteWriter mgef;
    text(mgef, "EDID", "RestoreHealth");
    text(mgef, "FULL", "Restore Health");
    text(mgef, "DNAM", "Restore <mag> points of Health.");
    {
        ByteWriter data;
        data.u32(0x0020'0001); // hostile, power affects magnitude
        data.f32(2.5F);
        data.u32(0);
        data.u32(0xFFFF'FFFFU);
        data.u32(0xFFFF'FFFFU);
        for (int i = 0; i < 11; ++i) {
            data.u32(0);
        }
        data.u32(2);  // archetype at offset 64
        data.u32(24); // actor value
        data.u32(0);
        data.u32(0);
        data.u32(1);  // casting type
        data.u32(3);  // delivery
        bethconv::test::write_field(mgef, "DATA", data);
    }
    record(file, "MGEF", 0x802, mgef);

    ByteWriter ench;
    text(ench, "EDID", "EnchantFire");
    text(ench, "FULL", "Fiery");
    {
        ByteWriter enit;
        enit.u32(120);
        enit.u32(0);
        enit.u32(1);
        enit.u32(600); // amount
        enit.u32(0);
        enit.u32(0x06);
        enit.f32(0.0F);
        enit.u32(0);
        enit.u32(0);
        bethconv::test::write_field(ench, "ENIT", enit);
    }
    effect(ench, 0x802, 8.0F, 0);
    record(file, "ENCH", 0x803, ench);

    ByteWriter ingr;
    text(ingr, "EDID", "Herb");
    text(ingr, "FULL", "Herb");
    {
        ByteWriter data;
        data.u32(3);
        data.f32(0.25F);
        bethconv::test::write_field(ingr, "DATA", data);
        ByteWriter enit;
        enit.u32(3);
        enit.u32(0);
        bethconv::test::write_field(ingr, "ENIT", enit);
    }
    effect(ingr, 0x802, 4.0F, 30);
    record(file, "INGR", 0x804, ingr);

    ByteWriter weap;
    text(weap, "EDID", "FireSword");
    u32(weap, "EITM", 0x803);
    {
        ByteWriter data;
        data.u32(25);
        data.f32(9.0F);
        data.u16(7);
        bethconv::test::write_field(weap, "DATA", data);
    }
    record(file, "WEAP", 0x805, weap);

    ByteWriter furn;
    text(furn, "EDID", "Forge");
    text(furn, "MODL", "Furniture\\Forge.nif");
    {
        ByteWriter ksiz;
        ksiz.u32(1);
        bethconv::test::write_field(furn, "KSIZ", ksiz);
        u32(furn, "KWDA", 0x801);
        ByteWriter wbdt;
        wbdt.u8(1);
        wbdt.u8(12);
        bethconv::test::write_field(furn, "WBDT", wbdt);
    }
    record(file, "FURN", 0x806, furn);

    ByteWriter flor;
    text(flor, "EDID", "Plant");
    text(flor, "MODL", "Plants\\Plant.nif");
    u32(flor, "PFIG", 0x804);
    u32(flor, "SNAM", 0);
    {
        ByteWriter pfpc;
        pfpc.u8(10);
        pfpc.u8(20);
        pfpc.u8(30);
        pfpc.u8(40);
        bethconv::test::write_field(flor, "PFPC", pfpc);
    }
    record(file, "FLOR", 0x807, flor);

    ByteWriter cobj;
    text(cobj, "EDID", "RecipeSword");
    u32(cobj, "COCT", 2);
    for (const auto& [form, count] : {std::pair<std::uint32_t, std::uint32_t>{0x804, 2},
                                      std::pair<std::uint32_t, std::uint32_t>{0x805, 1}}) {
        ByteWriter cnto;
        cnto.u32(form);
        cnto.u32(count);
        bethconv::test::write_field(cobj, "CNTO", cnto);
    }
    {
        ByteWriter ctda;
        ctda.u8(0x60);
        ctda.u8(0);
        ctda.u16(0);
        ctda.f32(1.0F);
        ctda.u16(47);
        ctda.u16(0);
        ctda.u32(0x804);
        ctda.u32(0);
        ctda.u32(0);
        ctda.u32(0);
        ctda.u32(0xFFFF'FFFFU);
        bethconv::test::write_field(cobj, "CTDA", ctda);
    }
    u32(cobj, "CNAM", 0x805);
    u32(cobj, "BNAM", 0x801);
    {
        ByteWriter nam1;
        nam1.u16(3);
        bethconv::test::write_field(cobj, "NAM1", nam1);
    }
    record(file, "COBJ", 0x808, cobj);

    std::ofstream out(dir / "Craft.esm", std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

} // namespace

TEST_CASE("world.fb 29 carries recipes, magic effects, harvest data and bench types",
          "[pack][world][crafting]") {
    const TempDir dir;
    make_crafting(dir);
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "Craft.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->unresolved == 0);
    CHECK(stats->recipes == 1);
    CHECK(stats->magic_effects == 1);
    CHECK(stats->enchantments == 1);
    CHECK(stats->harvestables == 1);
    CHECK(stats->benches == 1);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());

    const auto recipe = file->recipe(0x808);
    REQUIRE(recipe.has_value());
    CHECK(recipe->editor_id == "RecipeSword");
    CHECK(recipe->product == 0x805);
    CHECK(recipe->product_count == 3);
    CHECK(recipe->bench_keyword == 0x801);
    REQUIRE(recipe->components.size() == 2);
    CHECK(recipe->components[0].form() == 0x804);
    CHECK(recipe->components[0].count() == 2);
    REQUIRE(recipe->conditions.size() == 1);
    CHECK(recipe->conditions[0]->function == 47);
    CHECK(recipe->conditions[0]->type == 0x60);
    CHECK(recipe->conditions[0]->value == 1.0F);
    CHECK(recipe->conditions[0]->param1 == 0x804);

    const auto effect = file->magic_effect(0x802);
    REQUIRE(effect.has_value());
    CHECK(effect->editor_id == "RestoreHealth");
    CHECK(effect->name == "Restore Health");
    CHECK(effect->description == "Restore <mag> points of Health.");
    CHECK(effect->flags == 0x0020'0001);
    CHECK(effect->base_cost == 2.5F);
    CHECK(effect->magic_skill == -1);
    CHECK(effect->archetype == 2);
    CHECK(effect->actor_value == 24);
    CHECK(effect->casting_type == 1);
    CHECK(effect->delivery == 3);

    const auto enchantment = file->enchantment(0x803);
    REQUIRE(enchantment.has_value());
    CHECK(enchantment->name == "Fiery");
    CHECK(enchantment->cost == 120);
    CHECK(enchantment->charge == 600);
    REQUIRE(enchantment->effects.size() == 1);
    CHECK(enchantment->effects[0].effect() == 0x802);
    CHECK(enchantment->effects[0].magnitude() == 8.0F);

    const auto herb = file->base(0x804);
    REQUIRE(herb.has_value());
    REQUIRE(herb->item != nullptr);
    REQUIRE(herb->item->effects.size() == 1);
    CHECK(herb->item->effects[0].effect() == 0x802);
    CHECK(herb->item->effects[0].magnitude() == 4.0F);
    CHECK(herb->item->effects[0].duration() == 30);
    const auto sword = file->base(0x805);
    REQUIRE(sword.has_value());
    REQUIRE(sword->item != nullptr);
    CHECK(sword->item->enchantment == 0x803);

    const auto forge = file->base(0x806);
    REQUIRE(forge.has_value());
    CHECK(forge->bench_type == 1);
    CHECK(forge->bench_skill == 12);
    REQUIRE(forge->keywords.size() == 1);
    CHECK(forge->keywords[0] == 0x801);
    CHECK(forge->harvest == nullptr);

    const auto plant = file->base(0x807);
    REQUIRE(plant.has_value());
    REQUIRE(plant->harvest != nullptr);
    CHECK(plant->harvest->ingredient == 0x804);
    CHECK(plant->harvest->sound == 0);
    CHECK(plant->harvest->seasonal_yield == 0x281E140AU);
    CHECK(plant->bench_type == 0);
}
