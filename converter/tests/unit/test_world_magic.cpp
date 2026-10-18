// SPDX-License-Identifier: GPL-3.0-or-later
//
// world.fb 33: spells (SPEL), effect conditions, the whole DATA of magic
// effects with their sounds, counter effects and scripts, art objects (ARTO),
// projectile and enchantment data, weapon charge (EAMT) and the spells of
// NPC_ and RACE.
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

/// A 32-byte CTDA: function 47 (GetItemCount-like), comparison value, one
/// FormID parameter.
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
    c.u32(0); // run on subject
    c.u32(0);
    c.u32(0xFFFF'FFFFU);
    bethconv::test::write_field(payload, "CTDA", c);
}

void effect(ByteWriter& payload, std::uint32_t mgef, float magnitude, std::uint32_t area,
            std::uint32_t duration) {
    u32(payload, "EFID", mgef);
    ByteWriter efit;
    efit.f32(magnitude);
    efit.u32(area);
    efit.u32(duration);
    bethconv::test::write_field(payload, "EFIT", efit);
}

/// VMAD with one script without properties.
ByteWriter vmad(std::string_view script) {
    ByteWriter v;
    v.u16(5);
    v.u16(2);
    v.u16(1);
    v.u16(static_cast<std::uint16_t>(script.size()));
    v.raw(script);
    v.u8(0);
    v.u16(0);
    return v;
}

void make_magic(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0, {});

    ByteWriter arto;
    text(arto, "EDID", "FireCastingArt");
    text(arto, "MODL", "Effects\\Fire\\Cast.nif");
    u32(arto, "DNAM", 1);
    record(file, "ARTO", 0x900, arto);

    ByteWriter proj;
    text(proj, "EDID", "FireProjectile");
    text(proj, "MODL", "Effects\\Fire\\Ball.nif");
    {
        ByteWriter d;
        d.u16(0x28);   // flags: explosion and muzzle flash
        d.u16(8);      // type: flame
        d.f32(0.0F);   // gravity
        d.f32(1500.0F);
        d.f32(5000.0F);
        d.u32(0x910);  // 16 light
        d.u32(0x911);  // 20 muzzle flash light
        d.f32(0.5F);   // 24 tracer chance
        d.f32(64.0F);  // 28 proximity
        d.f32(3.0F);   // 32 timer
        d.u32(0x912);  // 36 explosion
        d.u32(0x913);  // 40 sound
        d.f32(0.25F);  // 44 muzzle flash duration
        d.f32(1.5F);   // 48 fade duration
        d.f32(40.0F);  // 52 impact force
        d.u32(0x914);  // 56 countdown sound
        d.u32(0x915);  // 60 disable sound
        d.u32(0x916);  // 64 default weapon source
        d.f32(7.0F);   // 68 cone spread
        d.f32(12.0F);  // 72 collision radius
        d.f32(8.0F);   // 76 lifetime
        d.f32(0.2F);   // 80 relaunch interval
        d.u32(0);      // 84 decal data
        d.u32(0);      // 88 collision layer
        bethconv::test::write_field(proj, "DATA", d);
    }
    u32(proj, "VNAM", 2);
    record(file, "PROJ", 0x901, proj);

    ByteWriter mgef;
    text(mgef, "EDID", "FireDamage");
    text(mgef, "FULL", "Fire Damage");
    {
        ByteWriter d;
        d.u32(0x0020'0005); // 0 flags: hostile, detrimental, power affects magnitude
        d.f32(2.0F);        // 4 base cost
        d.u32(0x920);       // 8 associated item
        d.u32(18);          // 12 magic skill
        d.u32(0x40);        // 16 resist value
        d.u32(2);           // 20 counter effect count
        d.u32(0x921);       // 24 casting light
        d.f32(0.3F);        // 28 taper weight
        d.u32(0x922);       // 32 hit shader
        d.u32(0x923);       // 36 enchant shader
        d.u32(25);          // 40 skill level
        d.u32(9);           // 44 area
        d.f32(1.5F);        // 48 casting time
        d.f32(2.0F);        // 52 taper curve
        d.f32(0.1F);        // 56 taper duration
        d.f32(0.6F);        // 60 second AV weight
        d.u32(5);           // 64 archetype
        d.u32(24);          // 68 actor value
        d.u32(0x901);       // 72 projectile
        d.u32(0x924);       // 76 explosion
        d.u32(2);           // 80 casting type
        d.u32(2);           // 84 delivery
        d.u32(26);          // 88 second actor value
        d.u32(0x900);       // 92 casting art
        d.u32(0x925);       // 96 hit effect art
        d.u32(0x926);       // 100 impact data
        d.f32(1.4F);        // 104 skill usage multiplier
        d.u32(0x927);       // 108 dual cast art
        d.f32(1.25F);       // 112 dual cast scale
        d.u32(0x928);       // 116 enchant art
        d.u32(0);           // 120
        d.u32(0);           // 124
        d.u32(0x929);       // 128 equip ability
        d.u32(0x92A);       // 132 image space modifier
        d.u32(0x92B);       // 136 perk to apply
        d.u32(1);           // 140 casting sound level
        d.f32(3.0F);        // 144 AI score
        d.f32(0.5F);        // 148 AI delay
        bethconv::test::write_field(mgef, "DATA", d);
        ByteWriter sndd;
        sndd.u32(3);
        sndd.u32(0x930);
        sndd.u32(4);
        sndd.u32(0x931);
        bethconv::test::write_field(mgef, "SNDD", sndd);
        ByteWriter esce;
        esce.u32(0x940);
        esce.u32(0x941);
        bethconv::test::write_field(mgef, "ESCE", esce);
    }
    ctda(mgef, 47, 1.0F, 0x920);
    bethconv::test::write_field(mgef, "VMAD", vmad("FireScript"));
    record(file, "MGEF", 0x902, mgef);

    ByteWriter ench;
    text(ench, "EDID", "EnchantStaffFire");
    {
        ByteWriter enit;
        enit.u32(120);
        enit.u32(0x5);   // flags
        enit.u32(2);     // casting type
        enit.u32(600);   // amount
        enit.u32(3);     // delivery
        enit.u32(12);    // staff enchantment
        enit.f32(0.75F); // charge time
        enit.u32(0x950); // base enchantment
        enit.u32(0x951); // worn restrictions
        bethconv::test::write_field(ench, "ENIT", enit);
    }
    effect(ench, 0x902, 8.0F, 3, 5);
    ctda(ench, 72, 1.0F, 0x920);
    ctda(ench, 73, 0.0F, 0x921);
    effect(ench, 0x902, 2.0F, 0, 0);
    record(file, "ENCH", 0x903, ench);

    ByteWriter spel;
    text(spel, "EDID", "Firebolt");
    text(spel, "FULL", "Firebolt");
    u32(spel, "ETYP", 0x13F44);
    {
        ByteWriter spit;
        spit.u32(14);     // cost
        spit.u32(0x1);    // flags: manual cost
        spit.u32(0);      // type: spell
        spit.f32(0.5F);   // charge time
        spit.u32(1);      // casting type
        spit.u32(2);      // delivery
        spit.f32(1.25F);  // cast duration
        spit.f32(4000.0F);
        spit.u32(0x960);  // half-cost perk
        bethconv::test::write_field(spel, "SPIT", spit);
    }
    effect(spel, 0x902, 25.0F, 0, 0);
    ctda(spel, 47, 1.0F, 0x920);
    record(file, "SPEL", 0x904, spel);

    ByteWriter weap;
    text(weap, "EDID", "FireStaff");
    u32(weap, "EITM", 0x903);
    {
        ByteWriter d;
        d.u32(25);
        d.f32(9.0F);
        d.u16(7);
        bethconv::test::write_field(weap, "DATA", d);
        ByteWriter eamt;
        eamt.u16(2500);
        bethconv::test::write_field(weap, "EAMT", eamt);
    }
    record(file, "WEAP", 0x905, weap);

    ByteWriter npc;
    text(npc, "EDID", "Mage");
    u32(npc, "SPCT", 3);
    u32(npc, "SPLO", 0x904);
    u32(npc, "SPLO", 0x970);
    u32(npc, "SPLO", 0x971);
    record(file, "NPC_", 0x906, npc);

    ByteWriter race;
    text(race, "EDID", "MageRace");
    u32(race, "SPCT", 2);
    u32(race, "SPLO", 0x904);
    u32(race, "SPLO", 0x972);
    record(file, "RACE", 0x907, race);

    std::ofstream out(dir / "Magic.esm", std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

} // namespace

TEST_CASE("world.fb 33 carries spells, magic effect data, art objects and projectile data",
          "[pack][world][magic]") {
    const TempDir dir;
    make_magic(dir);
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "Magic.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->spells == 1);
    CHECK(stats->art_objects == 1);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());

    const auto art = file->art_object(0x900);
    REQUIRE(art.has_value());
    CHECK(art->editor_id == "FireCastingArt");
    CHECK(art->model == "meshes/effects/fire/cast.nif");
    CHECK(art->art_type == 1);

    const auto proj = file->projectile(0x901);
    REQUIRE(proj.has_value());
    CHECK(proj->type == 8);
    CHECK(proj->flags == 0x28);
    CHECK(proj->speed == 1500.0F);
    CHECK(proj->model == "meshes/effects/fire/ball.nif");
    CHECK(proj->light == 0x910);
    CHECK(proj->muzzle_flash_light == 0x911);
    CHECK(proj->tracer_chance == 0.5F);
    CHECK(proj->explosion_proximity == 64.0F);
    CHECK(proj->explosion_timer == 3.0F);
    CHECK(proj->explosion == 0x912);
    CHECK(proj->sound == 0x913);
    CHECK(proj->muzzle_flash_duration == 0.25F);
    CHECK(proj->fade_duration == 1.5F);
    CHECK(proj->impact_force == 40.0F);
    CHECK(proj->sound_countdown == 0x914);
    CHECK(proj->sound_disable == 0x915);
    CHECK(proj->default_weapon_source == 0x916);
    CHECK(proj->cone_spread == 7.0F);
    CHECK(proj->collision_radius == 12.0F);
    CHECK(proj->lifetime == 8.0F);
    CHECK(proj->relaunch_interval == 0.2F);
    CHECK(proj->sound_level == 2);

    const auto m = file->magic_effect(0x902);
    REQUIRE(m.has_value());
    CHECK(m->flags == 0x0020'0005);
    CHECK(m->base_cost == 2.0F);
    CHECK(m->associated_item == 0x920);
    CHECK(m->magic_skill == 18);
    CHECK(m->resist_value == 0x40);
    CHECK(m->casting_light == 0x921);
    CHECK(m->taper_weight == 0.3F);
    CHECK(m->hit_shader == 0x922);
    CHECK(m->enchant_shader == 0x923);
    CHECK(m->skill_level == 25);
    CHECK(m->area == 9);
    CHECK(m->casting_time == 1.5F);
    CHECK(m->taper_curve == 2.0F);
    CHECK(m->taper_duration == 0.1F);
    CHECK(m->second_av_weight == 0.6F);
    CHECK(m->archetype == 5);
    CHECK(m->actor_value == 24);
    CHECK(m->projectile == 0x901);
    CHECK(m->explosion == 0x924);
    CHECK(m->casting_type == 2);
    CHECK(m->delivery == 2);
    CHECK(m->second_actor_value == 26);
    CHECK(m->casting_art == 0x900);
    CHECK(m->hit_effect_art == 0x925);
    CHECK(m->impact_data == 0x926);
    CHECK(m->skill_usage_multiplier == 1.4F);
    CHECK(m->dual_cast_art == 0x927);
    CHECK(m->dual_cast_scale == 1.25F);
    CHECK(m->enchant_art == 0x928);
    CHECK(m->equip_ability == 0x929);
    CHECK(m->image_space_mod == 0x92A);
    CHECK(m->perk_to_apply == 0x92B);
    CHECK(m->casting_sound_level == 1);
    CHECK(m->ai_score == 3.0F);
    CHECK(m->ai_delay_time == 0.5F);
    REQUIRE(m->counter_effects.size() == 2);
    CHECK(m->counter_effects[1] == 0x941);
    REQUIRE(m->sounds.size() == 2);
    CHECK(m->sounds[0].kind() == 3);
    CHECK(m->sounds[0].sound() == 0x930);
    CHECK(m->sounds[1].kind() == 4);
    CHECK(m->sounds[1].sound() == 0x931);
    REQUIRE(m->conditions.size() == 1);
    CHECK(m->conditions[0]->function == 47);
    REQUIRE(m->scripts.size() == 1);
    CHECK(m->scripts[0]->name == "FireScript");

    const auto ench = file->enchantment(0x903);
    REQUIRE(ench.has_value());
    CHECK(ench->charge == 600);
    CHECK(ench->flags == 0x5);
    CHECK(ench->casting_type == 2);
    CHECK(ench->delivery == 3);
    CHECK(ench->enchantment_type == 12);
    CHECK(ench->charge_time == 0.75F);
    CHECK(ench->base_enchantment == 0x950);
    CHECK(ench->worn_restrictions == 0x951);
    REQUIRE(ench->effects.size() == 2);
    CHECK(ench->effects[0]->magnitude == 8.0F);
    CHECK(ench->effects[0]->area == 3);
    CHECK(ench->effects[0]->duration == 5);
    REQUIRE(ench->effects[0]->conditions.size() == 2);
    CHECK(ench->effects[0]->conditions[1]->function == 73);
    CHECK(ench->effects[1]->conditions.empty());

    const auto spell = file->spell(0x904);
    REQUIRE(spell.has_value());
    CHECK(spell->name == "Firebolt");
    CHECK(spell->cost == 14);
    CHECK(spell->flags == 0x1);
    CHECK(spell->type == 0);
    CHECK(spell->charge_time == 0.5F);
    CHECK(spell->casting_type == 1);
    CHECK(spell->delivery == 2);
    CHECK(spell->cast_duration == 1.25F);
    CHECK(spell->range == 4000.0F);
    CHECK(spell->half_cost_perk == 0x960);
    CHECK(spell->equip_slot == 0x13F44);
    REQUIRE(spell->effects.size() == 1);
    CHECK(spell->effects[0]->effect == 0x902);
    CHECK(spell->effects[0]->magnitude == 25.0F);
    REQUIRE(spell->effects[0]->conditions.size() == 1);
    CHECK(spell->effects[0]->conditions[0]->param1 == 0x920);

    const auto staff = file->base(0x905);
    REQUIRE(staff.has_value());
    REQUIRE(staff->item != nullptr);
    CHECK(staff->item->enchantment == 0x903);
    CHECK(staff->item->enchantment_amount == 2500);

    const auto npc = file->npc(0x906);
    REQUIRE(npc.has_value());
    REQUIRE(npc->spells.size() == 3);
    CHECK(npc->spells[0] == 0x904);
    CHECK(npc->spells[2] == 0x971);
    const auto race = file->race(0x907);
    REQUIRE(race.has_value());
    REQUIRE(race->spells.size() == 2);
    CHECK(race->spells[1] == 0x972);
}
