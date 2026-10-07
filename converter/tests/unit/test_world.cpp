// SPDX-License-Identifier: GPL-3.0-or-later
//
// world.fb: cells, references and bases decoded for the engine. The main case
// is a plugin whose master list differs from the load order, where FormIDs in
// payloads are only correct if resolved through the winning plugin.
#include "bethconv/pack/world.hpp"

#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"
#include "skydot_formats/flags.hpp"

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
    ByteWriter xemi;
    xemi.u32(0x0100'0B03);
    bethconv::test::write_field(payload, "XEMI", xemi);
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
    // An editor marker: the header's IsMarker flag (bit 23) reaches Base.
    bethconv::test::write_record(stats, "STAT", 0x0000'0803,
                                 static_payload("XMarker", "MarkerX.nif").span(), 0x0080'0000);
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
    // Two cloud layers, the second disabled by NAM1; layer 0 drifts east.
    ByteWriter cloud0;
    cloud0.zstring("Sky\\Clouds01.dds");
    bethconv::test::write_field(wthr, "00TX", cloud0);
    ByteWriter cloud1;
    cloud1.zstring("Sky\\Clouds02.dds");
    bethconv::test::write_field(wthr, "10TX", cloud1);
    ByteWriter nam1;
    nam1.u32(0x2);
    bethconv::test::write_field(wthr, "NAM1", nam1);
    ByteWriter qnam;
    ByteWriter rnam;
    for (int i = 0; i < 32; ++i) {
        qnam.u8(i == 0 ? 254 : 127);
        rnam.u8(127);
    }
    bethconv::test::write_field(wthr, "QNAM", qnam);
    bethconv::test::write_field(wthr, "RNAM", rnam);
    ByteWriter pnam;
    ByteWriter jnam;
    for (std::uint32_t layer = 0; layer < 32; ++layer) {
        for (std::uint32_t time = 0; time < 4; ++time) {
            pnam.u32(layer | (time << 8));
            jnam.f32(static_cast<float>(time) / 4.0F);
        }
    }
    bethconv::test::write_field(wthr, "PNAM", pnam);
    bethconv::test::write_field(wthr, "JNAM", jnam);
    ByteWriter wthr_data;
    for (const int b : {51, 0, 0, 125, 0, 51, 153, 103, 0, 255, 246, 4, 10, 20, 30, 0, 0, 64, 128}) {
        wthr_data.u8(static_cast<std::uint8_t>(b));
    }
    bethconv::test::write_field(wthr, "DATA", wthr_data);
    ByteWriter mnam;
    mnam.u32(0x0000'0C05);
    bethconv::test::write_field(wthr, "MNAM", mnam);
    ByteWriter wthrs;
    bethconv::test::write_record(wthrs, "WTHR", 0x0000'0C03, wthr.span());
    top_group(file, "WTHR", wthrs);

    // SPGD: rain, in the 48-byte form.
    ByteWriter spgd;
    ByteWriter spgd_id;
    spgd_id.zstring("LandRain");
    bethconv::test::write_field(spgd, "EDID", spgd_id);
    ByteWriter spgd_data;
    for (const float v : {675.0F, 0.0F, 0.35F, 2.0F, 0.0F, 0.0F, 0.0F}) {
        spgd_data.f32(v);
    }
    spgd_data.u32(4);
    spgd_data.u32(2);
    spgd_data.u32(0);
    spgd_data.u32(1300);
    spgd_data.f32(1.0F);
    bethconv::test::write_field(spgd, "DATA", spgd_data);
    ByteWriter icon;
    icon.zstring("Effects\\FXRaindrops.dds");
    bethconv::test::write_field(spgd, "ICON", icon);
    ByteWriter spgds;
    bethconv::test::write_record(spgds, "SPGD", 0x0000'0C05, spgd.span());
    top_group(file, "SPGD", spgds);

    // REGN: a square over the origin cell with that weather, priority 60.
    ByteWriter regn;
    ByteWriter regn_id;
    regn_id.zstring("LandStorms");
    bethconv::test::write_field(regn, "EDID", regn_id);
    ByteWriter wnam;
    wnam.u32(0x0000'0D00);
    bethconv::test::write_field(regn, "WNAM", wnam);
    ByteWriter rpli;
    rpli.u32(0);
    bethconv::test::write_field(regn, "RPLI", rpli);
    ByteWriter rpld;
    for (const float v : {0.0F, 0.0F, 4096.0F, 0.0F, 4096.0F, 4096.0F, 0.0F, 4096.0F}) {
        rpld.f32(v);
    }
    bethconv::test::write_field(regn, "RPLD", rpld);
    ByteWriter rdat;
    rdat.u32(3);
    rdat.u8(1);
    rdat.u8(60);
    rdat.u16(0);
    bethconv::test::write_field(regn, "RDAT", rdat);
    ByteWriter rdwt;
    rdwt.u32(0x0000'0C03);
    rdwt.u32(80);
    rdwt.u32(0);
    bethconv::test::write_field(regn, "RDWT", rdwt);
    ByteWriter regns;
    bethconv::test::write_record(regns, "REGN", 0x0000'0C06, regn.span());
    top_group(file, "REGN", regns);

    // CLMT: that weather at 100%, sunrise 5:30-10:00, sunset 16:00-20:30.
    ByteWriter clmt;
    ByteWriter wlst;
    wlst.u32(0x0000'0C03);
    wlst.u32(100);
    wlst.u32(0);
    bethconv::test::write_field(clmt, "WLST", wlst);
    ByteWriter tnam_clmt;
    for (const int v : {33, 60, 96, 123, 50, 0x80 | 3}) {
        tnam_clmt.u8(static_cast<std::uint8_t>(v));
    }
    bethconv::test::write_field(clmt, "TNAM", tnam_clmt);
    ByteWriter sun;
    sun.zstring("Sky\\Sun.dds");
    bethconv::test::write_field(clmt, "FNAM", sun);
    ByteWriter stars;
    stars.zstring("Sky\\Stars.nif");
    bethconv::test::write_field(clmt, "MODL", stars);
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
    // NAVM: one triangle linked to itself across edge 2, with a door.
    bethconv::test::NavSpec nav;
    nav.world = world;
    nav.vertices = {{0, 0, 24}, {128, 0, 16}, {0, 128, 32}};
    nav.triangles = {{.vertices = {0, 1, 2}, .edges = {-1, -1, 0}, .flags = 0x0404}};
    nav.links = {{.type = 0, .navmesh = 0x0000'0D04, .triangle = 0}};
    nav.doors = {{.triangle = 0, .door = 0x0000'0D10}};
    ByteWriter navm;
    bethconv::test::write_field(navm, "NVNM", bethconv::test::nvnm(nav));
    bethconv::test::write_record(land_group, "NAVM", 0x0000'0D04, navm.span());
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
        dir.path(), list, record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    REQUIRE(order.entries().size() == 3);

    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->cells == 1);
    CHECK(stats->interior_cells == 1);
    CHECK(stats->refs == 3);
    CHECK(stats->bases == 8);
    CHECK(stats->scripts == 3);
    CHECK(stats->locks == 1);
    CHECK(stats->lights == 1);
    CHECK(stats->unresolved == 0);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());

    // The cell itself: local 0x01000B00, global 0x02000B00.
    const auto cell = file->cell(0x0200'0B00);
    REQUIRE(cell.has_value());
    CHECK(pack::is_interior(*cell));
    CHECK(cell->editor_id == "ModRoom");
    CHECK(file->cell_by_editor_id("modroom")->id == 0x0200'0B00);
    REQUIRE(cell->refs.size() == 3);

    // Local 0x01000A00 in Mod.esp is Mod.esp's own record: global 0x02000A00,
    // not 0x01000A00 (which would be Other.esm's space).
    CHECK(cell->refs[0].id() == 0x0200'0B01);
    CHECK(cell->refs[0].base() == 0x0000'0800);
    CHECK(cell->refs[1].base() == 0x0200'0A00);
    CHECK(cell->refs[1].scale() == 2.0F);
    CHECK(cell->refs[1].position().x() == 20.0F);
    CHECK(cell->refs[1].rotation().z() == 1.5F);
    CHECK(cell->refs[2].base() == 0x0000'0801);

    const auto chair = file->base(0x0200'0A00);
    REQUIRE(chair.has_value());
    CHECK(chair->model == "meshes/furniture/chair.nif");
    CHECK(file->base(0x0000'0800)->model == "meshes/architecture/wall.nif");
    CHECK(file->base(0x0000'0800)->record_flags == pack::wfb::RecordFlags::NONE);
    CHECK(file->base(0x0000'0803)->record_flags == pack::wfb::RecordFlags::editor_marker);
    // Already prefixed paths are not prefixed again.
    CHECK(file->base(0x0100'0900)->model == "meshes/other.nif");

    CHECK(file->base(0x0000'0802)->model == "meshes/armor/iron/shieldgnd.nif");

    const auto light = file->base(0x0000'0801);
    REQUIRE(light.has_value());
    REQUIRE((light->has_light && light->light));
    CHECK(light->light->radius() == 256);
    CHECK(light->light->color() == 0x0080'40FFu);

    // Scripts and the rest of the chair reference's extras, with every
    // FormID made global.
    REQUIRE(cell->scripts.size() == 1);
    CHECK(cell->scripts[0]->ref == 0x0200'0B02);
    REQUIRE(cell->scripts[0]->scripts.size() == 1);
    const auto& lever = *cell->scripts[0]->scripts[0];
    CHECK(lever.name == "TestLever");
    REQUIRE(lever.properties.size() == 2);
    CHECK(lever.properties[0]->objects.at(0).form() == 0x0200'0B01);
    CHECK(lever.properties[1]->ints == std::vector<std::int32_t>{3});
    REQUIRE(cell->locks.size() == 1);
    CHECK(cell->locks[0].level() == 255);
    CHECK(cell->locks[0].key() == 0x0200'0A00);
    REQUIRE(cell->links.size() == 1);
    CHECK(cell->links[0].target() == 0x0200'0B03);
    REQUIRE(cell->activate_parents.size() == 1);
    CHECK(cell->activate_parents[0].parent() == 0x0200'0B01);
    CHECK(cell->activate_parents[0].delay() == 0.5F);
    CHECK(skydot::formats::has_flag(cell->refs[1].flags(),
                                    pack::wfb::RefFlags::parent_activate_only));
    REQUIRE(cell->primitives.size() == 1);
    CHECK(cell->primitives[0].bounds().x() == 64.0F);
    CHECK(cell->primitives[0].type() == 1);
    REQUIRE(cell->light_emitters.size() == 1);
    CHECK(cell->light_emitters[0].ref() == 0x0200'0B02);
    CHECK(cell->light_emitters[0].source() == 0x0200'0B03);

    const auto door = file->base(0x0200'0A01);
    REQUIRE(door.has_value());
    CHECK(door->flags == 0x02);
    REQUIRE(door->scripts.size() == 1);
    CHECK(door->scripts[0]->properties[0]->objects.at(0).form() == 0x0000'0800);
    const auto trigger = file->base(0x0200'0A02);
    REQUIRE(trigger.has_value());
    CHECK(trigger->model.empty());
    CHECK(trigger->scripts.at(0)->properties[0]->objects.at(0).form() == 0x0200'0A01);
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
        dir.path(), list, record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->worlds == 1);
    CHECK(stats->terrains == 1);
    CHECK(stats->terrain_layers == 3);
    CHECK(stats->land_textures == 1);
    CHECK(stats->parse_errors == 0);
    CHECK(stats->navmeshes == 1);
    CHECK(stats->nav_triangles == 1);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());

    const auto worlds = file->worldspaces();
    REQUIRE(worlds.size() == 1);
    CHECK(worlds[0].editor_id == "LandWorld");
    REQUIRE(worlds[0].has_defaults);
    CHECK(worlds[0].default_land_height == -2048.0F);
    CHECK(worlds[0].default_water_height == -14000.0F);

    // The persistent cell shares (0, 0) but is not the cell at (0, 0).
    CHECK(file->cell(0x0000'0D01)->persistent);
    const auto cell = file->cell_at_grid(0x0000'0D00, 0, 0);
    REQUIRE(cell.has_value());
    CHECK(cell->id == 0x0000'0D02);
    CHECK_FALSE(cell->persistent);
    CHECK_FALSE(file->cell_at_grid(0x0000'0D00, 1, 0).has_value());

    REQUIRE(cell->navmeshes.size() == 1);
    const auto& nav = *cell->navmeshes[0];
    CHECK(nav.id == 0x0000'0D04);
    REQUIRE(nav.vertices.size() == 3);
    CHECK((nav.vertices[2].x() == 0 && nav.vertices[2].y() == 128 && nav.vertices[2].z() == 32));
    REQUIRE(nav.triangles.size() == 1);
    CHECK((nav.triangles[0].e0() == -1 && nav.triangles[0].e1() == -1 &&
           nav.triangles[0].e2() == 0));
    CHECK(nav.triangles[0].flags() == (pack::wfb::NavTriangleFlags::edge2_link |
                                       pack::wfb::NavTriangleFlags::in_front_of_door));
    REQUIRE(nav.links.size() == 1);
    CHECK(nav.links[0].navmesh() == 0x0000'0D04);
    REQUIRE(nav.doors.size() == 1);
    CHECK(nav.doors[0].door() == 0x0000'0D10);
    CHECK(file->cell(0x0000'0D01)->navmeshes.empty());

    REQUIRE(cell->terrain);
    const auto heights = pack::terrain_heights(*cell->terrain);
    REQUIRE(heights.size() == 33 * 33);
    CHECK(heights[0] == 24.0F);
    CHECK(heights[1] == 16.0F);
    CHECK(heights[32] == 16.0F);
    CHECK(heights[33] == 32.0F);
    CHECK(heights[33 * 33 - 1] == 32.0F);
    CHECK(cell->terrain->colours.size() == 33 * 33 * 3);

    const auto& layers = cell->terrain->layers;
    REQUIRE(layers.size() == 3);
    CHECK((layers[0]->quadrant == 0 && layers[0]->layer == 0 && layers[0]->texture == 0x0000'0C01));
    CHECK((layers[1]->quadrant == 0 && layers[1]->layer == 1 && layers[1]->texture == 0));
    CHECK(layers[1]->points == std::vector<std::uint16_t>{5, 288});
    CHECK(layers[1]->opacity == std::vector<std::uint8_t>{128, 255});
    CHECK((layers[2]->quadrant == 2 && layers[2]->layer == -1));

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
    CHECK(river->layers[1].wind_speed() == 116.0F);
    CHECK(river->layers[2].uv_scale() == 180.0F);
    CHECK(river->reflection_magnitude == 196.0F);
    REQUIRE(river->noise.size() == 3);
    CHECK(river->noise[0] == "textures/water/defaultwater.dds");

    CHECK(worlds[0].climate == 0x0000'0C04);
    const auto climate = file->climate(0x0000'0C04);
    REQUIRE(climate.has_value());
    REQUIRE(climate->weathers.size() == 1);
    CHECK(climate->weathers[0].weather() == 0x0000'0C03);
    CHECK(climate->weathers[0].chance() == 100);
    CHECK(climate->sunrise_begin == 5.5F);
    CHECK(climate->sunset_end == 20.5F);
    const auto weather = file->weather(0x0000'0C03);
    REQUIRE(weather.has_value());
    REQUIRE(weather->colors.size() == 68);
    CHECK(weather->colors[8 * 4 + 1] == (8u | (1u << 8) | (7u << 16))); // horizon, day
    REQUIRE(weather->fog.size() == 8);
    CHECK(weather->fog[1] == 2000.0F);
    REQUIRE(weather->directional_ambient.size() == 7);
    CHECK(weather->directional_ambient[1] == 0x00020406u);

    // Sky, moons (only Secunda here, three days a phase), and the stars.
    CHECK(climate->sun_texture == "textures/sky/sun.dds");
    CHECK(climate->sky == "meshes/sky/stars.nif");
    CHECK(climate->volatility == 50);
    CHECK(climate->moons == 0x2);
    CHECK(climate->phase_length == 3);

    REQUIRE(weather->clouds.size() == 29);
    CHECK(weather->clouds[0]->texture == "textures/sky/clouds01.dds");
    CHECK(weather->clouds[0]->enabled);
    CHECK_FALSE(weather->clouds[1]->enabled); // NAM1 bit 1
    CHECK_FALSE(weather->clouds[2]->enabled); // no texture
    CHECK(weather->clouds[0]->speed_x == 1.0F);
    CHECK(weather->clouds[0]->speed_y == 0.0F);
    CHECK(weather->clouds[1]->colors[2] == (1u | (2u << 8)));
    CHECK(weather->clouds[1]->alphas[3] == 0.75F);
    CHECK(weather->classification == pack::wfb::WeatherClass::rainy);
    CHECK(weather->wind_speed == 0.2F);
    CHECK(weather->wind_direction == 90.0F);
    CHECK(weather->lightning_color == 0x001E140Au);
    CHECK(weather->precipitation == 0x0000'0C05);

    CHECK(stats->precipitations == 1);
    const auto rain = file->precipitation(0x0000'0C05);
    REQUIRE(rain.has_value());
    CHECK(rain->texture == "textures/effects/fxraindrops.dds");
    CHECK(rain->gravity_velocity == 675.0F);
    CHECK(rain->size_y == 2.0F);
    CHECK((rain->subtextures_x == 4 && rain->subtextures_y == 2));
    CHECK(rain->type == 0);
    CHECK(rain->box_size == 1300);

    const auto regions = file->regions();
    REQUIRE(regions.size() == 1);
    CHECK(regions[0].editor_id == "LandStorms");
    CHECK(regions[0].world == 0x0000'0D00);
    CHECK(regions[0].weather_priority == 60);
    CHECK(regions[0].weather_override);
    REQUIRE(regions[0].areas.size() == 1);
    CHECK(regions[0].areas[0]->points.size() == 8);
    REQUIRE(regions[0].weathers.size() == 1);
    CHECK((regions[0].weathers[0].weather() == 0x0000'0C03 && regions[0].weathers[0].chance() == 80));
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
    ByteWriter xlkr;
    xlkr.u32(0x0000'0A00); // a keyword in Base.esm's space
    xlkr.u32(0x0100'0B01); // itself, local
    bethconv::test::write_field(achr, "XLKR", xlkr);
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
        dir.path(), list, record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
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
    CHECK(quest->flags == pack::wfb::QuestFlags::start_game_enabled);
    CHECK(quest->priority == 50);
    REQUIRE(quest->scripts.size() == 1);
    CHECK(quest->scripts[0]->properties[0]->objects[0].form() == 0x0200'0C00);
    CHECK(quest->fragment_script == "QF_TestQuest");
    REQUIRE(quest->fragments.size() == 1);
    CHECK(quest->fragments[0]->stage == 10);
    CHECK(quest->fragments[0]->function == "Fragment_0");
    REQUIRE(quest->stages.size() == 1);
    CHECK(quest->stages[0]->flags == pack::wfb::StageFlags::start_up);
    CHECK(quest->stages[0]->log.at(0)->text == "Begun.");
    REQUIRE(quest->aliases.size() == 2);
    CHECK(quest->aliases[0]->name == "Guard");
    CHECK(quest->aliases[0]->forced == 0x0200'0B01);
    REQUIRE(quest->aliases[0]->scripts.size() == 1);
    CHECK(quest->aliases[0]->scripts[0]->name == "TestAliasScript");
    CHECK(quest->aliases[1]->unique_actor == 0x0000'0900);
    CHECK(quest->aliases[1]->scripts.empty());

    const auto counter = file->global(0x0200'0C00);
    REQUIRE(counter.has_value());
    CHECK(counter->kind == static_cast<std::uint8_t>('s'));
    CHECK(counter->value == 7.0F);

    const auto actors = file->actors();
    REQUIRE(actors.size() == 1);
    CHECK(actors[0].ref() == 0x0200'0B01);
    CHECK(actors[0].base() == 0x0000'0900);
    CHECK(actors[0].cell() == 0x0200'0B00);
    CHECK(actors[0].rotation().z() == 0.5F);
    // A placed actor's linked references are its cell's, like a reference's.
    const auto room = file->cell(0x0200'0B00);
    REQUIRE(room.has_value());
    REQUIRE(room->links.size() == 1);
    CHECK(room->links[0].ref() == 0x0200'0B01);
    CHECK(room->links[0].keyword() == 0x0000'0A00);
    CHECK(room->links[0].target() == 0x0200'0B01);

    const auto plugins = file->plugins();
    REQUIRE(plugins.size() == 3);
    CHECK(plugins[2].name == "Quests.esp");
    CHECK(plugins[2].prefix == 0x0200'0000u);
}

namespace {

/// Actors.esp, masters [Base.esm], loaded third: a race, its skin (an ARMO
/// with one addon), an outfit and a female NPC wearing it.
void make_actors(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0, {"Base.esm"});

    ByteWriter race;
    field_text(race, "EDID", "TestRace");
    field_u32(race, "WNAM", 0x0100'0E10);
    ByteWriter data;
    for (int i = 0; i < 16; ++i) {
        data.u8(0);
    }
    data.f32(1.0F);
    data.f32(0.95F);
    data.f32(1.0F);
    data.f32(1.0F);
    data.u32(1);
    for (int i = 36; i < 164; ++i) {
        data.u8(0);
    }
    bethconv::test::write_field(race, "DATA", data);
    bethconv::test::write_field(race, "MNAM", ByteWriter{});
    field_text(race, "ANAM", "Actors\\Character\\Character Assets\\skeleton.nif");
    bethconv::test::write_field(race, "FNAM", ByteWriter{});
    field_text(race, "ANAM", "Actors\\Character\\Character Assets Female\\skeleton_female.nif");
    bethconv::test::write_field(race, "NAM3", ByteWriter{});
    bethconv::test::write_field(race, "MNAM", ByteWriter{});
    field_text(race, "MODL", "Actors\\Character\\DefaultMale.hkx");
    ByteWriter races;
    bethconv::test::write_record(races, "RACE", 0x0100'0E00, race.span());
    top_group(file, "RACE", races);

    ByteWriter addon;
    field_text(addon, "EDID", "TestSkinAddon");
    ByteWriter bod2;
    bod2.u32(0x4);
    bod2.u32(2);
    bethconv::test::write_field(addon, "BOD2", bod2);
    field_u32(addon, "RNAM", 0x0100'0E00);
    field_text(addon, "MOD2", "Actors\\Character\\Character Assets\\MaleBody_1.nif");
    field_text(addon, "MOD3", "Actors\\Character\\Character Assets\\FemaleBody_1.nif");
    ByteWriter addons;
    bethconv::test::write_record(addons, "ARMA", 0x0100'0E11, addon.span());
    top_group(file, "ARMA", addons);

    ByteWriter skin;
    field_text(skin, "EDID", "TestSkin");
    bethconv::test::write_field(skin, "BOD2", bod2);
    field_u32(skin, "RNAM", 0x0100'0E00);
    field_u32(skin, "MODL", 0x0100'0E11);
    ByteWriter armors;
    bethconv::test::write_record(armors, "ARMO", 0x0100'0E10, skin.span());
    top_group(file, "ARMO", armors);

    ByteWriter outfit;
    field_text(outfit, "EDID", "TestOutfit");
    field_u32(outfit, "INAM", 0x0100'0E10);
    ByteWriter outfits;
    bethconv::test::write_record(outfits, "OTFT", 0x0100'0E20, outfit.span());
    top_group(file, "OTFT", outfits);

    ByteWriter npc;
    field_text(npc, "EDID", "TestNpc");
    ByteWriter acbs;
    acbs.u32(0x1); // female
    for (int i = 0; i < 10; ++i) {
        acbs.u16(0);
    }
    bethconv::test::write_field(npc, "ACBS", acbs);
    field_u32(npc, "RNAM", 0x0100'0E00);
    field_u32(npc, "DOFT", 0x0100'0E20);
    ByteWriter height;
    height.f32(1.05F);
    bethconv::test::write_field(npc, "NAM6", height);
    ByteWriter tone;
    for (const float f : {0.9F, 0.75F, 0.6F}) {
        tone.f32(f);
    }
    bethconv::test::write_field(npc, "QNAM", tone);
    field_u32(npc, "PKID", 0x0100'0E40);
    ByteWriter snam;
    snam.u32(0x0000'0A10); // a faction in Base.esm's space
    snam.u32(2);
    bethconv::test::write_field(npc, "SNAM", snam);
    field_u32(npc, "DPLT", 0x0100'0E50);
    ByteWriter npcs;
    bethconv::test::write_record(npcs, "NPC_", 0x0100'0E30, npc.span());
    top_group(file, "NPC_", npcs);

    // A package: GetIsID on the NPC (a FormID parameter), sandboxing near a
    // reference (a FormID) and targeting an object type (not a FormID).
    ByteWriter pack;
    field_text(pack, "EDID", "TestSandbox8x4");
    ByteWriter pkdt;
    pkdt.u32(0x4);
    pkdt.u8(18);
    pkdt.u8(0);
    pkdt.u8(0);
    pkdt.u8(0);
    pkdt.u16(0);
    pkdt.u16(0);
    bethconv::test::write_field(pack, "PKDT", pkdt);
    ByteWriter psdt;
    for (const int b : {0xFF, 0xFF, 0x00, 0x08, 0xFF, 0x00, 0x00, 0x00}) {
        psdt.u8(static_cast<std::uint8_t>(b));
    }
    psdt.u32(240);
    bethconv::test::write_field(pack, "PSDT", psdt);
    ByteWriter ctda;
    ctda.u32(0);
    ctda.f32(1.0F);
    ctda.u16(72); // GetIsID
    ctda.u16(0);
    ctda.u32(0x0100'0E30);
    ctda.u32(0);
    ctda.u32(0);
    ctda.u32(0);
    ctda.u32(0xFFFF'FFFF);
    bethconv::test::write_field(pack, "CTDA", ctda);
    ByteWriter pkcu;
    pkcu.u32(3);
    pkcu.u32(0x0100'0E41);
    pkcu.u32(1);
    bethconv::test::write_field(pack, "PKCU", pkcu);
    field_text(pack, "ANAM", "Location");
    ByteWriter pldt;
    pldt.u32(0);
    pldt.u32(0x0100'0E30);
    pldt.u32(300);
    bethconv::test::write_field(pack, "PLDT", pldt);
    field_text(pack, "ANAM", "TargetSelector");
    ByteWriter ptda;
    ptda.u32(2);
    ptda.u32(27); // chairs
    ptda.u32(1);
    bethconv::test::write_field(pack, "PTDA", ptda);
    field_text(pack, "ANAM", "Int");
    field_u32(pack, "CNAM", 7);
    for (const int key : {0, 3, 5}) {
        ByteWriter unam;
        unam.u8(static_cast<std::uint8_t>(key));
        bethconv::test::write_field(pack, "UNAM", unam);
    }
    ByteWriter xnam;
    xnam.u8(6);
    bethconv::test::write_field(pack, "XNAM", xnam);
    ByteWriter packs;
    bethconv::test::write_record(packs, "PACK", 0x0100'0E40, pack.span());
    top_group(file, "PACK", packs);

    ByteWriter flst;
    field_text(flst, "EDID", "TestDefaultPackages");
    field_u32(flst, "LNAM", 0x0100'0E40);
    field_u32(flst, "LNAM", 0x0000'0901); // in Base.esm's space
    ByteWriter lists;
    bethconv::test::write_record(lists, "FLST", 0x0100'0E50, flst.span());
    top_group(file, "FLST", lists);
    save(dir, "Actors.esp", file);
}

} // namespace

TEST_CASE("world.fb carries what actors are built from, with global FormIDs", "[pack][world]") {
    const TempDir dir;
    make_base(dir);
    make_other(dir);
    make_actors(dir);
    record::PluginList list;
    for (const char* name : {"Base.esm", "Other.esm", "Actors.esp"}) {
        list.plugins.push_back(record::ListedPlugin{.name = name, .active = true});
    }
    const auto order = record::LoadOrder::build(
        dir.path(), list, record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->npcs == 1);
    CHECK(stats->races == 1);
    CHECK(stats->armors == 2); // and Base.esm's placed armor
    CHECK(stats->armor_addons == 1);
    CHECK(stats->outfits == 1);
    CHECK(stats->parse_errors == 0);
    CHECK(stats->unresolved == 0);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());
    const auto npc = file->npc(0x0200'0E30);
    REQUIRE(npc.has_value());
    CHECK(npc->editor_id == "TestNpc");
    CHECK(skydot::formats::has_flag(npc->flags, pack::wfb::NpcFlags::female));
    CHECK(npc->race == 0x0200'0E00);
    CHECK(npc->default_outfit == 0x0200'0E20);
    CHECK(npc->height == 1.05F);
    CHECK(npc->skin_tone == std::vector<float>{0.9F, 0.75F, 0.6F});
    // Named by the defining plugin and the form's id within it.
    CHECK(npc->face_model == "meshes/actors/character/facegendata/facegeom/actors.esp/00000e30.nif");

    const auto race = file->race(0x0200'0E00);
    REQUIRE(race.has_value());
    CHECK(race->skin == 0x0200'0E10);
    CHECK(race->skeletons.at(0) == "meshes/actors/character/character assets/skeleton.nif");
    CHECK(race->skeletons.at(1) == "meshes/actors/character/character assets female/skeleton_female.nif");
    CHECK(race->behaviours.at(0) == "meshes/actors/character/defaultmale.hkx");
    CHECK(race->heights.at(1) == 0.95F);

    const auto addon = file->armor_addon(0x0200'0E11);
    REQUIRE(addon.has_value());
    CHECK(addon->race == 0x0200'0E00);
    CHECK(addon->slots == 4);
    CHECK(addon->female_model == "meshes/actors/character/character assets/femalebody_1.nif");

    CHECK(stats->packages == 1);
    CHECK(npc->packages == std::vector<std::uint32_t>{0x0200'0E40});
    CHECK(npc->default_packages == std::vector<std::uint32_t>{0x0200'0E40, 0x0000'0901});
    REQUIRE(npc->factions.size() == 1);
    CHECK(npc->factions[0].faction() == 0x0000'0A10);
    CHECK(npc->factions[0].rank() == 2);
    CHECK(file->package_count() == 1);
    const auto pack = file->package(0x0200'0E40);
    REQUIRE(pack.has_value());
    CHECK(pack->editor_id == "TestSandbox8x4");
    CHECK(pack->type == 18);
    CHECK(pack->flags == pack::wfb::PackageFlags::must_complete);
    CHECK(pack->hour == 8);
    CHECK(pack->duration == 240);
    CHECK(pack->template_ == 0x0200'0E41);
    REQUIRE(pack->conditions.size() == 1);
    CHECK(pack->conditions[0]->function == 72);
    CHECK(pack->conditions[0]->param1 == 0x0200'0E30);
    REQUIRE(pack->inputs.size() == 3);
    CHECK(pack->inputs[0]->key == 0);
    CHECK(pack->inputs[0]->location_type == 0);
    CHECK(pack->inputs[0]->location_value == 0x0200'0E30);
    CHECK(pack->inputs[0]->location_radius == 300);
    CHECK(pack->inputs[0]->target_type == -1);
    CHECK(pack->inputs[1]->key == 3);
    CHECK(pack->inputs[1]->target_type == 2);
    CHECK(pack->inputs[1]->target_value == 27); // an object type stays as it is
    CHECK(pack->inputs[2]->type == "Int");
    CHECK(pack->inputs[2]->number == 7.0F);
    CHECK(pack->branches.empty());
}

namespace {

constexpr std::uint32_t k_lt_full = 0x0000'0A01;   // LGTM, 92-byte DATA, DALC 32
constexpr std::uint32_t k_lt_to_spec = 0x0000'0A02; // 72-byte DATA, DALC 24
constexpr std::uint32_t k_lt_to_ambient = 0x0000'0A03; // 64-byte DATA, DALC 24
constexpr std::uint32_t k_lt_bad = 0x0000'0A04;     // 80-byte DATA: no layout

/// The first `size` bytes of the 92-byte lighting layout of XCLL and LGTM DATA
/// (UESP, CELL XCLL and LGTM DATA; xEdit's definitions): the shorter layouts
/// of Skyrim.esm are prefixes of it.
ByteWriter lighting_bytes(std::size_t size, std::uint32_t ambient, float fog_near,
                          std::uint32_t inherit = 0) {
    ByteWriter all;
    all.u32(ambient);
    all.u32(0x0011'2233); // directional
    all.u32(0x0040'3020); // fog near colour
    all.f32(fog_near);
    all.f32(3000.0F); // fog far
    all.u32(10);      // rotation XY
    all.u32(90);      // rotation Z
    all.f32(0.5F);    // directional fade
    all.f32(100.0F);  // fog clip distance
    all.f32(0.7F);    // fog power
    for (std::uint32_t i = 1; i <= 6; ++i) {
        all.u32(0x00A0'0000 + i); // directional ambient x+ .. z-
    }
    all.u32(0x00AB'CDEF); // specular
    all.f32(1.0F);        // fresnel power
    all.u32(0x0060'4050); // fog far colour
    all.f32(0.6F);        // fog max
    all.f32(200.0F);      // light fade begin
    all.f32(800.0F);      // light fade end
    all.u32(inherit);
    ByteWriter cut;
    cut.raw(all.span().first(size));
    return cut;
}

void lighting_template(ByteWriter& group, std::uint32_t id, std::string_view editor_id,
                       std::size_t data_size, std::size_t dalc_size, std::uint32_t ambient) {
    ByteWriter payload;
    ByteWriter edid;
    edid.zstring(editor_id);
    bethconv::test::write_field(payload, "EDID", edid);
    bethconv::test::write_field(payload, "DATA", lighting_bytes(data_size, ambient, 500.0F));
    ByteWriter dalc;
    for (std::uint32_t i = 1; i <= 6; ++i) {
        dalc.u32(0x00B0'0000 + i);
    }
    dalc.u32(0x0001'0203); // specular
    dalc.f32(1.0F);        // fresnel power
    ByteWriter cut;
    cut.raw(dalc.span().first(dalc_size));
    bethconv::test::write_field(payload, "DALC", cut);
    bethconv::test::write_record(group, "LGTM", id, payload.span());
}

/// An interior CELL; `xcll_size` 0 leaves the XCLL out, `tmpl` 0 the LTMP.
void lit_cell(ByteWriter& group, std::uint32_t id, std::string_view editor_id,
              std::size_t xcll_size, std::uint32_t inherit, std::uint32_t tmpl) {
    ByteWriter payload = cell_payload(editor_id);
    if (xcll_size != 0) {
        bethconv::test::write_field(payload, "XCLL",
                                    lighting_bytes(xcll_size, 0x0000'0011, 250.0F, inherit));
    }
    if (tmpl != 0) {
        ByteWriter ltmp;
        ltmp.u32(tmpl);
        bethconv::test::write_field(payload, "LTMP", ltmp);
    }
    bethconv::test::write_record(group, "CELL", id, payload.span());
}

/// Lighting.esm: four lighting templates (92, 72, 64 and 80 bytes of DATA) and
/// interiors that use them.
void make_lighting(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0x1, {});
    ByteWriter templates;
    lighting_template(templates, k_lt_full, "FullTemplate", 92, 32, 0x0000'0101);
    lighting_template(templates, k_lt_to_spec, "SpecTemplate", 72, 24, 0x0000'0202);
    lighting_template(templates, k_lt_to_ambient, "AmbientTemplate", 64, 24, 0x0000'0303);
    lighting_template(templates, k_lt_bad, "BadTemplate", 80, 32, 0x0000'0404);
    top_group(file, "LGTM", templates);

    constexpr std::uint32_t all_inherited = 0x0000'07FF;
    ByteWriter cells;
    lit_cell(cells, 0x0000'0B01, "OwnFull", 92, 0, 0);
    lit_cell(cells, 0x0000'0B02, "OwnToSpecular", 72, 0, 0);
    lit_cell(cells, 0x0000'0B03, "OwnToAmbient", 64, 0, 0);
    lit_cell(cells, 0x0000'0B04, "OwnBadSize", 80, 0, 0);
    lit_cell(cells, 0x0000'0B05, "NoLighting", 0, 0, 0);
    lit_cell(cells, 0x0000'0B06, "OnlyFull", 0, 0, k_lt_full);
    lit_cell(cells, 0x0000'0B07, "OnlyToSpecular", 0, 0, k_lt_to_spec);
    lit_cell(cells, 0x0000'0B08, "OnlyToAmbient", 0, 0, k_lt_to_ambient);
    lit_cell(cells, 0x0000'0B09, "InheritsToAmbient", 92, all_inherited, k_lt_to_ambient);
    lit_cell(cells, 0x0000'0B0A, "InheritsFull", 92, all_inherited, k_lt_full);
    lit_cell(cells, 0x0000'0B0B, "OnlyBad", 0, 0, k_lt_bad);
    lit_cell(cells, 0x0000'0B0C, "OwnTooShort", 40, 0, 0);
    ByteWriter sub_block;
    bethconv::test::write_group(sub_block, 0, 3, cells.span());
    ByteWriter block;
    bethconv::test::write_group(block, 0, 2, sub_block.span());
    top_group(file, "CELL", block);
    save(dir, "Lighting.esm", file);
}

} // namespace

TEST_CASE("world.fb decodes the 64, 72 and 92-byte lighting layouts", "[pack][world][lighting]") {
    const TempDir dir;
    make_lighting(dir);
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "Lighting.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list, record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->cells == 12);
    CHECK(stats->lighting_templates == 3);
    // The template and the cell whose DATA/XCLL has no layout: 80 bytes of
    // LGTM DATA, 80 and 40 of XCLL.
    CHECK(stats->parse_errors == 3);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());
    const auto cell = [&](std::string_view editor_id) {
        const auto found = file->cell_by_editor_id(editor_id);
        REQUIRE(found.has_value());
        return *found;
    };

    // 92 bytes: every field is read.
    const auto full = cell("OwnFull");
    REQUIRE(full.has_lighting);
    CHECK(full.lighting->ambient() == 0x11);
    CHECK(full.lighting->directional() == 0x0011'2233);
    CHECK(full.lighting->fog_near_color() == 0x0040'3020);
    CHECK(full.lighting->fog_near() == 250.0F);
    CHECK(full.lighting->fog_far() == 3000.0F);
    CHECK(full.lighting->directional_rotation_xy() == 10);
    CHECK(full.lighting->directional_rotation_z() == 90);
    CHECK(full.lighting->directional_fade() == 0.5F);
    CHECK(full.lighting->fog_power() == 0.7F);
    CHECK(full.lighting->fog_far_color() == 0x0060'4050);
    CHECK(full.lighting->fog_max() == 0.6F);
    CHECK(full.lighting->light_fade_begin() == 200.0F);
    CHECK(full.lighting->light_fade_end() == 800.0F);
    REQUIRE(full.directional_ambient.size() == 6);
    CHECK(full.directional_ambient[0] == 0x00A0'0001);
    CHECK(full.directional_ambient[5] == 0x00A0'0006);

    // 72 and 64 bytes: what is there is read as in the full layout, what is
    // not is neutral (fog far colour = near colour, fog max 1, no fade
    // distances, nothing inherited).
    for (const auto name : {"OwnToSpecular", "OwnToAmbient"}) {
        const auto c = cell(name);
        REQUIRE(c.has_lighting);
        CHECK(c.lighting->ambient() == 0x11);
        CHECK(c.lighting->directional() == 0x0011'2233);
        CHECK(c.lighting->fog_near() == 250.0F);
        CHECK(c.lighting->fog_far() == 3000.0F);
        CHECK(c.lighting->directional_rotation_z() == 90);
        CHECK(c.lighting->directional_fade() == 0.5F);
        CHECK(c.lighting->fog_power() == 0.7F);
        CHECK(c.lighting->fog_near_color() == 0x0040'3020);
        CHECK(c.lighting->fog_far_color() == 0x0040'3020);
        CHECK(c.lighting->fog_max() == 1.0F);
        CHECK(c.lighting->light_fade_begin() == 0.0F);
        CHECK(c.lighting->light_fade_end() == 0.0F);
        CHECK(c.lighting->inherit() == 0);
        REQUIRE(c.directional_ambient.size() == 6);
        CHECK(c.directional_ambient[0] == 0x00A0'0001);
    }

    // A size that is no layout is no lighting; the cell is still there.
    CHECK_FALSE(cell("OwnBadSize").has_lighting);
    CHECK_FALSE(cell("OwnTooShort").has_lighting);
    CHECK_FALSE(cell("NoLighting").has_lighting);

    // A cell without XCLL takes its template's, whatever the template's DATA
    // size; the directional ambient is the DALC (24 or 32 bytes), not the
    // template's own block.
    const auto only_full = cell("OnlyFull");
    REQUIRE(only_full.has_lighting);
    CHECK(only_full.lighting->ambient() == 0x0101);
    CHECK(only_full.lighting->fog_near() == 500.0F);
    CHECK(only_full.lighting->fog_far_color() == 0x0060'4050);
    CHECK(only_full.lighting->fog_max() == 0.6F);
    CHECK(only_full.lighting->light_fade_end() == 800.0F);
    CHECK(only_full.lighting->inherit() == 0);
    REQUIRE(only_full.directional_ambient.size() == 6);
    CHECK(only_full.directional_ambient[0] == 0x00B0'0001);
    for (const auto& [name, ambient] :
         {std::pair{"OnlyToSpecular", 0x0202U}, std::pair{"OnlyToAmbient", 0x0303U}}) {
        const auto c = cell(name);
        REQUIRE(c.has_lighting);
        CHECK(c.lighting->ambient() == ambient);
        CHECK(c.lighting->directional() == 0x0011'2233);
        CHECK(c.lighting->fog_near() == 500.0F);
        CHECK(c.lighting->fog_far_color() == c.lighting->fog_near_color());
        CHECK(c.lighting->fog_max() == 1.0F);
        CHECK(c.lighting->light_fade_begin() == 0.0F);
        CHECK(c.lighting->light_fade_end() == 0.0F);
        CHECK(c.lighting->inherit() == 0);
        REQUIRE(c.directional_ambient.size() == 6);
        CHECK(c.directional_ambient[0] == 0x00B0'0001);
        CHECK(c.directional_ambient[5] == 0x00B0'0006);
    }

    // An XCLL that inherits everything gets the template's values, the
    // neutral ones of a short template included.
    const auto inherits = cell("InheritsToAmbient");
    REQUIRE(inherits.has_lighting);
    CHECK(inherits.lighting->ambient() == 0x0303);
    CHECK(inherits.lighting->fog_near() == 500.0F);
    CHECK(inherits.lighting->fog_near_color() == 0x0040'3020);
    CHECK(inherits.lighting->fog_far_color() == 0x0040'3020);
    CHECK(inherits.lighting->fog_max() == 1.0F);
    CHECK(inherits.lighting->light_fade_begin() == 0.0F);
    CHECK(inherits.lighting->light_fade_end() == 0.0F);
    REQUIRE(inherits.directional_ambient.size() == 6);
    CHECK(inherits.directional_ambient[0] == 0x00B0'0001);
    const auto inherits_full = cell("InheritsFull");
    REQUIRE(inherits_full.has_lighting);
    CHECK(inherits_full.lighting->ambient() == 0x0101);
    CHECK(inherits_full.lighting->fog_max() == 0.6F);
    CHECK(inherits_full.lighting->light_fade_begin() == 200.0F);

    // A template with no layout is none: the cell keeps what it has.
    CHECK_FALSE(cell("OnlyBad").has_lighting);
}

namespace {

/// LargeWorld.esm: one exterior cell with four references, A (0xE10) and D
/// (0xE13) plain, B (0xE11) initially disabled and C (0xE12) deleted, and a
/// WRLD whose RNAM lists A, B, C, D and a reference that does not exist (E,
/// 0xE14) for the cell (0, 0), and D then A for the cell x 1, y 0.
void make_large_refs(const TempDir& dir) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0x1, {});

    const std::uint32_t world = 0x0000'0E00;
    const std::uint32_t real = 0x0000'0E02;
    ByteWriter cell_payload;
    ByteWriter cell_id;
    cell_id.zstring("LargeCell");
    bethconv::test::write_field(cell_payload, "EDID", cell_id);
    ByteWriter xclc;
    xclc.u32(0);
    xclc.u32(0);
    xclc.u32(0);
    bethconv::test::write_field(cell_payload, "XCLC", xclc);

    ByteWriter refs;
    bethconv::test::write_record(refs, "REFR", 0x0000'0E10, refr_payload(0x0000'0800, 1.0F, 1.0F).span());
    bethconv::test::write_record(refs, "REFR", 0x0000'0E11, refr_payload(0x0000'0800, 2.0F, 1.0F).span(),
                                 0x800);
    bethconv::test::write_record(refs, "REFR", 0x0000'0E12, ByteWriter{}.span(), 0x20);
    bethconv::test::write_record(refs, "REFR", 0x0000'0E13, refr_payload(0x0000'0800, 4.0F, 2.0F).span(),
                                 0x8000);
    ByteWriter temporary;
    bethconv::test::write_group(temporary, real, 9, refs.span());
    ByteWriter real_children;
    bethconv::test::write_group(real_children, real, 6, temporary.span());
    ByteWriter cells;
    bethconv::test::write_record(cells, "CELL", real, cell_payload.span());
    cells.raw(real_children.span());
    ByteWriter sub_block;
    bethconv::test::write_group(sub_block, 0, 5, cells.span());
    ByteWriter block;
    bethconv::test::write_group(block, 0, 4, sub_block.span());

    ByteWriter wrld;
    ByteWriter edid;
    edid.zstring("LargeWorld");
    bethconv::test::write_field(wrld, "EDID", edid);
    const auto rnam = [&](std::int16_t y, std::int16_t x,
                          std::initializer_list<std::uint32_t> list) {
        ByteWriter field;
        field.u16(static_cast<std::uint16_t>(y));
        field.u16(static_cast<std::uint16_t>(x));
        field.u32(static_cast<std::uint32_t>(list.size()));
        for (const auto ref : list) {
            field.u32(ref);
            field.u16(0);
            field.u16(0);
        }
        bethconv::test::write_field(wrld, "RNAM", field);
    };
    rnam(0, 0, {0x0E10, 0x0E11, 0x0E12, 0x0E13, 0x0E14});
    rnam(0, 1, {0x0E13, 0x0E10});
    ByteWriter worlds;
    bethconv::test::write_record(worlds, "WRLD", world, wrld.span());
    ByteWriter world_children;
    bethconv::test::write_group(world_children, world, 1, block.span());
    worlds.raw(world_children.span());
    top_group(file, "WRLD", worlds);
    save(dir, "LargeWorld.esm", file);
}

} // namespace

TEST_CASE("world.fb lists a worldspace's large references, without deleted or disabled ones",
          "[pack][world][large]") {
    const TempDir dir;
    make_large_refs(dir);
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "LargeWorld.esm", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->large_refs == 2);
    CHECK(stats->large_ref_cells == 2);
    CHECK(stats->large_refs_dropped == 3); // disabled, deleted and missing

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());
    const auto worlds = file->worldspaces();
    REQUIRE(worlds.size() == 1);
    const auto& w = worlds[0];
    REQUIRE(w.large_refs.size() == 2);
    CHECK(w.large_refs[0].id() == 0x0000'0E10);
    CHECK(w.large_refs[0].position().x() == 1.0F);
    CHECK(w.large_refs[1].id() == 0x0000'0E13);
    CHECK(w.large_refs[1].scale() == 2.0F);
    CHECK(skydot::formats::has_flag(w.large_refs[1].flags(), pack::wfb::RefFlags::visible_when_distant));
    CHECK_FALSE(skydot::formats::has_flag(w.large_refs[0].flags(), pack::wfb::RefFlags::visible_when_distant));
    CHECK(w.large_refs[1].base() == 0x0000'0800);
    REQUIRE(w.large_cells.size() == 2);
    CHECK((w.large_cells[0].cell_x() == 0 && w.large_cells[0].cell_y() == 0));
    CHECK((w.large_cells[0].first() == 0 && w.large_cells[0].count() == 2));
    CHECK((w.large_cells[1].cell_x() == 1 && w.large_cells[1].cell_y() == 0));
    CHECK((w.large_cells[1].first() == 2 && w.large_cells[1].count() == 2));
    CHECK(w.large_cell_refs == std::vector<std::uint32_t>{0, 1, 0, 1});
}

TEST_CASE("a later plugin's WRLD RNAM lists replace only the cells it lists",
          "[pack][world][large]") {
    const TempDir dir;
    make_large_refs(dir);
    // LargeOver.esp overrides the worldspace, listing only three cells: (0, 0)
    // emptied, (x 1, y 0) now D alone, and (x 2, y 0) new, with A.
    {
        ByteWriter file;
        bethconv::test::write_tes4(file, 0, {"LargeWorld.esm"});
        ByteWriter wrld;
        const auto rnam = [&](std::int16_t y, std::int16_t x,
                              std::initializer_list<std::uint32_t> list) {
            ByteWriter field;
            field.u16(static_cast<std::uint16_t>(y));
            field.u16(static_cast<std::uint16_t>(x));
            field.u32(static_cast<std::uint32_t>(list.size()));
            for (const auto ref : list) {
                field.u32(ref);
                field.u16(0);
                field.u16(0);
            }
            bethconv::test::write_field(wrld, "RNAM", field);
        };
        rnam(0, 0, {});
        rnam(0, 1, {0x0E13});
        rnam(0, 2, {0x0E10});
        ByteWriter worlds;
        bethconv::test::write_record(worlds, "WRLD", 0x0000'0E00, wrld.span());
        top_group(file, "WRLD", worlds);
        save(dir, "LargeOver.esp", file);
    }
    record::PluginList list;
    list.plugins.push_back(record::ListedPlugin{.name = "LargeWorld.esm", .active = true});
    list.plugins.push_back(record::ListedPlugin{.name = "LargeOver.esp", .active = true});
    const auto order = record::LoadOrder::build(
        dir.path(), list,
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}});
    const auto world = record::MergedWorld::build(order);
    const auto out = dir / "world.fb";
    const auto stats = pack::write_world(world, order, out);
    REQUIRE(stats.has_value());
    CHECK(stats->parse_errors == 0);
    CHECK(stats->large_refs == 2);
    CHECK(stats->large_ref_cells == 2);
    CHECK(stats->large_refs_dropped == 0);

    const auto file = pack::WorldFile::open(out);
    REQUIRE(file.has_value());
    const auto worlds = file->worldspaces();
    REQUIRE(worlds.size() == 1);
    const auto& w = worlds[0];
    REQUIRE(w.large_refs.size() == 2);
    REQUIRE(w.large_cells.size() == 2);
    CHECK((w.large_cells[0].cell_x() == 1 && w.large_cells[0].count() == 1));
    CHECK((w.large_cells[1].cell_x() == 2 && w.large_cells[1].count() == 1));
    CHECK(w.large_cell_refs == std::vector<std::uint32_t>{1, 0});
}
