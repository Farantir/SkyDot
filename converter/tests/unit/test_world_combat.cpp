// SPDX-License-Identifier: GPL-3.0-or-later
//
// world.fb 30: game settings (GMST), weapon speed, reach, skill and critical
// data, first-person models, race attacks and unarmed data, impact data sets
// (IPDS) and impacts (IPCT).
// world.fb 31: combat styles (CSTY), NPC combat style and crime faction,
// faction relations (XNAM).
#include "bethconv/pack/world.hpp"

#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"

#include "../support/esm_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <tuple>
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

void zeros(ByteWriter& w, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        w.u8(0);
    }
}

void attack(ByteWriter& race, std::string_view event, float mult, std::uint32_t flags,
            float strike) {
    ByteWriter atkd;
    atkd.f32(mult);
    atkd.f32(0.5F);       // chance
    atkd.u32(0);          // spell
    atkd.u32(flags);
    atkd.f32(-90.0F);     // attack angle
    atkd.f32(strike);
    atkd.f32(1.0F);       // stagger
    atkd.u32(0x80A);      // attack type keyword
    atkd.f32(0.25F);      // knockdown
    atkd.f32(1.5F);       // recovery
    atkd.f32(2.0F);       // stamina
    bethconv::test::write_field(race, "ATKD", atkd);
    text(race, "ATKE", event);
}

void make_combat(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0, {});

    // GMSTs: the editor id's first letter picks the type.
    for (const auto& [name, kind] : {std::pair<std::string_view, int>{"fCombatDistance", 0},
                                     {"iDifficultyLevel", 1},
                                     {"bAllowFoo", 2},
                                     {"sGreeting", 3},
                                     {"fArmorScalingFactor", 4}}) {
        ByteWriter gmst;
        text(gmst, "EDID", name);
        ByteWriter data;
        switch (kind) {
        case 0: data.f32(141.0F); break;
        case 1: data.u32(0xFFFF'FFFFU); break; // -1
        case 2: data.u32(1); break;
        case 3: data.zstring("Hello"); break;
        default: data.f32(0.12F); break;
        }
        bethconv::test::write_field(gmst, "DATA", data);
        record(file, "GMST", 0x900 + static_cast<std::uint32_t>(kind), gmst);
    }

    ByteWriter stat;
    text(stat, "EDID", "IronSword1stP");
    text(stat, "MODL", "Weapons\\Iron\\1stPersonLongsword.nif");
    record(file, "STAT", 0x810, stat);

    ByteWriter weap;
    text(weap, "EDID", "IronSword");
    {
        ByteWriter data;
        data.u32(25);
        data.f32(9.0F);
        data.u16(7);
        bethconv::test::write_field(weap, "DATA", data);
        ByteWriter dnam;
        dnam.u8(1); // one-handed sword
        zeros(dnam, 3);
        dnam.f32(1.25F); // speed
        dnam.f32(0.9F);  // reach
        zeros(dnam, 64);
        dnam.u32(6);     // skill at 76
        zeros(dnam, 8);
        dnam.u32(0xFFFF'FFFFU);
        dnam.u32(0);
        dnam.f32(0.75F); // stagger at 96
        bethconv::test::write_field(weap, "DNAM", dnam);
        ByteWriter crdt;
        crdt.u16(3);
        crdt.u16(0);
        crdt.f32(1.5F);
        zeros(crdt, 16);
        bethconv::test::write_field(weap, "CRDT", crdt);
    }
    u32(weap, "INAM", 0x820);
    u32(weap, "WNAM", 0x810);
    u32(weap, "NAM9", 0x830);
    u32(weap, "NAM8", 0x831);
    record(file, "WEAP", 0x811, weap);

    ByteWriter arma;
    text(arma, "EDID", "ArmorAddon");
    text(arma, "MOD2", "Armor\\A\\m\\cuirass_1.nif");
    text(arma, "MOD4", "Armor\\A\\m\\1stPersonCuirass_1.nif");
    text(arma, "MOD5", "Armor\\A\\f\\1stPersonCuirass_1.nif");
    record(file, "ARMA", 0x812, arma);

    ByteWriter race;
    text(race, "EDID", "TestRace");
    {
        ByteWriter data;
        zeros(data, 96);
        data.f32(4.0F);  // unarmed damage
        data.f32(96.0F); // unarmed reach
        zeros(data, 164 - 104);
        bethconv::test::write_field(race, "DATA", data);
    }
    u32(race, "NAM4", 0x840);
    u32(race, "NAM5", 0x821);
    attack(race, "attackStart", 1.0F, 0, 50.0F);
    attack(race, "attackPowerStartInPlace", 2.0F, 0x4, 35.0F);
    record(file, "RACE", 0x813, race);

    ByteWriter ipds;
    text(ipds, "EDID", "WPNImpactSet");
    for (const auto& [material, impact] : {std::pair<std::uint32_t, std::uint32_t>{0x840, 0x850},
                                           std::pair<std::uint32_t, std::uint32_t>{0x841, 0x851}}) {
        ByteWriter pnam;
        pnam.u32(material);
        pnam.u32(impact);
        bethconv::test::write_field(ipds, "PNAM", pnam);
    }
    record(file, "IPDS", 0x821, ipds);

    ByteWriter ipct;
    text(ipct, "EDID", "WPNImpact");
    text(ipct, "MODL", "Effects\\ImpactEffects\\Dust.nif");
    {
        ByteWriter data;
        data.f32(0.25F);  // duration
        data.u32(2);      // orientation
        data.f32(80.0F);  // angle threshold
        data.f32(16.0F);  // placement radius
        data.u32(1);      // sound level
        data.u8(1);       // flags
        data.u8(0);
        data.u16(0);
        bethconv::test::write_field(ipct, "DATA", data);
    }
    u32(ipct, "SNAM", 0x832);
    u32(ipct, "NAM1", 0x833);
    record(file, "IPCT", 0x850, ipct);

    std::ofstream out(dir / "Combat.esm", std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

} // namespace

TEST_CASE("world.fb 30 carries game settings, weapon combat data, race attacks and impacts",
          "[pack][world][combat]") {
    const TempDir dir;
    make_combat(dir);
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "Combat.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->gmsts == 5);
    CHECK(stats->impact_data_sets == 1);
    CHECK(stats->impacts == 1);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());

    // GMSTs: sorted by lower-case id, found in any case.
    const auto all = file->game_settings();
    REQUIRE(all.size() == 5);
    CHECK(all[0].editor_id == "bAllowFoo");
    CHECK(all[4].editor_id == "sGreeting");
    const auto distance = file->game_setting("FCOMBATDISTANCE");
    REQUIRE(distance.has_value());
    CHECK(distance->kind == pack::wfb::GameSettingKind::floating);
    CHECK(distance->float_value == 141.0F);
    CHECK(file->game_setting("fArmorScalingFactor")->float_value == 0.12F);
    CHECK(file->game_setting("iDifficultyLevel")->kind == pack::wfb::GameSettingKind::integer);
    CHECK(file->game_setting("iDifficultyLevel")->int_value == -1);
    CHECK(file->game_setting("bAllowFoo")->kind == pack::wfb::GameSettingKind::boolean);
    CHECK(file->game_setting("bAllowFoo")->int_value == 1);
    CHECK(file->game_setting("sGreeting")->text_value == "Hello");
    CHECK_FALSE(file->game_setting("fNoSuchSetting").has_value());

    const auto sword = file->base(0x811);
    REQUIRE(sword.has_value());
    REQUIRE(sword->item != nullptr);
    const auto& item = *sword->item;
    CHECK(item.weapon_type == 1);
    CHECK(item.damage == 7.0F);
    CHECK(item.speed == 1.25F);
    CHECK(item.reach == 0.9F);
    CHECK(item.weapon_skill == 6);
    CHECK(item.stagger == 0.75F);
    CHECK(item.crit_damage == 3);
    CHECK(item.crit_mult == 1.5F);
    CHECK(item.impact_data_set == 0x820);
    CHECK(item.first_person_model == "meshes/weapons/iron/1stpersonlongsword.nif");
    CHECK(item.equip_sound == 0x830);
    CHECK(item.unequip_sound == 0x831);

    const auto addon = file->armor_addon(0x812);
    REQUIRE(addon.has_value());
    CHECK(addon->male_model == "meshes/armor/a/m/cuirass_1.nif");
    CHECK(addon->male_model_1st == "meshes/armor/a/m/1stpersoncuirass_1.nif");
    CHECK(addon->female_model_1st == "meshes/armor/a/f/1stpersoncuirass_1.nif");

    const auto race = file->race(0x813);
    REQUIRE(race.has_value());
    CHECK(race->unarmed_damage == 4.0F);
    CHECK(race->unarmed_reach == 96.0F);
    CHECK(race->impact_material == 0x840);
    CHECK(race->impact_data_set == 0x821);
    REQUIRE(race->attacks.size() == 2);
    CHECK(race->attacks[0]->event == "attackStart");
    CHECK(race->attacks[0]->damage_mult == 1.0F);
    CHECK(race->attacks[0]->strike_angle == 50.0F);
    const auto& power = *race->attacks[1];
    CHECK(power.event == "attackPowerStartInPlace");
    CHECK(power.damage_mult == 2.0F);
    CHECK(power.chance == 0.5F);
    CHECK(power.flags == 0x4);
    CHECK(power.attack_angle == -90.0F);
    CHECK(power.strike_angle == 35.0F);
    CHECK(power.stagger == 1.0F);
    CHECK(power.attack_type == 0x80A);
    CHECK(power.knockdown == 0.25F);
    CHECK(power.recovery_time == 1.5F);
    CHECK(power.stamina_mult == 2.0F);

    const auto set = file->impact_data_set(0x821);
    REQUIRE(set.has_value());
    CHECK(set->editor_id == "WPNImpactSet");
    REQUIRE(set->entries.size() == 2);
    CHECK(set->entries[1].material() == 0x841);
    CHECK(set->entries[1].impact() == 0x851);

    const auto impact = file->impact(0x850);
    REQUIRE(impact.has_value());
    CHECK(impact->model == "meshes/effects/impacteffects/dust.nif");
    CHECK(impact->duration == 0.25F);
    CHECK(impact->orientation == 2);
    CHECK(impact->angle_threshold == 80.0F);
    CHECK(impact->placement_radius == 16.0F);
    CHECK(impact->flags == 1);
    CHECK(impact->sound1 == 0x832);
    CHECK(impact->sound2 == 0x833);
}

namespace {

void floats(ByteWriter& w, float first, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        w.f32(first + static_cast<float>(i));
    }
}

void make_styles(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0, {});

    ByteWriter full;
    text(full, "EDID", "csFull");
    {
        ByteWriter w;
        floats(w, 1.0F, 10); // CSGD: 1..10
        bethconv::test::write_field(full, "CSGD", w);
        ByteWriter md;
        zeros(md, 8);
        bethconv::test::write_field(full, "CSMD", md);
        ByteWriter me;
        floats(me, 20.0F, 8);
        bethconv::test::write_field(full, "CSME", me);
        ByteWriter cr;
        floats(cr, 30.0F, 4);
        bethconv::test::write_field(full, "CSCR", cr);
        ByteWriter lr;
        lr.f32(40.0F);
        bethconv::test::write_field(full, "CSLR", lr);
        ByteWriter fl;
        floats(fl, 50.0F, 8);
        bethconv::test::write_field(full, "CSFL", fl);
    }
    u32(full, "DATA", 5);
    record(file, "CSTY", 0x900, full);

    // An older record: CSGD of 3 floats only.
    ByteWriter shorter;
    text(shorter, "EDID", "csShort");
    {
        ByteWriter w;
        floats(w, 7.0F, 3);
        bethconv::test::write_field(shorter, "CSGD", w);
    }
    record(file, "CSTY", 0x901, shorter);

    ByteWriter npc;
    text(npc, "EDID", "StyledNpc");
    u32(npc, "ZNAM", 0x900);
    u32(npc, "CRIF", 0x911);
    u32(npc, "INAM", 0x911); // world.fb 37: the death item (any form here)
    {
        // AIDT: seven behaviour bytes, a pad, then warn, warn or attack and attack distances.
        ByteWriter aidt;
        for (const int b : {1, 4, 50, 0, 0, 1, 1, 0}) {
            aidt.u8(static_cast<std::uint8_t>(b));
        }
        aidt.u32(300);
        aidt.u32(400);
        aidt.u32(500);
        bethconv::test::write_field(npc, "AIDT", aidt);
    }
    record(file, "NPC_", 0x910, npc);

    ByteWriter fact;
    text(fact, "EDID", "BanditLike");
    for (const auto& [other, modifier, reaction] :
         {std::tuple<std::uint32_t, std::int32_t, std::uint32_t>{0x912, 0, 1}, {0x913, -5, 2},
          {0x914, 3, 3}}) {
        ByteWriter xnam;
        xnam.u32(other);
        xnam.u32(static_cast<std::uint32_t>(modifier));
        xnam.u32(reaction);
        bethconv::test::write_field(fact, "XNAM", xnam);
    }
    u32(fact, "DATA", 0x8001'4005U);
    record(file, "FACT", 0x911, fact);

    std::ofstream out(dir / "Styles.esm", std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

} // namespace

TEST_CASE("world.fb 31 carries combat styles, the NPC's style and crime faction, faction relations",
          "[pack][world][combat]") {
    const TempDir dir;
    make_styles(dir);
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "Styles.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->combat_styles == 2);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());
    const auto full = file->combat_style(0x900);
    REQUIRE(full.has_value());
    CHECK(full->editor_id == "csFull");
    CHECK(full->offensive_mult == 1.0F);
    CHECK(full->group_offensive_mult == 3.0F);
    CHECK(full->melee_score_mult == 4.0F);
    CHECK(full->staff_score_mult == 9.0F);
    CHECK(full->avoid_threat_chance == 10.0F);
    CHECK(full->attack_staggered_mult == 20.0F);
    CHECK(full->bash_mult == 23.0F);
    CHECK(full->special_attack_mult == 27.0F);
    CHECK(full->circle_mult == 30.0F);
    CHECK(full->stalk_time == 33.0F);
    CHECK(full->strafe_mult == 40.0F);
    CHECK(full->hover_chance == 50.0F);
    CHECK(full->flying_attack_chance == 57.0F);
    CHECK(full->flags == 5);

    const auto shorter = file->combat_style(0x901);
    REQUIRE(shorter.has_value());
    CHECK(shorter->defensive_mult == 8.0F);
    CHECK(shorter->group_offensive_mult == 9.0F);
    CHECK(shorter->melee_score_mult == 0.0F);
    CHECK(shorter->flags == 0);
    CHECK_FALSE(file->combat_style(0x999).has_value());

    const auto npc = file->npc(0x910);
    REQUIRE(npc.has_value());
    CHECK(npc->combat_style == 0x900);
    CHECK(npc->crime_faction == 0x911);
    CHECK(npc->death_item == 0x911);
    CHECK(npc->aggro_radius_behaviour == 1);
    CHECK(npc->aggro_warn == 300);
    CHECK(npc->aggro_warn_attack == 400);
    CHECK(npc->aggro_attack == 500);

    const auto faction = file->faction(0x911);
    REQUIRE(faction.has_value());
    CHECK(static_cast<std::uint32_t>(faction->flags) == 0x8001'4005U);
    REQUIRE(faction->relations.size() == 3);
    CHECK(faction->relations[0].faction() == 0x912);
    CHECK(faction->relations[0].reaction() == 1);
    CHECK(faction->relations[1].modifier() == -5);
    CHECK(faction->relations[1].reaction() == 2);
    CHECK(faction->relations[2].reaction() == 3);
}
