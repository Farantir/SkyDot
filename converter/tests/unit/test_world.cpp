// SPDX-License-Identifier: GPL-3.0-or-later
//
// world.fb: cells, references and bases decoded for the engine. The main case
// is a plugin whose master list differs from the load order, where FormIDs in
// payloads are only correct if resolved through the winning plugin.
#include "bethconv/pack/world.hpp"

#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"

#include "../support/esm_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>
#include <vector>

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

void save(const TempDir& dir, std::string_view name, const ByteWriter& file) {
    std::ofstream out(dir / name, std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

void top_group(ByteWriter& file, std::string_view type, const ByteWriter& children) {
    bethconv::test::write_group(file, tag_label(type), 0, children.span());
}

ByteWriter static_payload(std::string_view editor_id, std::string_view model) {
    ByteWriter payload;
    ByteWriter edid;
    edid.zstring(editor_id);
    bethconv::test::write_field(payload, "EDID", edid);
    ByteWriter modl;
    modl.zstring(model);
    bethconv::test::write_field(payload, "MODL", modl);
    return payload;
}

ByteWriter light_payload() {
    ByteWriter payload;
    ByteWriter edid;
    edid.zstring("TestLight");
    bethconv::test::write_field(payload, "EDID", edid);
    ByteWriter data;
    data.u32(static_cast<std::uint32_t>(-1)); // time
    data.u32(256);                            // radius
    data.u32(0x0080'40FFu);                   // color
    data.u32(0);                              // flags
    for (int i = 0; i < 6; ++i) {
        data.f32(1.0F);
    }
    data.u32(0);    // value
    data.f32(0.0F); // weight
    bethconv::test::write_field(payload, "DATA", data);
    ByteWriter fnam;
    fnam.f32(1.0F);
    bethconv::test::write_field(payload, "FNAM", fnam);
    return payload;
}

ByteWriter refr_payload(std::uint32_t base, float x, float scale) {
    ByteWriter payload;
    ByteWriter name;
    name.u32(base);
    bethconv::test::write_field(payload, "NAME", name);
    ByteWriter data;
    data.f32(x);
    data.f32(2.0F);
    data.f32(3.0F);
    data.f32(0.0F);
    data.f32(0.0F);
    data.f32(1.5F);
    bethconv::test::write_field(payload, "DATA", data);
    if (scale != 1.0F) {
        ByteWriter xscl;
        xscl.f32(scale);
        bethconv::test::write_field(payload, "XSCL", xscl);
    }
    return payload;
}

/// VMAD with one script whose "Target" property points at `target` (a local
/// FormID) and whose "Count" is 3.
ByteWriter vmad_field(std::string_view script, std::uint32_t target) {
    ByteWriter vmad;
    const auto wstring = [&](std::string_view text) {
        vmad.u16(static_cast<std::uint16_t>(text.size()));
        vmad.raw(text);
    };
    vmad.u16(5);
    vmad.u16(2);
    vmad.u16(1);
    wstring(script);
    vmad.u8(0);
    vmad.u16(2);
    wstring("Target");
    vmad.u8(1);
    vmad.u8(1);
    vmad.u16(0);
    vmad.u16(0xFFFF);
    vmad.u32(target);
    wstring("Count");
    vmad.u8(3);
    vmad.u8(1);
    vmad.u32(3);
    return vmad;
}

/// The chair reference, with scripts, a lock, a linked ref, an activate
/// parent and a primitive, all naming Mod.esp's own records.
ByteWriter scripted_refr_payload() {
    ByteWriter payload = refr_payload(0x0100'0A00, 20.0F, 2.0F);
    bethconv::test::write_field(payload, "VMAD", vmad_field("TestLever", 0x0100'0B01));
    ByteWriter xloc;
    xloc.u8(255);
    xloc.u8(0);
    xloc.u16(0);
    xloc.u32(0x0100'0A00);
    xloc.u8(0);
    xloc.u8(0);
    xloc.u16(0);
    xloc.u32(0);
    xloc.u32(0);
    bethconv::test::write_field(payload, "XLOC", xloc);
    ByteWriter xlkr;
    xlkr.u32(0);
    xlkr.u32(0x0100'0B03);
    bethconv::test::write_field(payload, "XLKR", xlkr);
    ByteWriter xapd;
    xapd.u8(1);
    bethconv::test::write_field(payload, "XAPD", xapd);
    ByteWriter xapr;
    xapr.u32(0x0100'0B01);
    xapr.f32(0.5F);
    bethconv::test::write_field(payload, "XAPR", xapr);
    ByteWriter xprm;
    for (const float v : {64.0F, 32.0F, 16.0F, 0.0F, 1.0F, 0.0F, 0.0F}) {
        xprm.f32(v);
    }
    xprm.u32(1);
    bethconv::test::write_field(payload, "XPRM", xprm);
    return payload;
}

ByteWriter cell_payload(std::string_view editor_id) {
    ByteWriter payload;
    ByteWriter edid;
    edid.zstring(editor_id);
    bethconv::test::write_field(payload, "EDID", edid);
    ByteWriter data;
    data.u16(0x0001); // interior
    bethconv::test::write_field(payload, "DATA", data);
    return payload;
}

/// Base.esm: a STAT (0x800) and a LIGH (0x801), both in its own space.
void make_base(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0x1, {});
    ByteWriter stats;
    bethconv::test::write_record(stats, "STAT", 0x0000'0800,
                                 static_payload("BaseWall", "Architecture\\Wall.nif").span());
    top_group(file, "STAT", stats);
    ByteWriter lights;
    bethconv::test::write_record(lights, "LIGH", 0x0000'0801, light_payload().span());
    top_group(file, "LIGH", lights);

    // ARMO: MODL is an armature FormID; the world model is MOD2.
    ByteWriter armor_payload;
    ByteWriter armature;
    armature.u32(0x0000'0999);
    bethconv::test::write_field(armor_payload, "MODL", armature);
    ByteWriter mod2;
    mod2.zstring("Armor\\Iron\\ShieldGND.nif");
    bethconv::test::write_field(armor_payload, "MOD2", mod2);
    ByteWriter armors;
    bethconv::test::write_record(armors, "ARMO", 0x0000'0802, armor_payload.span());
    top_group(file, "ARMO", armors);
    save(dir, "Base.esm", file);
}

/// Other.esm: an unrelated master that shifts Mod.esp's load-order index.
void make_other(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0x1, {});
    ByteWriter stats;
    bethconv::test::write_record(stats, "STAT", 0x0000'0900,
                                 static_payload("OtherThing", "meshes\\other.nif").span());
    top_group(file, "STAT", stats);
    save(dir, "Other.esm", file);
}

/// Mod.esp, masters [Base.esm] only, loaded third. Its own records are 0x01...
/// on disk and 0x02... globally.
void make_mod(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0, {"Base.esm"});

    ByteWriter stats;
    bethconv::test::write_record(stats, "STAT", 0x0100'0A00,
                                 static_payload("ModChair", "Furniture\\Chair.nif").span());
    top_group(file, "STAT", stats);

    // A scripted door, and an activator with scripts but no model (a trigger).
    ByteWriter door_payload = static_payload("ModDoor", "Doors\\Door.nif");
    ByteWriter fnam;
    fnam.u8(0x02); // automatic
    bethconv::test::write_field(door_payload, "FNAM", fnam);
    bethconv::test::write_field(door_payload, "VMAD", vmad_field("DoorScript", 0x0000'0800));
    ByteWriter doors;
    bethconv::test::write_record(doors, "DOOR", 0x0100'0A01, door_payload.span());
    top_group(file, "DOOR", doors);
    ByteWriter trigger_payload;
    bethconv::test::write_field(trigger_payload, "VMAD", vmad_field("TriggerScript", 0x0100'0A01));
    ByteWriter activators;
    bethconv::test::write_record(activators, "ACTI", 0x0100'0A02, trigger_payload.span());
    top_group(file, "ACTI", activators);

    const std::uint32_t cell = 0x0100'0B00;
    ByteWriter refrs;
    bethconv::test::write_record(refrs, "REFR", 0x0100'0B01,
                                 refr_payload(0x0000'0800, 10.0F, 1.0F).span()); // Base wall
    bethconv::test::write_record(refrs, "REFR", 0x0100'0B02,
                                 scripted_refr_payload().span()); // own chair
    bethconv::test::write_record(refrs, "REFR", 0x0100'0B03,
                                 refr_payload(0x0000'0801, 30.0F, 1.0F).span()); // Base light
    ByteWriter temporary;
    bethconv::test::write_group(temporary, cell, 9, refrs.span());
    ByteWriter cell_children;
    bethconv::test::write_group(cell_children, cell, 6, temporary.span());
    ByteWriter cell_and_children;
    bethconv::test::write_record(cell_and_children, "CELL", cell,
                                 cell_payload("ModRoom").span());
    cell_and_children.raw(cell_children.span());
    ByteWriter sub_block;
    bethconv::test::write_group(sub_block, 0, 3, cell_and_children.span());
    ByteWriter block;
    bethconv::test::write_group(block, 0, 2, sub_block.span());
    top_group(file, "CELL", block);
    save(dir, "Mod.esp", file);
}

/// Land.esm: a worldspace with a persistent cell and a real cell at (0, 0),
/// the real one with LAND; an LTEX and its TXST.
void make_land(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0x1, {});

    ByteWriter txst_payload;
    ByteWriter tx00;
    tx00.zstring("Landscape\\Dirt01.dds");
    bethconv::test::write_field(txst_payload, "TX00", tx00);
    ByteWriter tx01;
    tx01.zstring("Landscape\\Dirt01_n.dds");
    bethconv::test::write_field(txst_payload, "TX01", tx01);
    ByteWriter txsts;
    bethconv::test::write_record(txsts, "TXST", 0x0000'0C00, txst_payload.span());
    top_group(file, "TXST", txsts);

    ByteWriter ltex_payload;
    ByteWriter edid;
    edid.zstring("LDirt");
    bethconv::test::write_field(ltex_payload, "EDID", edid);
    ByteWriter tnam;
    tnam.u32(0x0000'0C00);
    bethconv::test::write_field(ltex_payload, "TNAM", tnam);
    ByteWriter snam;
    snam.u8(30);
    bethconv::test::write_field(ltex_payload, "SNAM", snam);
    ByteWriter ltexs;
    bethconv::test::write_record(ltexs, "LTEX", 0x0000'0C01, ltex_payload.span());
    top_group(file, "LTEX", ltexs);

    // WATR: DNAM with a recognizable value at every offset read.
    ByteWriter watr;
    ByteWriter watr_id;
    watr_id.zstring("LandRiver");
    bethconv::test::write_field(watr, "EDID", watr_id);
    ByteWriter anam;
    anam.u8(50);
    bethconv::test::write_field(watr, "ANAM", anam);
    ByteWriter water_dnam;
    for (std::size_t offset = 0; offset < 228; offset += 4) {
        if (offset == 40) {
            water_dnam.u32(0x00182726); // shallow, red in the low byte
        } else if (offset == 44 || offset == 48) {
            water_dnam.u32(0xFF000000 | static_cast<std::uint32_t>(offset));
        } else {
            water_dnam.f32(static_cast<float>(offset));
        }
    }
    bethconv::test::write_field(watr, "DNAM", water_dnam);
    for (int i = 0; i < 3; ++i) {
        ByteWriter nnam;
        nnam.zstring("Data\\Textures\\Water\\DefaultWater.dds");
        bethconv::test::write_field(watr, "NNAM", nnam);
    }
    ByteWriter watrs;
    bethconv::test::write_record(watrs, "WATR", 0x0000'0C02, watr.span());
    top_group(file, "WATR", watrs);

    // WTHR: NAM0 colour i*4+t is (i, t, 7); FNAM counts up; one DALC.
    ByteWriter wthr;
    ByteWriter wthr_id;
    wthr_id.zstring("LandClear");
    bethconv::test::write_field(wthr, "EDID", wthr_id);
    ByteWriter nam0;
    for (std::uint32_t colour = 0; colour < 17; ++colour) {
        for (std::uint32_t time = 0; time < 4; ++time) {
            nam0.u32(colour | (time << 8) | (7u << 16));
        }
    }
    bethconv::test::write_field(wthr, "NAM0", nam0);
    ByteWriter fog;
    for (int i = 0; i < 8; ++i) {
        fog.f32(static_cast<float>(1000 * (i + 1)));
    }
    bethconv::test::write_field(wthr, "FNAM", fog);
    ByteWriter dalc;
    for (int i = 0; i < 7; ++i) {
        dalc.u32(0x00010203u * static_cast<std::uint32_t>(i + 1));
    }
    dalc.f32(1.0F);
    bethconv::test::write_field(wthr, "DALC", dalc);
    ByteWriter wthrs;
    bethconv::test::write_record(wthrs, "WTHR", 0x0000'0C03, wthr.span());
    top_group(file, "WTHR", wthrs);

    // CLMT: that weather at 100%, sunrise 5:30-10:00, sunset 16:00-20:30.
    ByteWriter clmt;
    ByteWriter wlst;
    wlst.u32(0x0000'0C03);
    wlst.u32(100);
    wlst.u32(0);
    bethconv::test::write_field(clmt, "WLST", wlst);
    ByteWriter tnam_clmt;
    for (const int v : {33, 60, 96, 123, 0, 0}) {
        tnam_clmt.u8(static_cast<std::uint8_t>(v));
    }
    bethconv::test::write_field(clmt, "TNAM", tnam_clmt);
    ByteWriter clmts;
    bethconv::test::write_record(clmts, "CLMT", 0x0000'0C04, clmt.span());
    top_group(file, "CLMT", clmts);

    const auto exterior = [](std::string_view name, std::uint32_t water) {
        ByteWriter payload;
        ByteWriter id;
        id.zstring(name);
        bethconv::test::write_field(payload, "EDID", id);
        ByteWriter data;
        data.u16(0x0002); // has water
        bethconv::test::write_field(payload, "DATA", data);
        ByteWriter xclc;
        xclc.u32(0);
        xclc.u32(0);
        xclc.u32(0);
        bethconv::test::write_field(payload, "XCLC", xclc);
        if (water != 0) {
            ByteWriter xcwt;
            xcwt.u32(water);
            bethconv::test::write_field(payload, "XCWT", xcwt);
        }
        return payload;
    };

    // LAND: row 0 starts at (1 + 2) * 8 = 24 and drops to 16 at column 1; row 1
    // starts one step higher, at 32.
    ByteWriter land;
    ByteWriter land_data;
    land_data.u32(0x1);
    bethconv::test::write_field(land, "DATA", land_data);
    ByteWriter vhgt;
    vhgt.f32(1.0F);
    for (std::size_t i = 0; i < 33 * 33; ++i) {
        const int delta = i == 0 ? 2 : i == 1 ? -1 : i == 33 ? 1 : 0;
        vhgt.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(delta)));
    }
    vhgt.u8(0);
    vhgt.u8(0);
    vhgt.u8(0);
    bethconv::test::write_field(land, "VHGT", vhgt);
    ByteWriter vclr;
    for (std::size_t i = 0; i < 33 * 33 * 3; ++i) {
        vclr.u8(0x7F);
    }
    bethconv::test::write_field(land, "VCLR", vclr);
    const auto layer = [&](std::string_view type, std::uint32_t texture, std::uint8_t quadrant,
                           std::int16_t index) {
        ByteWriter l;
        l.u32(texture);
        l.u8(quadrant);
        l.u8(0);
        l.u16(static_cast<std::uint16_t>(index));
        bethconv::test::write_field(land, type, l);
    };
    layer("BTXT", 0x0000'0C01, 2, -1);
    // Out of order on disk; world.fb sorts by quadrant, then layer.
    layer("ATXT", 0, 0, 1);
    ByteWriter vtxt;
    vtxt.u16(5);
    vtxt.u16(0);
    vtxt.f32(0.5F);
    vtxt.u16(288);
    vtxt.u16(0);
    vtxt.f32(1.0F);
    bethconv::test::write_field(land, "VTXT", vtxt);
    layer("ATXT", 0x0000'0C01, 0, 0);

    const std::uint32_t world = 0x0000'0D00;
    const std::uint32_t persistent = 0x0000'0D01;
    const std::uint32_t real = 0x0000'0D02;
    ByteWriter children;
    bethconv::test::write_record(children, "CELL", persistent,
                                 exterior("LandPersistent", 0).span(), 0x400);
    ByteWriter land_group;
    bethconv::test::write_record(land_group, "LAND", 0x0000'0D03, land.span());
    ByteWriter temporary;
    bethconv::test::write_group(temporary, real, 9, land_group.span());
    ByteWriter real_children;
    bethconv::test::write_group(real_children, real, 6, temporary.span());
    ByteWriter cells;
    bethconv::test::write_record(cells, "CELL", real, exterior("LandOrigin", 0x0000'0C02).span());
    cells.raw(real_children.span());
    ByteWriter sub_block;
    bethconv::test::write_group(sub_block, 0, 5, cells.span());
    ByteWriter block;
    bethconv::test::write_group(block, 0, 4, sub_block.span());
    children.raw(block.span());

    ByteWriter wrld;
    ByteWriter world_id;
    world_id.zstring("LandWorld");
    bethconv::test::write_field(wrld, "EDID", world_id);
    ByteWriter dnam;
    dnam.f32(-2048.0F);
    dnam.f32(-14000.0F);
    bethconv::test::write_field(wrld, "DNAM", dnam);
    ByteWriter cnam;
    cnam.u32(0x0000'0C04);
    bethconv::test::write_field(wrld, "CNAM", cnam);
    ByteWriter worlds;
    bethconv::test::write_record(worlds, "WRLD", world, wrld.span());
    ByteWriter world_children;
    bethconv::test::write_group(world_children, world, 1, children.span());
    worlds.raw(world_children.span());
    top_group(file, "WRLD", worlds);
    save(dir, "Land.esm", file);
}

} // namespace

TEST_CASE("world.fb resolves payload FormIDs through the winning plugin", "[pack][world]") {
    const TempDir dir;
    make_base(dir);
    make_other(dir);
    make_mod(dir);

    record::PluginList list;
    for (const char* name : {"Base.esm", "Other.esm", "Mod.esp"}) {
        list.plugins.push_back(record::ListedPlugin{.name = name, .active = true});
    }
    const auto order = record::LoadOrder::build(
        dir.path(), list, record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false});
    REQUIRE(order.entries().size() == 3);

    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->cells == 1);
    CHECK(stats->interior_cells == 1);
    CHECK(stats->refs == 3);
    CHECK(stats->bases == 7);
    CHECK(stats->scripts == 3);
    CHECK(stats->locks == 1);
    CHECK(stats->lights == 1);
    CHECK(stats->unresolved == 0);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());

    // The cell itself: local 0x01000B00, global 0x02000B00.
    const auto cell = file->cell(0x0200'0B00);
    REQUIRE(cell.has_value());
    CHECK(cell->interior());
    CHECK(cell->editor_id == "ModRoom");
    CHECK(file->cell_by_editor_id("modroom")->id == 0x0200'0B00);
    REQUIRE(cell->refs.size() == 3);

    // Local 0x01000A00 in Mod.esp is Mod.esp's own record: global 0x02000A00,
    // not 0x01000A00 (which would be Other.esm's space).
    CHECK(cell->refs[0].id == 0x0200'0B01);
    CHECK(cell->refs[0].base == 0x0000'0800);
    CHECK(cell->refs[1].base == 0x0200'0A00);
    CHECK(cell->refs[1].scale == 2.0F);
    CHECK(cell->refs[1].position.x == 20.0F);
    CHECK(cell->refs[1].rotation.z == 1.5F);
    CHECK(cell->refs[2].base == 0x0000'0801);

    const auto chair = file->base(0x0200'0A00);
    REQUIRE(chair.has_value());
    CHECK(chair->model == "meshes/furniture/chair.nif");
    CHECK(file->base(0x0000'0800)->model == "meshes/architecture/wall.nif");
    // Already prefixed paths are not prefixed again.
    CHECK(file->base(0x0100'0900)->model == "meshes/other.nif");

    CHECK(file->base(0x0000'0802)->model == "meshes/armor/iron/shieldgnd.nif");

    const auto light = file->base(0x0000'0801);
    REQUIRE(light.has_value());
    REQUIRE(light->light.has_value());
    CHECK(light->light->radius == 256);
    CHECK(light->light->color == 0x0080'40FFu);

    // Scripts and the rest of the chair reference's extras, with every
    // FormID made global.
    REQUIRE(cell->scripts.size() == 1);
    CHECK(cell->scripts[0].ref == 0x0200'0B02);
    REQUIRE(cell->scripts[0].scripts.size() == 1);
    const auto& lever = cell->scripts[0].scripts[0];
    CHECK(lever.name == "TestLever");
    REQUIRE(lever.properties.size() == 2);
    CHECK(lever.properties[0].objects.at(0).form == record::FormId{0x0200'0B01});
    CHECK(lever.properties[1].integers == std::vector<std::int32_t>{3});
    REQUIRE(cell->locks.size() == 1);
    CHECK(cell->locks[0].level == 255);
    CHECK(cell->locks[0].key == 0x0200'0A00);
    REQUIRE(cell->links.size() == 1);
    CHECK(cell->links[0].target == 0x0200'0B03);
    REQUIRE(cell->activate_parents.size() == 1);
    CHECK(cell->activate_parents[0].parent == 0x0200'0B01);
    CHECK(cell->activate_parents[0].delay == 0.5F);
    CHECK((cell->refs[1].flags & pack::k_ref_parent_activate_only) != 0);
    REQUIRE(cell->primitives.size() == 1);
    CHECK(cell->primitives[0].bounds.x == 64.0F);
    CHECK(cell->primitives[0].type == 1);

    const auto door = file->base(0x0200'0A01);
    REQUIRE(door.has_value());
    CHECK(door->flags == 0x02);
    REQUIRE(door->scripts.size() == 1);
    CHECK(door->scripts[0].properties[0].objects.at(0).form == record::FormId{0x0000'0800});
    const auto trigger = file->base(0x0200'0A02);
    REQUIRE(trigger.has_value());
    CHECK(trigger->model.empty());
    CHECK(trigger->scripts.at(0).properties[0].objects.at(0).form ==
          record::FormId{0x0200'0A01});
}

TEST_CASE("world.fb is refused when damaged", "[pack][world]") {
    std::vector<std::byte> junk(64, std::byte{0x5A});
    CHECK_FALSE(pack::WorldFile::from_bytes(junk, "junk").has_value());
    CHECK_FALSE(pack::WorldFile::from_bytes({}, "empty").has_value());
}

TEST_CASE("world.fb carries terrain, worldspaces and land textures", "[pack][world]") {
    const TempDir dir;
    make_land(dir);
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "Land.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list, record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->worlds == 1);
    CHECK(stats->terrains == 1);
    CHECK(stats->terrain_layers == 3);
    CHECK(stats->land_textures == 1);
    CHECK(stats->parse_errors == 0);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());

    const auto worlds = file->worldspaces();
    REQUIRE(worlds.size() == 1);
    CHECK(worlds[0].editor_id == "LandWorld");
    REQUIRE(worlds[0].defaults.has_value());
    CHECK((*worlds[0].defaults)[0] == -2048.0F);
    CHECK((*worlds[0].defaults)[1] == -14000.0F);

    // The persistent cell shares (0, 0) but is not the cell at (0, 0).
    CHECK(file->cell(0x0000'0D01)->persistent);
    const auto cell = file->cell_at_grid(0x0000'0D00, 0, 0);
    REQUIRE(cell.has_value());
    CHECK(cell->id == 0x0000'0D02);
    CHECK_FALSE(cell->persistent);
    CHECK_FALSE(file->cell_at_grid(0x0000'0D00, 1, 0).has_value());

    REQUIRE(cell->terrain.has_value());
    const auto heights = cell->terrain->heights();
    REQUIRE(heights.size() == 33 * 33);
    CHECK(heights[0] == 24.0F);
    CHECK(heights[1] == 16.0F);
    CHECK(heights[32] == 16.0F);
    CHECK(heights[33] == 32.0F);
    CHECK(heights[33 * 33 - 1] == 32.0F);
    CHECK(cell->terrain->colours.size() == 33 * 33 * 3);

    const auto& layers = cell->terrain->layers;
    REQUIRE(layers.size() == 3);
    CHECK((layers[0].quadrant == 0 && layers[0].layer == 0 && layers[0].texture == 0x0000'0C01));
    CHECK((layers[1].quadrant == 0 && layers[1].layer == 1 && layers[1].texture == 0));
    CHECK(layers[1].points == std::vector<std::uint16_t>{5, 288});
    CHECK(layers[1].opacity == std::vector<std::uint8_t>{128, 255});
    CHECK((layers[2].quadrant == 2 && layers[2].layer == -1));

    const auto dirt = file->land_texture(0x0000'0C01);
    REQUIRE(dirt.has_value());
    CHECK(dirt->editor_id == "LDirt");
    CHECK(dirt->diffuse == "textures/landscape/dirt01.dds");
    CHECK(dirt->normal == "textures/landscape/dirt01_n.dds");
    CHECK(dirt->specular == 30);
    CHECK_FALSE(file->land_texture(0x0000'0C00).has_value());

    CHECK(stats->waters == 1);
    CHECK(cell->water == 0x0000'0C02);
    const auto river = file->water(0x0000'0C02);
    REQUIRE(river.has_value());
    CHECK(river->editor_id == "LandRiver");
    CHECK(river->opacity == 50);
    CHECK(river->shallow_color == 0x00182726u);
    CHECK(river->deep_color == 0x0000002Cu); // alpha byte masked off
    CHECK(river->reflectivity == 20.0F);
    CHECK(river->fog_far == 36.0F);
    CHECK(river->layers[1].wind_speed == 116.0F);
    CHECK(river->layers[2].uv_scale == 180.0F);
    CHECK(river->reflection_magnitude == 196.0F);
    REQUIRE(river->noise.size() == 3);
    CHECK(river->noise[0] == "textures/water/defaultwater.dds");

    CHECK(worlds[0].climate == 0x0000'0C04);
    const auto climate = file->climate(0x0000'0C04);
    REQUIRE(climate.has_value());
    REQUIRE(climate->weathers.size() == 1);
    CHECK(climate->weathers[0].first == 0x0000'0C03);
    CHECK(climate->weathers[0].second == 100);
    CHECK(climate->sun[0] == 5.5F);
    CHECK(climate->sun[3] == 20.5F);
    const auto weather = file->weather(0x0000'0C03);
    REQUIRE(weather.has_value());
    REQUIRE(weather->colors.size() == 68);
    CHECK(weather->colors[8 * 4 + 1] == (8u | (1u << 8) | (7u << 16))); // horizon, day
    REQUIRE(weather->fog.size() == 8);
    CHECK(weather->fog[1] == 2000.0F);
    REQUIRE(weather->directional_ambient.size() == 7);
    CHECK(weather->directional_ambient[1] == 0x00020406u);
}

namespace {

void field_u32(ByteWriter& payload, std::string_view tag, std::uint32_t value) {
    ByteWriter w;
    w.u32(value);
    bethconv::test::write_field(payload, tag, w);
}

void field_text(ByteWriter& payload, std::string_view tag, std::string_view text) {
    ByteWriter w;
    w.zstring(text);
    bethconv::test::write_field(payload, tag, w);
}

/// Quests.esp, masters [Base.esm], loaded third like Mod.esp: a quest whose
/// alias is forced to its own actor, a global and the actor in a cell.
void make_quests(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0, {"Base.esm"});

    ByteWriter glob;
    field_text(glob, "EDID", "QuestCounter");
    ByteWriter fnam;
    fnam.u8('s');
    bethconv::test::write_field(glob, "FNAM", fnam);
    ByteWriter fltv;
    fltv.f32(7.0F);
    bethconv::test::write_field(glob, "FLTV", fltv);
    ByteWriter globs;
    bethconv::test::write_record(globs, "GLOB", 0x0100'0C00, glob.span());
    top_group(file, "GLOB", globs);

    ByteWriter q;
    field_text(q, "EDID", "TestQuest");
    ByteWriter vmad;
    const auto wstring = [&](std::string_view text) {
        vmad.u16(static_cast<std::uint16_t>(text.size()));
        vmad.raw(text);
    };
    vmad.u16(5);
    vmad.u16(2);
    vmad.u16(1);
    wstring("TestQuestScript");
    vmad.u8(0);
    vmad.u16(1);
    wstring("Counter");
    vmad.u8(1);
    vmad.u8(1);
    vmad.u16(0);
    vmad.u16(0xFFFF);
    vmad.u32(0x0100'0C00); // the global, local
    vmad.u8(2);
    vmad.u16(1);
    wstring("QF_TestQuest");
    vmad.u16(10);
    vmad.u16(0);
    vmad.u32(0);
    vmad.u8(1);
    wstring("QF_TestQuest");
    wstring("Fragment_0");
    vmad.u16(1);
    vmad.u16(0);
    vmad.u16(0); // alias 0
    vmad.u32(0);
    vmad.u16(5);
    vmad.u16(2);
    vmad.u16(1);
    wstring("TestAliasScript");
    vmad.u8(0);
    vmad.u16(0);
    bethconv::test::write_field(q, "VMAD", vmad);
    ByteWriter dnam;
    dnam.u16(0x0001);
    dnam.u8(50);
    dnam.u8(0);
    dnam.u32(0);
    dnam.u32(0);
    bethconv::test::write_field(q, "DNAM", dnam);
    ByteWriter indx;
    indx.u16(10);
    indx.u8(0x02);
    indx.u8(0);
    bethconv::test::write_field(q, "INDX", indx);
    ByteWriter qsdt;
    qsdt.u8(0);
    bethconv::test::write_field(q, "QSDT", qsdt);
    field_text(q, "CNAM", "Begun.");
    field_u32(q, "ANAM", 2);
    field_u32(q, "ALST", 0);
    field_text(q, "ALID", "Guard");
    field_u32(q, "ALFR", 0x0100'0B01); // own actor, local
    bethconv::test::write_field(q, "ALED", ByteWriter{});
    field_u32(q, "ALST", 1);
    field_text(q, "ALID", "Boss");
    field_u32(q, "ALUA", 0x0000'0900); // an NPC_ in Base.esm's space
    bethconv::test::write_field(q, "ALED", ByteWriter{});
    ByteWriter quests;
    bethconv::test::write_record(quests, "QUST", 0x0100'0D00, q.span());
    top_group(file, "QUST", quests);

    const std::uint32_t cell = 0x0100'0B00;
    ByteWriter achr;
    field_u32(achr, "NAME", 0x0000'0900);
    ByteWriter data;
    for (const float f : {1.0F, 2.0F, 3.0F, 0.0F, 0.0F, 0.5F}) {
        data.f32(f);
    }
    bethconv::test::write_field(achr, "DATA", data);
    ByteWriter refrs;
    bethconv::test::write_record(refrs, "ACHR", 0x0100'0B01, achr.span());
    ByteWriter temporary;
    bethconv::test::write_group(temporary, cell, 9, refrs.span());
    ByteWriter cell_children;
    bethconv::test::write_group(cell_children, cell, 6, temporary.span());
    ByteWriter cell_and_children;
    bethconv::test::write_record(cell_and_children, "CELL", cell,
                                 cell_payload("QuestRoom").span());
    cell_and_children.raw(cell_children.span());
    ByteWriter sub_block;
    bethconv::test::write_group(sub_block, 0, 3, cell_and_children.span());
    ByteWriter block;
    bethconv::test::write_group(block, 0, 2, sub_block.span());
    top_group(file, "CELL", block);
    save(dir, "Quests.esp", file);
}

} // namespace

TEST_CASE("world.fb carries quests, globals and actors with global FormIDs", "[pack][world]") {
    const TempDir dir;
    make_base(dir);
    make_other(dir);
    make_quests(dir);
    record::PluginList list;
    for (const char* name : {"Base.esm", "Other.esm", "Quests.esp"}) {
        list.plugins.push_back(record::ListedPlugin{.name = name, .active = true});
    }
    const auto order = record::LoadOrder::build(
        dir.path(), list, record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->quests == 1);
    CHECK(stats->quest_aliases == 2);
    CHECK(stats->quest_fragments == 1);
    CHECK(stats->globals == 1);
    CHECK(stats->actors == 1);
    CHECK(stats->parse_errors == 0);
    CHECK(stats->unresolved == 0);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());
    CHECK(file->quest_count() == 1);
    const auto quest = file->quest(0x0200'0D00);
    REQUIRE(quest.has_value());
    CHECK(quest->editor_id == "TestQuest");
    CHECK(quest->flags == 0x0001);
    CHECK(quest->priority == 50);
    REQUIRE(quest->scripts.size() == 1);
    CHECK(quest->scripts[0].properties[0].objects[0].form == record::FormId{0x0200'0C00});
    CHECK(quest->fragment_script == "QF_TestQuest");
    REQUIRE(quest->fragments.size() == 1);
    CHECK(quest->fragments[0].stage == 10);
    CHECK(quest->fragments[0].function == "Fragment_0");
    REQUIRE(quest->stages.size() == 1);
    CHECK(quest->stages[0].flags == 0x02);
    CHECK(quest->stages[0].log.at(0).text == "Begun.");
    REQUIRE(quest->aliases.size() == 2);
    CHECK(quest->aliases[0].name == "Guard");
    CHECK(quest->aliases[0].forced == 0x0200'0B01);
    REQUIRE(quest->aliases[0].scripts.size() == 1);
    CHECK(quest->aliases[0].scripts[0].name == "TestAliasScript");
    CHECK(quest->aliases[1].unique_actor == 0x0000'0900);
    CHECK(quest->aliases[1].scripts.empty());

    const auto counter = file->global(0x0200'0C00);
    REQUIRE(counter.has_value());
    CHECK(counter->kind == 's');
    CHECK(counter->value == 7.0F);

    const auto actors = file->actors();
    REQUIRE(actors.size() == 1);
    CHECK(actors[0].ref == 0x0200'0B01);
    CHECK(actors[0].base == 0x0000'0900);
    CHECK(actors[0].cell == 0x0200'0B00);
    CHECK(actors[0].rotation.z == 0.5F);

    const auto plugins = file->plugins();
    REQUIRE(plugins.size() == 3);
    CHECK(plugins[2].first == "Quests.esp");
    CHECK(plugins[2].second == 0x0200'0000u);
}
