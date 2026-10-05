// SPDX-License-Identifier: GPL-3.0-or-later
//
// `bethconv-testpack`: builds a complete, valid pack without any game data.
// Engine CI runs against it, since it can never have Bethesda files. The output
// contains no Bethesda bytes and can be shared freely.
//
// It is built through `pack::convert`, the production path, so it has a real
// manifest, `vpath.idx`, `records.fb` (with a real merge) and `report.json`,
// and follows every format change. Inputs come from the builders in
// tests/support/, the same ones the converter's tests use.
//
// Records, chosen so every engine query has an answer:
//
//   * a WRLD with a persistent cell and a real exterior cell both at (0, 0);
//   * a second exterior cell at (1, 0), for misses and neighbor streaming;
//   * a CLMT with one WTHR, whose day sky is pure blue and night sky black,
//     and a rainy WTHR (cloud layer, SPGD, thunder) that a REGN over the
//     cell at (1, 0) offers;
//   * LAND on the cell at (0, 0): a slope rising to the east, one LTEX (via a
//     TXST) as the base layer and a second layer on one quadrant;
//   * navmeshes on both exterior cells, linked across their border, and on
//     the interior, with a door triangle;
//   * an interior cell;
//   * REFRs placing STATs with scale and an enable parent;
//   * a LIGH;
//   * a load door pair between the interior and the origin cell (the outside
//     one locked), and a scripted lever with a linked reference;
//   * a start-game-enabled QUST with two stage fragments, an objective and an
//     alias forced to the lever, and a GLOB its fragment sets;
//   * a STAT with `XRGD`, `VMAD` and an invented tag, to check the
//     verbatim-payload rule;
//   * an NPC_ placed in the interior, whose race's behaviour project has an
//     idle, a walk and a run with root motion; it is persistent and has AI
//     packages: home by night, by the scaled cube outside by day.
//
// LOD for the worldspace: settings, terrain LOD at levels 4 and 8, object LOD
// and tree LOD with its list and atlas.
//
// Assets: both NIF encodings (static and clutter collision), a duplicate that dedupes to one asset under two
// paths, a path with a space, a texture with a mip chain, a cubemap, scripts
// for the engine's VM tests (scripts.hpp), and an unconverted file kind for
// `report.json`.
#include "bethconv/archive/archive_set.hpp"
#include "bethconv/pack/asset_store.hpp"
#include "bethconv/mesh/mesh_ir.hpp"
#include "bethconv/pack/convert.hpp"
#include "bethconv/pack/vpath_index.hpp"
#include "bethconv/pack/snapshot.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/record/load_order.hpp"

#include "../../tests/support/dds_builder.hpp"
#include "../../tests/support/esm_builder.hpp"
#include "../../tests/support/hkx_builder.hpp"
#include "../../tests/support/lod_builder.hpp"
#include "../../tests/support/nif_builder.hpp"
#include "../../tests/support/pex_builder.hpp"
#include "scripts.hpp"

#include <CLI/CLI.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace {

using bethconv::test::ByteWriter;
using bethconv::testing::DdsSpec;

namespace fs = std::filesystem;

// ---- the plugin -----------------------------------------------------------

constexpr std::uint32_t k_flag_master = 0x0000'0001;
constexpr std::uint32_t k_flag_persistent = 0x0000'0400;

std::uint32_t tag_value(std::string_view type) {
    ByteWriter w;
    w.tag(type);
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        raw |= static_cast<std::uint32_t>(static_cast<unsigned char>(w.bytes()[i])) << (i * 8);
    }
    return raw;
}

void edid(ByteWriter& payload, std::string_view name) {
    ByteWriter field;
    field.zstring(name);
    bethconv::test::write_field(payload, "EDID", field);
}

/// OBND, six int16 (used for culling).
void obnd(ByteWriter& payload, std::int16_t half) {
    ByteWriter field;
    for (const std::int16_t v : {static_cast<std::int16_t>(-half), static_cast<std::int16_t>(-half),
                                 static_cast<std::int16_t>(-half), half, half, half}) {
        field.u16(static_cast<std::uint16_t>(v));
    }
    bethconv::test::write_field(payload, "OBND", field);
}

void modl(ByteWriter& payload, std::string_view path) {
    ByteWriter field;
    field.zstring(path);
    bethconv::test::write_field(payload, "MODL", field);
}

/// A STAT: bounds, a model path to a mesh in this pack, and the 8-byte DNAM
/// used by all 9,720 Skyrim.esm statics.
ByteWriter static_record(std::string_view name, std::string_view model) {
    ByteWriter payload;
    edid(payload, name);
    obnd(payload, 32);
    modl(payload, model);
    ByteWriter dnam;
    dnam.f32(0.0F);  // terrain-conform angle
    dnam.u32(0);     // MATO FormID: none
    bethconv::test::write_field(payload, "DNAM", dnam);
    return payload;
}

/// A STAT with `XRGD`, `VMAD` (real fields nothing decodes) and `ZZZZ`
/// (invented). v1 promises all three survive byte for byte in the payload.
ByteWriter verbatim_record() {
    ByteWriter payload = static_record("TestpackVerbatimStatic", "testpack/cube_se.nif");

    ByteWriter xrgd;
    for (std::uint32_t i = 0; i < 6; ++i) {
        xrgd.f32(static_cast<float>(i) * 1.5F);
    }
    bethconv::test::write_field(payload, "XRGD", xrgd);

    ByteWriter vmad; // version 5, object format 2, no scripts
    vmad.u16(5);
    vmad.u16(2);
    vmad.u16(0);
    bethconv::test::write_field(payload, "VMAD", vmad);

    ByteWriter zzzz;
    zzzz.zstring("a field no definition anywhere knows");
    bethconv::test::write_field(payload, "ZZZZ", zzzz);
    return payload;
}

ByteWriter light_record(std::string_view name) {
    ByteWriter payload;
    edid(payload, name);
    obnd(payload, 16);
    // DATA, 48 bytes, field order per UESP (as in forms.hpp).
    ByteWriter data;
    data.u32(static_cast<std::uint32_t>(-1)); // time
    data.u32(512);                            // radius
    data.u32(0x00A0'C8FFu);                   // color, RGBA
    data.u32(0x0000'0001u);                   // flags: dynamic
    data.f32(1.0F);                           // falloff exponent
    data.f32(90.0F);                          // fov
    data.f32(0.0F);                           // near clip
    data.f32(0.0F);                           // flicker period
    data.f32(0.0F);                           // flicker intensity amplitude
    data.f32(0.0F);                           // flicker movement amplitude
    data.u32(20);                             // value
    data.f32(1.0F);                           // weight
    bethconv::test::write_field(payload, "DATA", data);
    ByteWriter fnam;
    fnam.f32(1.0F);
    bethconv::test::write_field(payload, "FNAM", fnam);
    return payload;
}

/// VMAD with one script and one object property, `Target`.
ByteWriter vmad_field(std::string_view script, std::uint32_t target) {
    ByteWriter vmad;
    const auto wstring = [&](std::string_view text) {
        vmad.u16(static_cast<std::uint16_t>(text.size()));
        vmad.raw(text);
    };
    vmad.u16(5); // version
    vmad.u16(2); // object format: unused, alias, FormID
    vmad.u16(1);
    wstring(script);
    vmad.u8(0);
    vmad.u16(1);
    wstring("Target");
    vmad.u8(1); // object
    vmad.u8(1); // edited
    vmad.u16(0);
    vmad.u16(0xFFFF);
    vmad.u32(target);
    return vmad;
}

/// A DOOR with a model and FNAM (no flags).
ByteWriter door_record(std::string_view name, std::string_view model) {
    ByteWriter payload;
    edid(payload, name);
    obnd(payload, 32);
    modl(payload, model);
    ByteWriter fnam;
    fnam.u8(0);
    bethconv::test::write_field(payload, "FNAM", fnam);
    return payload;
}

/// An ACTI with a model and a script.
ByteWriter activator_record(std::string_view name, std::string_view model,
                            std::string_view script) {
    ByteWriter payload;
    edid(payload, name);
    bethconv::test::write_field(payload, "VMAD", vmad_field(script, 0));
    obnd(payload, 16);
    modl(payload, model);
    return payload;
}

struct Placement {
    std::uint32_t form{};
    std::uint32_t base{};
    float x{};
    float y{};
    float z{};
    float scale{1.0F};
    std::uint32_t enable_parent{}; ///< 0 for none.
    float rz{}; ///< Rotation about Z, radians.
    /// Further fields, written after DATA.
    std::vector<std::byte> extra{};
    std::uint32_t record_flags{}; ///< E.g. persistent (0x400).
};

/// XTEL: the destination door and where the player arrives.
std::vector<std::byte> teleport(std::uint32_t door, float x, float y, float z, float angle) {
    ByteWriter xtel;
    xtel.u32(door);
    for (const float v : {x, y, z, 0.0F, 0.0F, angle}) {
        xtel.f32(v);
    }
    xtel.u32(0);
    ByteWriter out;
    bethconv::test::write_field(out, "XTEL", xtel);
    return out.bytes();
}

/// XLOC: level and key; 20 bytes as in Skyrim.esm.
std::vector<std::byte> lock(std::uint8_t level) {
    ByteWriter xloc;
    xloc.u8(level);
    xloc.u8(0);
    xloc.u16(0);
    xloc.u32(0);
    xloc.u32(0);
    xloc.u32(0);
    xloc.u32(0);
    ByteWriter out;
    bethconv::test::write_field(out, "XLOC", xloc);
    return out.bytes();
}

/// A REFR: NAME is the base record, DATA position and rotation (radians), XSCL
/// the scale.
ByteWriter reference_record(const Placement& p) {
    ByteWriter payload;
    ByteWriter name;
    name.u32(p.base);
    bethconv::test::write_field(payload, "NAME", name);

    if (p.enable_parent != 0) {
        ByteWriter xesp;
        xesp.u32(p.enable_parent);
        xesp.u32(0); // flags: not "set enable state to opposite of parent"
        bethconv::test::write_field(payload, "XESP", xesp);
    }

    if (p.scale != 1.0F) {
        ByteWriter xscl;
        xscl.f32(p.scale);
        bethconv::test::write_field(payload, "XSCL", xscl);
    }

    ByteWriter data;
    data.f32(p.x);
    data.f32(p.y);
    data.f32(p.z);
    data.f32(0.0F);
    data.f32(0.0F);
    data.f32(p.rz);
    bethconv::test::write_field(payload, "DATA", data);
    payload.raw(p.extra);
    return payload;
}

ByteWriter cell_record(std::string_view name, bool exterior, std::int32_t x, std::int32_t y) {
    ByteWriter payload;
    edid(payload, name);
    ByteWriter data;
    data.u16(exterior ? 0x0000 : 0x0001); // DATA flags: interior
    bethconv::test::write_field(payload, "DATA", data);
    if (!exterior) {
        // XCLL, 92 bytes (layout per UESP): warm ambient, dim directional,
        // fog 500-4000.
        ByteWriter xcll;
        xcll.u32(0x0030'3848u); // ambient RGBA
        xcll.u32(0x0040'5060u); // directional
        xcll.u32(0x0010'1010u); // fog near color
        xcll.f32(500.0F);       // fog near
        xcll.f32(4000.0F);      // fog far
        xcll.u32(45);           // directional rotation XY
        xcll.u32(30);           // directional rotation Z
        xcll.f32(1.0F);         // directional fade
        xcll.f32(5000.0F);      // fog clip distance
        xcll.f32(1.0F);         // fog power
        for (int i = 0; i < 8; ++i) {
            xcll.u32(0); // directional ambient (6), specular, fresnel power
        }
        xcll.u32(0x0020'2020u); // fog far color
        xcll.f32(1.0F);         // fog max
        xcll.f32(2000.0F);      // light fade begin
        xcll.f32(4000.0F);      // light fade end
        xcll.u32(0);            // inherit flags
        bethconv::test::write_field(payload, "XCLL", xcll);
    }
    if (exterior) {
        ByteWriter xclc;
        xclc.u32(static_cast<std::uint32_t>(x));
        xclc.u32(static_cast<std::uint32_t>(y));
        xclc.u32(0); // land/water flags
        bethconv::test::write_field(payload, "XCLC", xclc);
    }
    ByteWriter xclw;
    xclw.f32(exterior ? -14000.0F : 0.0F); // water height
    bethconv::test::write_field(payload, "XCLW", xclw);
    return payload;
}

/// `CELL > GRUP cell_children > GRUP cell_temporary_children`, as in a real
/// worldspace; the merge's parent tracking depends on it.
void write_cell(ByteWriter& out, std::uint32_t form, const ByteWriter& payload,
                std::uint32_t cell_flags, const std::vector<Placement>& refrs,
                const ByteWriter* land = nullptr, std::uint32_t land_form = 0,
                const std::vector<std::pair<std::uint32_t, bethconv::test::NavSpec>>& navmeshes = {},
                const std::vector<Placement>& actors = {}) {
    bethconv::test::write_record(out, "CELL", form, payload.span(), cell_flags);

    ByteWriter children;
    if (land != nullptr) {
        bethconv::test::write_record(children, "LAND", land_form, land->span());
    }
    for (const auto& [nav_form, nav] : navmeshes) {
        ByteWriter navm;
        bethconv::test::write_field(navm, "NVNM", bethconv::test::nvnm(nav));
        bethconv::test::write_record(children, "NAVM", nav_form, navm.span());
    }
    for (const auto& p : refrs) {
        bethconv::test::write_record(children, "REFR", p.form, reference_record(p).span());
    }
    for (const auto& p : actors) {
        bethconv::test::write_record(children, "ACHR", p.form, reference_record(p).span(), p.record_flags);
    }
    ByteWriter temporary;
    bethconv::test::write_group(temporary, form, 9, children.span());
    ByteWriter cell_children;
    bethconv::test::write_group(cell_children, form, 6, temporary.span());
    out.raw(cell_children.span());
}

/// Navmeshes. The origin cell's follows its land (z = x / 16) and links
/// across x = 4096 to the east cell's, which sits 30 units higher there, as
/// real portals often do. The interior's has a door triangle at its door.
bethconv::test::NavSpec origin_navmesh() {
    bethconv::test::NavSpec nav;
    nav.world = 0x0000'0001;
    nav.vertices = {{0, 0, 0}, {4096, 0, 256}, {4096, 4096, 256}, {0, 4096, 0}};
    nav.triangles = {
        {.vertices = {0, 1, 2}, .edges = {-1, 0, 1}, .flags = 0x0002},
        {.vertices = {0, 2, 3}, .edges = {0, -1, -1}, .flags = 0},
    };
    nav.links = {{.type = 0, .navmesh = 0x0000'0223, .triangle = 1}};
    return nav;
}

bethconv::test::NavSpec east_navmesh() {
    bethconv::test::NavSpec nav;
    nav.world = 0x0000'0001;
    nav.grid_x = 1;
    nav.vertices = {{4096, 0, 286}, {8192, 0, 286}, {8192, 4096, 286}, {4096, 4096, 286}};
    nav.triangles = {
        {.vertices = {0, 1, 2}, .edges = {-1, -1, 1}, .flags = 0},
        {.vertices = {0, 2, 3}, .edges = {0, -1, 0}, .flags = 0x0004},
    };
    nav.links = {{.type = 0, .navmesh = 0x0000'0216, .triangle = 0}};
    return nav;
}

bethconv::test::NavSpec interior_navmesh() {
    bethconv::test::NavSpec nav;
    nav.cell = 0x0000'0300;
    nav.vertices = {{-256, -512, 0}, {256, -512, 0}, {256, 256, 0}, {-256, 256, 0}};
    nav.triangles = {
        {.vertices = {0, 1, 2}, .edges = {-1, -1, 1}, .flags = 0},
        {.vertices = {0, 2, 3}, .edges = {0, -1, -1}, .flags = 0x0400},
    };
    nav.doors = {{.triangle = 1, .door = 0x0000'0303}};
    return nav;
}

/// LAND: heights rising 8 units per vertex to the east, the testpack LTEX as
/// every quadrant's base, and the default texture (0) over half of quadrant 0.
ByteWriter land_record() {
    ByteWriter payload;
    ByteWriter data;
    data.u32(0x1);
    bethconv::test::write_field(payload, "DATA", data);
    ByteWriter vhgt;
    vhgt.f32(0.0F);
    for (int y = 0; y < 33; ++y) {
        for (int x = 0; x < 33; ++x) {
            vhgt.u8(x == 0 ? 0 : 1);
        }
    }
    vhgt.u8(0);
    vhgt.u8(0);
    vhgt.u8(0);
    bethconv::test::write_field(payload, "VHGT", vhgt);
    for (std::uint8_t quadrant = 0; quadrant < 4; ++quadrant) {
        ByteWriter btxt;
        btxt.u32(0x0000'0121);
        btxt.u8(quadrant);
        btxt.u8(0);
        btxt.u16(0xFFFF);
        bethconv::test::write_field(payload, "BTXT", btxt);
    }
    ByteWriter atxt;
    atxt.u32(0);
    atxt.u8(0);
    atxt.u8(0);
    atxt.u16(0);
    bethconv::test::write_field(payload, "ATXT", atxt);
    ByteWriter vtxt;
    for (std::uint16_t point = 0; point < 17 * 8; ++point) {
        vtxt.u16(point);
        vtxt.u16(0);
        vtxt.f32(1.0F);
    }
    bethconv::test::write_field(payload, "VTXT", vtxt);
    return payload;
}

/// TestpackQuest: start-game enabled; stage 10 (start-up) shows objective 10,
/// stage 20 completes the quest; alias 0 is the lever's reference, with
/// TestpackAliasScript. Scripts are in scripts.hpp.
ByteWriter quest_record() {
    constexpr std::uint32_t self = 0x0000'0150;
    ByteWriter payload;
    edid(payload, "TestpackQuest");
    ByteWriter vmad;
    const auto wstring = [&](std::string_view text) {
        vmad.u16(static_cast<std::uint16_t>(text.size()));
        vmad.raw(text);
    };
    const auto object = [&](std::uint32_t form, std::int16_t alias) {
        vmad.u16(0);
        vmad.u16(static_cast<std::uint16_t>(alias));
        vmad.u32(form);
    };
    const auto property = [&](std::string_view name) {
        wstring(name);
        vmad.u8(1); // object
        vmad.u8(1); // edited
    };
    vmad.u16(5);
    vmad.u16(2);
    vmad.u16(2);
    wstring("TestpackQuestScript");
    vmad.u8(0);
    vmad.u16(2);
    property("Counter");
    object(0x0000'0140, -1);
    property("Lever");
    object(self, 0);
    wstring("QF_TestpackQuest_00000150");
    vmad.u8(0);
    vmad.u16(1);
    property("Counter");
    object(0x0000'0140, -1);
    // Fragments.
    vmad.u8(2);
    vmad.u16(2);
    wstring("QF_TestpackQuest_00000150");
    for (const auto& [stage, fn] : {std::pair{10, "Fragment_0"}, std::pair{20, "Fragment_1"}}) {
        vmad.u16(static_cast<std::uint16_t>(stage));
        vmad.u16(0);
        vmad.u32(0);
        vmad.u8(1);
        wstring("QF_TestpackQuest_00000150");
        wstring(fn);
    }
    // Alias 0's script.
    vmad.u16(1);
    object(self, 0);
    vmad.u16(5);
    vmad.u16(2);
    vmad.u16(1);
    wstring("TestpackAliasScript");
    vmad.u8(0);
    vmad.u16(0);
    bethconv::test::write_field(payload, "VMAD", vmad);

    ByteWriter dnam;
    dnam.u16(0x0001); // start game enabled
    dnam.u8(50);
    dnam.u8(0);
    dnam.u32(0);
    dnam.u32(0);
    bethconv::test::write_field(payload, "DNAM", dnam);
    bethconv::test::write_field(payload, "NEXT", ByteWriter{});
    for (const auto& [stage, flags, log_flags, text] :
         {std::tuple{10, 0x02, 0x00, "The lever waits."},
          std::tuple{20, 0x00, 0x01, "The lever was pulled."}}) {
        ByteWriter indx;
        indx.u16(static_cast<std::uint16_t>(stage));
        indx.u8(static_cast<std::uint8_t>(flags));
        indx.u8(0);
        bethconv::test::write_field(payload, "INDX", indx);
        ByteWriter qsdt;
        qsdt.u8(static_cast<std::uint8_t>(log_flags));
        bethconv::test::write_field(payload, "QSDT", qsdt);
        ByteWriter cnam;
        cnam.zstring(text);
        bethconv::test::write_field(payload, "CNAM", cnam);
    }
    ByteWriter qobj;
    qobj.u16(10);
    bethconv::test::write_field(payload, "QOBJ", qobj);
    ByteWriter objective_flags;
    objective_flags.u32(0);
    bethconv::test::write_field(payload, "FNAM", objective_flags);
    ByteWriter nnam;
    nnam.zstring("Pull the lever");
    bethconv::test::write_field(payload, "NNAM", nnam);
    ByteWriter qsta;
    qsta.u32(0);
    qsta.u32(0);
    bethconv::test::write_field(payload, "QSTA", qsta);
    ByteWriter anam;
    anam.u32(1);
    bethconv::test::write_field(payload, "ANAM", anam);
    ByteWriter alst;
    alst.u32(0);
    bethconv::test::write_field(payload, "ALST", alst);
    ByteWriter alid;
    alid.zstring("Lever");
    bethconv::test::write_field(payload, "ALID", alid);
    ByteWriter alias_flags;
    alias_flags.u32(0);
    bethconv::test::write_field(payload, "FNAM", alias_flags);
    ByteWriter alfr;
    alfr.u32(0x0000'0304);
    bethconv::test::write_field(payload, "ALFR", alfr);
    bethconv::test::write_field(payload, "ALED", ByteWriter{});
    return payload;
}

std::vector<std::byte> build_plugin() {
    ByteWriter file;
    bethconv::test::write_tes4(file, k_flag_master, {});

    // ---- STAT and LIGH, referenced by the REFRs ----
    ByteWriter stats;
    bethconv::test::write_record(stats, "STAT", 0x0000'0100,
                                 static_record("TestpackCube", "testpack/cube_se.nif").span());
    bethconv::test::write_record(
        stats, "STAT", 0x0000'0101,
        static_record("TestpackCubeSpaced", "testpack/cube with space.nif").span());
    bethconv::test::write_record(stats, "STAT", 0x0000'0102, verbatim_record().span());
    bethconv::test::write_group(file, tag_value("STAT"), 0, stats.span());

    // ---- a door and a lever ----
    ByteWriter doors;
    bethconv::test::write_record(doors, "DOOR", 0x0000'0103,
                                 door_record("TestpackDoor", "testpack/cube_se.nif").span());
    bethconv::test::write_group(file, tag_value("DOOR"), 0, doors.span());
    ByteWriter activators;
    bethconv::test::write_record(
        activators, "ACTI", 0x0000'0104,
        activator_record("TestpackLever", "testpack/cube_se.nif", "TestpackLeverScript").span());
    {
        // A trigger: no model, a script, a primitive on its reference.
        ByteWriter trigger;
        edid(trigger, "TestpackTrigger");
        ByteWriter vmad; // version 5, format 2, one script without properties
        vmad.u16(5);
        vmad.u16(2);
        vmad.u16(1);
        vmad.u16(21);
        vmad.raw("TestpackTriggerScript");
        vmad.u8(0);
        vmad.u16(0);
        bethconv::test::write_field(trigger, "VMAD", vmad);
        obnd(trigger, 16);
        bethconv::test::write_record(activators, "ACTI", 0x0000'0105, trigger.span());
    }
    bethconv::test::write_group(file, tag_value("ACTI"), 0, activators.span());

    // ---- a global and a quest whose alias is the lever ----
    ByteWriter globs;
    {
        ByteWriter glob;
        edid(glob, "TestpackCounter");
        ByteWriter kind;
        kind.u8('s');
        bethconv::test::write_field(glob, "FNAM", kind);
        ByteWriter value;
        value.f32(0.0F);
        bethconv::test::write_field(glob, "FLTV", value);
        bethconv::test::write_record(globs, "GLOB", 0x0000'0140, glob.span());
    }
    bethconv::test::write_group(file, tag_value("GLOB"), 0, globs.span());
    ByteWriter quests;
    bethconv::test::write_record(quests, "QUST", 0x0000'0150, quest_record().span());
    bethconv::test::write_group(file, tag_value("QUST"), 0, quests.span());

    // ---- an actor: race, skin armor and its addon, NPC ----
    // The race's skeleton and behaviour name files under meshes/testpack/actor/
    // (see actor_skeleton and actor_clip); its skin is one addon, the skinned
    // body.
    {
        const auto text = [](ByteWriter& payload, std::string_view tag, std::string_view value) {
            ByteWriter field;
            field.zstring(value);
            bethconv::test::write_field(payload, tag, field);
        };
        const auto u32 = [](ByteWriter& payload, std::string_view tag, std::uint32_t value) {
            ByteWriter field;
            field.u32(value);
            bethconv::test::write_field(payload, tag, field);
        };
        ByteWriter race;
        edid(race, "TestpackRace");
        u32(race, "WNAM", 0x0000'0162);
        ByteWriter data;
        for (int i = 0; i < 16; ++i) {
            data.u8(0);
        }
        for (const float f : {1.0F, 1.0F, 1.0F, 1.0F}) {
            data.f32(f);
        }
        data.u32(1);
        for (int i = 36; i < 164; ++i) {
            data.u8(0);
        }
        bethconv::test::write_field(race, "DATA", data);
        bethconv::test::write_field(race, "MNAM", ByteWriter{});
        text(race, "ANAM", "TestPack\\Actor\\skeleton.nif");
        bethconv::test::write_field(race, "NAM3", ByteWriter{});
        bethconv::test::write_field(race, "MNAM", ByteWriter{});
        text(race, "MODL", "TestPack\\Actor\\Behaviour.hkx");
        ByteWriter races;
        bethconv::test::write_record(races, "RACE", 0x0000'0160, race.span());
        bethconv::test::write_group(file, tag_value("RACE"), 0, races.span());

        ByteWriter bod2;
        bod2.u32(0x4); // body
        bod2.u32(2);
        ByteWriter addon;
        edid(addon, "TestpackSkinAddon");
        bethconv::test::write_field(addon, "BOD2", bod2);
        u32(addon, "RNAM", 0x0000'0160);
        text(addon, "MOD2", "TestPack\\Actor\\body.nif");
        ByteWriter addons;
        bethconv::test::write_record(addons, "ARMA", 0x0000'0161, addon.span());
        bethconv::test::write_group(file, tag_value("ARMA"), 0, addons.span());

        ByteWriter skin;
        edid(skin, "TestpackSkin");
        bethconv::test::write_field(skin, "BOD2", bod2);
        u32(skin, "RNAM", 0x0000'0160);
        u32(skin, "MODL", 0x0000'0161);
        ByteWriter armors;
        bethconv::test::write_record(armors, "ARMO", 0x0000'0162, skin.span());
        bethconv::test::write_group(file, tag_value("ARMO"), 0, armors.span());

        ByteWriter npc;
        edid(npc, "TestpackNpc");
        ByteWriter acbs;
        acbs.u32(0); // male
        for (int i = 0; i < 10; ++i) {
            acbs.u16(0);
        }
        bethconv::test::write_field(npc, "ACBS", acbs);
        u32(npc, "RNAM", 0x0000'0160);
        u32(npc, "PKID", 0x0000'0172); // work by day
        u32(npc, "PKID", 0x0000'0171); // home by night
        ByteWriter npcs;
        bethconv::test::write_record(npcs, "NPC_", 0x0000'0164, npc.span());
        bethconv::test::write_group(file, tag_value("NPC_"), 0, npcs.span());

        // AI packages: a template like Skyrim.esm's Sandbox (travel and
        // unlock the doors there if "Unlock On Arrival?", then sandbox), a
        // home package near the editor location from 20:00 for 12 hours,
        // and a work package by the scaled cube outside from 08:00 that
        // unlocks doors there.
        const auto bytes = [](std::initializer_list<int> list) {
            ByteWriter w;
            for (const int b : list) {
                w.u8(static_cast<std::uint8_t>(b));
            }
            return w;
        };
        const auto pack_header = [&](ByteWriter& p, std::string_view name, std::uint8_t type,
                                     std::int8_t hour, std::uint32_t minutes,
                                     std::uint32_t templ, std::uint32_t inputs) {
            edid(p, name);
            ByteWriter pkdt;
            pkdt.u32(0);
            pkdt.u8(type);
            pkdt.u8(0);
            pkdt.u8(0);
            pkdt.u8(0);
            pkdt.u16(0);
            pkdt.u16(0);
            bethconv::test::write_field(p, "PKDT", pkdt);
            ByteWriter psdt = bytes({0xFF, 0xFF, 0, static_cast<std::uint8_t>(hour), 0xFF, 0, 0, 0});
            psdt.u32(minutes);
            bethconv::test::write_field(p, "PSDT", psdt);
            ByteWriter pkcu;
            pkcu.u32(inputs);
            pkcu.u32(templ);
            pkcu.u32(1);
            bethconv::test::write_field(p, "PKCU", pkcu);
        };
        const auto location = [&](ByteWriter& p, std::int32_t type, std::uint32_t value, std::int32_t radius) {
            text(p, "ANAM", "Location");
            ByteWriter pldt;
            pldt.u32(static_cast<std::uint32_t>(type));
            pldt.u32(value);
            pldt.u32(static_cast<std::uint32_t>(radius));
            bethconv::test::write_field(p, "PLDT", pldt);
        };
        const auto flag = [&](ByteWriter& p, bool on) {
            text(p, "ANAM", "Bool");
            bethconv::test::write_field(p, "CNAM", bytes({on ? 1 : 0}));
        };
        const auto keys = [&](ByteWriter& p, std::initializer_list<int> list) {
            for (const int k : list) {
                bethconv::test::write_field(p, "UNAM", bytes({k}));
            }
            bethconv::test::write_field(p, "XNAM", bytes({0x0F}));
        };
        const auto events = [&](ByteWriter& p) {
            for (const char* marker : {"POBA", "POEA", "POCA"}) {
                bethconv::test::write_field(p, marker, ByteWriter{});
                u32(p, "INAM", 0);
            }
        };
        // GetNumericPackageData(key 0x0E) == 1.
        const auto unlock_on_arrival = [&](ByteWriter& p) {
            u32(p, "CITC", 1);
            ByteWriter ctda;
            ctda.u32(0);
            ctda.f32(1.0F);
            ctda.u16(612);
            ctda.u16(0);
            ctda.u32(0x0E);
            ctda.u32(0);
            ctda.u32(0);
            ctda.u32(0);
            ctda.u32(0xFFFF'FFFF);
            bethconv::test::write_field(p, "CTDA", ctda);
        };
        const auto procedure = [&](ByteWriter& p, std::string_view name, bool conditional) {
            text(p, "ANAM", "Procedure");
            if (conditional) {
                unlock_on_arrival(p);
            } else {
                u32(p, "CITC", 0);
            }
            text(p, "PNAM", name);
            u32(p, "FNAM", 0);
            bethconv::test::write_field(p, "PKC2", bytes({0}));
        };
        ByteWriter packs;
        {
            ByteWriter p;
            pack_header(p, "TestpackSandboxTemplate", 19, -1, 0, 0, 2);
            location(p, 3, 0, 256);
            flag(p, false);
            keys(p, {0, 0x0E});
            text(p, "ANAM", "Sequence");
            u32(p, "CITC", 0);
            ByteWriter prcb;
            prcb.u32(3);
            prcb.u32(0);
            bethconv::test::write_field(p, "PRCB", prcb);
            procedure(p, "Travel", true);
            procedure(p, "UnlockDoors", true);
            procedure(p, "Sandbox", false);
            bethconv::test::write_field(p, "UNAM", bytes({0}));
            text(p, "BNAM", "Location");
            u32(p, "PNAM", 1);
            bethconv::test::write_field(p, "UNAM", bytes({0x0E}));
            text(p, "BNAM", "Unlock On Arrival?");
            u32(p, "PNAM", 1);
            events(p);
            bethconv::test::write_record(packs, "PACK", 0x0000'0170, p.span());
        }
        {
            ByteWriter p;
            pack_header(p, "TestpackHome20x12", 18, 20, 720, 0x0000'0170, 2);
            location(p, 3, 0, 256);
            flag(p, false);
            keys(p, {0, 0x0E});
            events(p);
            bethconv::test::write_record(packs, "PACK", 0x0000'0171, p.span());
        }
        {
            ByteWriter p;
            pack_header(p, "TestpackWork8x12", 18, 8, 720, 0x0000'0170, 2);
            location(p, 0, 0x0000'0212, 300); // near the scaled cube outside
            flag(p, true);
            keys(p, {0, 0x0E});
            events(p);
            bethconv::test::write_record(packs, "PACK", 0x0000'0172, p.span());
        }
        bethconv::test::write_group(file, tag_value("PACK"), 0, packs.span());
    }

    // ---- WTHR and CLMT, for the sky ----
    ByteWriter wthr;
    edid(wthr, "TestpackClear");
    ByteWriter nam0;
    for (int colour = 0; colour < 17; ++colour) {
        for (int time = 0; time < 4; ++time) {
            // Sky upper (colour 0): blue by day, black at night; others grey.
            nam0.u32(colour == 0 ? (time == 1 ? 0x00FF0000u : 0u) : 0x00808080u);
        }
    }
    bethconv::test::write_field(wthr, "NAM0", nam0);
    ByteWriter fnam;
    for (const float f : {1000.0F, 50000.0F, 500.0F, 20000.0F, 1.0F, 1.0F, 0.5F, 0.5F}) {
        fnam.f32(f);
    }
    bethconv::test::write_field(wthr, "FNAM", fnam);
    ByteWriter wthrs;
    bethconv::test::write_record(wthrs, "WTHR", 0x0000'0130, wthr.span());
    // TestpackRain: one cloud layer, rain from an SPGD, thunder; only the
    // region over the east cell offers it.
    ByteWriter rain;
    edid(rain, "TestpackRain");
    bethconv::test::write_field(rain, "NAM0", nam0);
    bethconv::test::write_field(rain, "FNAM", fnam);
    ByteWriter cloud;
    cloud.zstring("testpack\\sky.dds");
    bethconv::test::write_field(rain, "00TX", cloud);
    ByteWriter rain_data;
    for (const int b : {128, 0, 0, 255, 0, 0, 0, 255, 0, 255, 0, 4, 255, 255, 255, 0, 0, 0, 0}) {
        rain_data.u8(static_cast<std::uint8_t>(b)); // rainy, thunder at once and often
    }
    bethconv::test::write_field(rain, "DATA", rain_data);
    ByteWriter mnam;
    mnam.u32(0x0000'0133);
    bethconv::test::write_field(rain, "MNAM", mnam);
    bethconv::test::write_record(wthrs, "WTHR", 0x0000'0132, rain.span());
    bethconv::test::write_group(file, tag_value("WTHR"), 0, wthrs.span());
    ByteWriter spgd;
    edid(spgd, "TestpackDrops");
    ByteWriter spgd_data;
    for (const float v : {675.0F, 0.0F, 0.35F, 2.0F, 0.0F, 0.0F, 0.0F}) {
        spgd_data.f32(v);
    }
    for (const std::uint32_t v : {1U, 1U, 0U, 700U}) {
        spgd_data.u32(v);
    }
    spgd_data.f32(1.0F);
    bethconv::test::write_field(spgd, "DATA", spgd_data);
    ByteWriter icon;
    icon.zstring("testpack\\sky.dds");
    bethconv::test::write_field(spgd, "ICON", icon);
    ByteWriter spgds;
    bethconv::test::write_record(spgds, "SPGD", 0x0000'0133, spgd.span());
    bethconv::test::write_group(file, tag_value("SPGD"), 0, spgds.span());
    ByteWriter regn;
    edid(regn, "TestpackStorms");
    ByteWriter regn_world;
    regn_world.u32(0x0000'0001);
    bethconv::test::write_field(regn, "WNAM", regn_world);
    ByteWriter rpli;
    rpli.u32(0);
    bethconv::test::write_field(regn, "RPLI", rpli);
    ByteWriter rpld;
    for (const float v : {4096.0F, 0.0F, 8192.0F, 0.0F, 8192.0F, 4096.0F, 4096.0F, 4096.0F}) {
        rpld.f32(v);
    }
    bethconv::test::write_field(regn, "RPLD", rpld);
    ByteWriter rdat;
    rdat.u32(3); // weather
    rdat.u8(0);
    rdat.u8(50);
    rdat.u16(0);
    bethconv::test::write_field(regn, "RDAT", rdat);
    ByteWriter rdwt;
    rdwt.u32(0x0000'0132);
    rdwt.u32(100);
    rdwt.u32(0);
    bethconv::test::write_field(regn, "RDWT", rdwt);
    ByteWriter regns;
    bethconv::test::write_record(regns, "REGN", 0x0000'0134, regn.span());
    bethconv::test::write_group(file, tag_value("REGN"), 0, regns.span());
    ByteWriter clmt;
    edid(clmt, "TestpackClimate");
    ByteWriter wlst;
    wlst.u32(0x0000'0130);
    wlst.u32(100);
    wlst.u32(0);
    bethconv::test::write_field(clmt, "WLST", wlst);
    ByteWriter sun_times;
    for (const int v : {36, 48, 108, 120, 0, 0}) { // 6:00, 8:00, 18:00, 20:00
        sun_times.u8(static_cast<std::uint8_t>(v));
    }
    bethconv::test::write_field(clmt, "TNAM", sun_times);
    ByteWriter clmts;
    bethconv::test::write_record(clmts, "CLMT", 0x0000'0131, clmt.span());
    bethconv::test::write_group(file, tag_value("CLMT"), 0, clmts.span());

    // ---- TXST and LTEX, for the terrain ----
    ByteWriter txst;
    ByteWriter tx00;
    tx00.zstring("testpack\\cube.dds");
    bethconv::test::write_field(txst, "TX00", tx00);
    ByteWriter tx01;
    tx01.zstring("testpack\\cube_n.dds");
    bethconv::test::write_field(txst, "TX01", tx01);
    ByteWriter txsts;
    bethconv::test::write_record(txsts, "TXST", 0x0000'0120, txst.span());
    bethconv::test::write_group(file, tag_value("TXST"), 0, txsts.span());
    ByteWriter ltex;
    ByteWriter ltex_edid;
    ltex_edid.zstring("TestpackGround");
    bethconv::test::write_field(ltex, "EDID", ltex_edid);
    ByteWriter tnam;
    tnam.u32(0x0000'0120);
    bethconv::test::write_field(ltex, "TNAM", tnam);
    ByteWriter ltexs;
    bethconv::test::write_record(ltexs, "LTEX", 0x0000'0121, ltex.span());
    bethconv::test::write_group(file, tag_value("LTEX"), 0, ltexs.span());

    ByteWriter lights;
    bethconv::test::write_record(lights, "LIGH", 0x0000'0110,
                                 light_record("TestpackTorch").span());
    bethconv::test::write_group(file, tag_value("LIGH"), 0, lights.span());

    // ---- the worldspace ----
    ByteWriter cells;
    const ByteWriter land = land_record();
    // The persistent cell and the real cell, both at (0, 0).
    write_cell(cells, 0x0000'0200, cell_record("TestpackPersistent", true, 0, 0),
               k_flag_persistent,
               {{.form = 0x0000'0201, .base = 0x0000'0110, .x = 0, .y = 0, .z = 200}});
    write_cell(cells, 0x0000'0210, cell_record("TestpackOrigin", true, 0, 0), 0,
               {
                   {.form = 0x0000'0211, .base = 0x0000'0100, .x = 12, .y = -34, .z = 56},
                   {.form = 0x0000'0212,
                    .base = 0x0000'0101,
                    .x = 300,
                    .y = 0,
                    .z = 0,
                    .scale = 2.5F},
                   // Locked; leads to the interior's door.
                   {.form = 0x0000'0213,
                    .base = 0x0000'0103,
                    .x = 1000,
                    .y = 1000,
                    .z = 0,
                    .extra = [] {
                        auto bytes = teleport(0x0000'0303, 0, 150, 0, 0.0F);
                        const auto locked = lock(25);
                        bytes.insert(bytes.end(), locked.begin(), locked.end());
                        return bytes;
                    }()},
               },
               &land, 0x0000'0215, {{0x0000'0216, origin_navmesh()}});
    write_cell(cells, 0x0000'0220, cell_record("TestpackEast", true, 1, 0), 0,
               {
                   {.form = 0x0000'0221, .base = 0x0000'0100, .x = 4096, .y = 0, .z = 0},
                   // Enabled by the reference in the cell to the west: a
                   // cross-cell dependency.
                   {.form = 0x0000'0222,
                    .base = 0x0000'0102,
                    .x = 4400,
                    .y = 128,
                    .z = 0,
                    .enable_parent = 0x0000'0211},
               },
               nullptr, 0, {{0x0000'0223, east_navmesh()}});

    ByteWriter sub_block;
    bethconv::test::write_group(sub_block, 0, 5, cells.span());
    ByteWriter block;
    bethconv::test::write_group(block, 0, 4, sub_block.span());

    ByteWriter world_and_children;
    ByteWriter world_payload;
    edid(world_payload, "TestpackWorld");
    ByteWriter cnam;
    cnam.u32(0x0000'0131);
    bethconv::test::write_field(world_payload, "CNAM", cnam);
    bethconv::test::write_record(world_and_children, "WRLD", 0x0000'0001, world_payload.span());
    ByteWriter world_children;
    bethconv::test::write_group(world_children, 0x0000'0001, 1, block.span());
    world_and_children.raw(world_children.span());
    bethconv::test::write_group(file, tag_value("WRLD"), 0, world_and_children.span());

    // ---- one interior cell, outside any worldspace ----
    ByteWriter interior;
    write_cell(interior, 0x0000'0300, cell_record("TestpackInterior", false, 0, 0), 0,
               {
                   {.form = 0x0000'0301, .base = 0x0000'0100, .x = 0, .y = 0, .z = 0},
                   {.form = 0x0000'0302, .base = 0x0000'0110, .x = 0, .y = 0, .z = 180},
                   // Leads outside, arriving in front of the origin cell's door.
                   {.form = 0x0000'0303,
                    .base = 0x0000'0103,
                    .x = 0,
                    .y = 200,
                    .z = 0,
                    .extra = teleport(0x0000'0213, 1000, 900, 0, 3.14159265F)},
                   // A 128-unit trigger box turned 45 degrees, 300 units south.
                   {.form = 0x0000'0305,
                    .base = 0x0000'0105,
                    .x = 0,
                    .y = -300,
                    .z = 0,
                    .rz = 0.78539816F,
                    .extra = [] {
                        ByteWriter xprm;
                        for (const float v : {64.0F, 64.0F, 64.0F, 0.0F, 0.5F, 0.0F, 0.0F}) {
                            xprm.f32(v); // half extents, colour, unknown
                        }
                        xprm.u32(1); // box
                        ByteWriter out;
                        bethconv::test::write_field(out, "XPRM", xprm);
                        return out.bytes();
                    }()},
                   // The lever's reference script names the cube; so does its
                   // linked reference.
                   {.form = 0x0000'0304,
                    .base = 0x0000'0104,
                    .x = 100,
                    .y = 0,
                    .z = 0,
                    .extra = [] {
                        ByteWriter out;
                        bethconv::test::write_field(
                            out, "VMAD", vmad_field("TestpackLeverScript", 0x0000'0301));
                        ByteWriter xlkr;
                        xlkr.u32(0);
                        xlkr.u32(0x0000'0301);
                        bethconv::test::write_field(out, "XLKR", xlkr);
                        return out.bytes();
                    }()},
               },
               nullptr, 0, {{0x0000'0306, interior_navmesh()}},
               // The actor, 100 units west of the cube, facing east; persistent,
               // so its packages may take it outside.
               {{.form = 0x0000'0307,
                 .base = 0x0000'0164,
                 .x = -100,
                 .y = 0,
                 .z = 0,
                 .rz = 1.5707964F,
                 .record_flags = k_flag_persistent}});
    ByteWriter interior_sub_block;
    bethconv::test::write_group(interior_sub_block, 0, 3, interior.span());
    ByteWriter interior_block;
    bethconv::test::write_group(interior_block, 0, 2, interior_sub_block.span());
    bethconv::test::write_group(file, tag_value("CELL"), 0, interior_block.span());

    return file.bytes();
}

// ---- the assets -----------------------------------------------------------

/// What a cube collides as.
enum class Collision { none, fixed, clutter };

std::vector<std::byte> a_nif(bethconv::test::NifFlavor flavor, const std::string& shape_name,
                             Collision collision = Collision::none) {
    bethconv::test::NifBuilder builder(flavor);
    auto* root = builder.add_node("TestpackRoot");
    auto* shape = builder.add_shape(shape_name, bethconv::test::make_cube(24.0F));
    builder.add_shader(shape, "textures\\testpack\\cube.dds", "textures\\testpack\\cube_n.dds");
    (void)root;
    if (collision != Collision::none) {
        // A box around the cube: 12 game units in Havok units.
        auto box = std::make_unique<nifly::bhkBoxShape>();
        const float half = 12.0F / bethconv::mesh::k_havok_scale;
        box->dimensions = nifly::Vector3(half, half, half);
        bethconv::test::NifBuilder::Body body;
        if (collision == Collision::clutter) {
            body.layer = 4;   // clutter
            body.quality = 4; // moving
            body.mass = 5.0F;
        }
        builder.add_collision(std::move(box), body);
    }
    return builder.bytes();
}

void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

/// The synthetic install. Returns the number of files written.
/// An actor: a three-bone skeleton, a clip that turns the head bone 90
/// degrees about Z over 10 frames with a "FootLeft" annotation at 0.1 s, and a
/// cube skinned to the head, placed 120 units up like Skyrim's body parts.
std::vector<std::byte> actor_skeleton() {
    bethconv::test::HkxBuilder b(8);
    b.add_skeleton("NPC Root [Root]", {{"NPC Root [Root]", -1, {0, 0, 0}, {0, 0, 0, 1}},
                                       {"NPC Spine [Spn0]", 0, {0, 0, 70}, {0, 0, 0, 1}},
                                       {"NPC Head [Head]", 1, {0, 0, 50}, {0, 0, 0, 1}}});
    return b.bytes();
}

std::vector<std::byte> actor_clip() {
    using bethconv::test::HkxChannel;
    bethconv::test::HkxClip clip;
    clip.frames = 11;
    clip.tracks = 3;
    clip.skeleton_name = "NPC Root [Root]";
    clip.annotations = {{0.1f, "FootLeft"}};
    HkxChannel turn;
    turn.kind = HkxChannel::Kind::spline;
    turn.knots = {0, 0, 10, 10};
    turn.points = {{0, 0, 0, 1}, {0, 0, 0.70710678f, 0.70710678f}};
    bethconv::test::HkxBlock block;
    block.tracks = {{HkxChannel::constant({0, 0, 0, 0}), {}},
                    {HkxChannel::constant({0, 0, 70, 0}), {}},
                    {HkxChannel::constant({0, 0, 50, 0}), turn}};
    clip.blocks = {block};
    bethconv::test::HkxBuilder b(8);
    b.add_clip(clip);
    return b.bytes();
}

/// The race's behaviour project (`Behaviour.hkx`): one behaviour graph whose
/// clip generators name the idle, walk and run files, the animationdata
/// project giving those clips ids, and root motion: the walk 70 units forward
/// in a second (1 m/s), the run 280 (4 m/s).
std::vector<std::byte> actor_behaviour() {
    bethconv::test::HkxBuilder b(8);
    b.add_clip_generator("MT_Idle", "Animations\\mt_idle.hkx");
    b.add_clip_generator("MT_WalkForward", "Animations\\mt_walkforward.hkx");
    b.add_clip_generator("MT_RunForward", "Animations\\mt_runforward.hkx");
    return b.bytes();
}

std::vector<std::byte> text_bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span(text.data(), text.size()));
    return {view.begin(), view.end()};
}

constexpr std::string_view k_actor_project =
    "1\r\n1\r\nBehaviors\\Walk.hkx\r\n1\r\n"
    "MT_Idle\r\n1\r\n1\r\n0\r\n0\r\n0\r\n\r\n"
    "MT_WalkForward\r\n2\r\n1\r\n0\r\n0\r\n2\r\nFootLeft:0.2\r\nFootRight:0.7\r\n\r\n"
    "MT_RunForward\r\n3\r\n1\r\n0\r\n0\r\n0\r\n\r\n";

constexpr std::string_view k_actor_motion =
    "2\r\n1\r\n1\r\n1 0 70 0\r\n1\r\n1 0 0 0 1\r\n\r\n"
    "3\r\n1\r\n1\r\n1 0 280 0\r\n1\r\n1 0 0 0 1\r\n\r\n";

std::vector<std::byte> actor_body() {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    auto* root_bone = builder.add_node("NPC Root [Root]");
    auto* spine = builder.add_node("NPC Spine [Spn0]", root_bone);
    auto* head = builder.add_node("NPC Head [Head]", spine);
    nifly::MatTransform spine_t;
    spine_t.translation = nifly::Vector3(0, 0, 70);
    spine->SetTransformToParent(spine_t);
    nifly::MatTransform head_t;
    head_t.translation = nifly::Vector3(0, 0, 50);
    head->SetTransformToParent(head_t);
    auto* shape = builder.add_shape("Body", bethconv::test::make_cube());
    nifly::MatTransform placed;
    placed.translation = nifly::Vector3(0, 0, 120);
    shape->SetTransformToParent(placed);
    builder.add_skin(shape, head, placed, placed);
    return builder.bytes();
}

std::size_t write_data_folder(const fs::path& data) {
    std::size_t files = 0;
    const auto put = [&](const fs::path& rel, std::span<const std::byte> bytes) {
        write_file(data / rel, bytes);
        ++files;
    };

    put("Testpack.esm", build_plugin());

    // Static collision on the cube every base uses.
    const auto se_cube = a_nif(bethconv::test::NifFlavor::se, "Cube", Collision::fixed);
    put("meshes/testpack/cube_se.nif", se_cube);
    // Same bytes under a second path: two index lines, one asset.
    put("meshes/testpack/cube_se_copy.nif", se_cube);
    // Other encoding: the same call gives NiTriShape at stream 83, BSTriShape
    // at 100.
    // Placed by no reference; the engine's physics test drops it as clutter.
    put("meshes/testpack/cube_le.nif",
        a_nif(bethconv::test::NifFlavor::le, "CubeLE", Collision::clutter));
    // A space in the path; tests URI escaping.
    put("meshes/testpack/cube with space.nif", a_nif(bethconv::test::NifFlavor::se, "CubeSpaced"));

    put("textures/testpack/cube.dds",
        bethconv::testing::build_dds(DdsSpec{.width = 16, .height = 16, .mips = 5}));
    // Different bytes from the albedo, so a resolver returning the wrong slot
    // is detectable (identical textures would dedupe).
    put("textures/testpack/cube_n.dds",
        bethconv::testing::build_dds(DdsSpec{.width = 32, .height = 32, .mips = 6}));
    // A cubemap; Godot 4.7.2 loads these natively.
    put("textures/testpack/sky.dds", bethconv::testing::build_dds(DdsSpec{
                                         .width = 8, .height = 8, .mips = 4, .cubemap = true}));

    put("scripts/testpack/fixture.pex",
        bethconv::testing::build_script(bethconv::testing::empty_script("Fixture")));
    for (const auto& [vpath, bytes] : testpack::scripts()) {
        put(vpath, bytes);
    }

    // LOD for TestpackWorld: settings, terrain at levels 4 and 8, objects and
    // trees for the level-4 quad at (0, 0) and the tree atlas.
    put("lodsettings/testpackworld.lod", bethconv::testing::build_lod_settings(0, 0, 8, 4, 8));
    put("meshes/terrain/testpackworld/testpackworld.4.0.0.btr",
        a_nif(bethconv::test::NifFlavor::se, "land"));
    put("meshes/terrain/testpackworld/testpackworld.8.0.0.btr",
        a_nif(bethconv::test::NifFlavor::se, "Land"));
    put("meshes/terrain/testpackworld/objects/testpackworld.4.0.0.bto",
        a_nif(bethconv::test::NifFlavor::se, "Obj"));
    put("meshes/terrain/testpackworld/trees/testpackworld.lst",
        bethconv::testing::build_tree_list({{.index = 0}, {.index = 1, .u0 = 0.5F, .u1 = 1.0F}}));
    put("meshes/terrain/testpackworld/trees/testpackworld.4.0.0.btt",
        bethconv::testing::build_tree_blocks(
            {{0, {{.x = 2048.0F, .y = 2048.0F, .z = 0.0F, .ref = 0x0211}}},
             {1, {{.x = 6000.0F, .y = 1000.0F, .z = 50.0F, .rotation = 1.0F, .scale = 2.0F}}}}));
    put("textures/terrain/testpackworld/trees/testpackworldtreelod.dds",
        bethconv::testing::build_dds(DdsSpec{.width = 8, .height = 8, .mips = 4}));

    put("meshes/testpack/actor/skeleton.hkx", actor_skeleton());
    put("meshes/testpack/actor/turnhead.hkx", actor_clip());
    // The idle the race's behaviour folder offers; same bytes, one asset.
    put("meshes/testpack/actor/animations/mt_idle.hkx", actor_clip());
    // Walking: the behaviour project and the clips it names (same bytes).
    put("meshes/testpack/actor/behaviors/walk.hkx", actor_behaviour());
    put("meshes/testpack/actor/animations/mt_walkforward.hkx", actor_clip());
    put("meshes/testpack/actor/animations/mt_runforward.hkx", actor_clip());
    put("meshes/animationdata/behaviour.txt", text_bytes(k_actor_project));
    put("meshes/animationdata/boundanims/anims_behaviour.txt", text_bytes(k_actor_motion));
    put("meshes/testpack/actor/body.nif", actor_body());

    // Not converted; must appear in `report.json`.
    put("sound/testpack/voice.fuz", bethconv::testing::build_pex({}));

    return files;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"Build a synthetic bethconv pack containing no game data"};
    app.set_version_flag("--version", std::string(BETHCONV_VERSION));

    fs::path out;
    app.add_option("out", out, "Directory to write into")->required();
    bool keep_data = false;
    app.add_flag("--keep-data", keep_data,
                 "Keep the generated Data folder next to the pack (default: removed)");

    CLI11_PARSE(app, argc, argv);

    try {
        // Fixed parent directory name: sources are named `<parent>/<leaf>` and
        // that name is written into the manifest and `vpath.idx`. Using the
        // output directory's name would make the pack depend on where it was
        // built.
        const auto data = out / "testpack" / "Data";
        const auto pack_dir = out / "pack";
        fs::remove_all(out / "testpack");
        fs::remove_all(pack_dir);

        const auto files = write_data_folder(data);
        std::cout << "wrote " << files << " synthetic files into " << data << "\n";

        bethconv::archive::ArchiveSet set;
        const auto mounted = set.mount_loose(data, 0);
        if (!mounted) {
            std::cerr << "cannot mount the generated data folder: " << mounted.error().detail
                      << "\n";
            return EXIT_FAILURE;
        }

        bethconv::record::PluginList listed;
        listed.plugins.push_back(
            bethconv::record::ListedPlugin{.name = "Testpack.esm", .active = true});
        const auto order = bethconv::record::LoadOrder::build(
            data, listed,
            bethconv::record::LoadOrderOptions{.active_only = true,
                                               .add_implicit_masters = false,
                                               .always_loaded = {}});
        if (!order.problems().empty()) {
            for (const auto& problem : order.problems()) {
                std::cerr << "load order: " << problem.to_string() << "\n";
            }
            return EXIT_FAILURE;
        }

        bethconv::pack::ConvertOptions options;
        options.out = pack_dir;
        options.converter = std::string("bethconv-testpack ") + BETHCONV_VERSION;
        options.hash_archives = true;

        const auto result = bethconv::pack::convert(set, order, options);
        if (!result) {
            std::cerr << "convert failed: " << result.error().detail << "\n";
            return EXIT_FAILURE;
        }

        // ---- verify the output --------------------------------------------
        // A broken test pack would be debugged in the engine, so reopen it
        // through the consumer-side readers.
        const auto snapshot = bethconv::pack::Snapshot::open(pack_dir / "records.fb");
        if (!snapshot) {
            std::cerr << "the pack's own records.fb does not reopen: "
                      << snapshot.error().detail << "\n";
            return EXIT_FAILURE;
        }
        if (!snapshot->blob_intact()) {
            std::cerr << "the pack's own records.fb fails its blob hash\n";
            return EXIT_FAILURE;
        }

        // Every `vpath.idx` line must name an asset that exists.
        const auto index = bethconv::pack::VpathIndex::read(pack_dir / "vpath.idx");
        if (!index) {
            std::cerr << "the pack's own vpath.idx does not read back: " << index.error().detail
                      << "\n";
            return EXIT_FAILURE;
        }
        if (index->format_version() != bethconv::pack::k_pack_format_version) {
            std::cerr << "vpath.idx declares v" << index->format_version() << ", expected v"
                      << bethconv::pack::k_pack_format_version << "\n";
            return EXIT_FAILURE;
        }
        const auto assets = bethconv::pack::AssetReader::open(pack_dir);
        if (!assets) {
            std::cerr << "the pack's own asset store does not open: " << assets.error().detail
                      << "\n";
            return EXIT_FAILURE;
        }
        for (const auto& entry : index->entries()) {
            if (const auto bytes = assets->read(entry); !bytes || bytes->data.empty()) {
                std::cerr << "vpath.idx names an asset that is not there: " << entry.vpath
                          << " -> " << entry.hex << "\n";
                return EXIT_FAILURE;
            }
        }

        // Every reference in world.fb must place a known base, and every base
        // model must be in vpath.idx.
        const auto world = bethconv::pack::WorldFile::open(pack_dir / "world.fb");
        if (!world) {
            std::cerr << "the pack's own world.fb does not reopen: " << world.error().detail
                      << "\n";
            return EXIT_FAILURE;
        }
        std::size_t world_refs = 0;
        for (std::size_t i = 0; i < world->cell_count(); ++i) {
            // Held in a local: before C++23 (GCC 14), a range-for does not
            // keep the temporary cell alive.
            const auto cell = world->cell_at(i);
            for (const auto& ref : cell->refs) {
                ++world_refs;
                const auto base = world->base(ref.base);
                if (!base) {
                    std::cerr << "world.fb: reference " << ref.id << " places unknown base "
                              << ref.base << "\n";
                    return EXIT_FAILURE;
                }
                if (!base->model.empty() && index->find(base->model) == nullptr) {
                    std::cerr << "world.fb: base " << base->editor_id << " names model "
                              << base->model << ", which vpath.idx does not have\n";
                    return EXIT_FAILURE;
                }
            }
        }

        // The origin cell's terrain, and its texture through LTEX and TXST.
        const auto origin = world->cell_at_grid(0x0000'0001, 0, 0);
        const auto ground = world->land_texture(0x0000'0121);
        if (!origin || !origin->terrain || origin->terrain->layers.size() != 5 || !ground ||
            index->find(ground->diffuse) == nullptr || index->find(ground->normal) == nullptr) {
            std::cerr << "world.fb: the origin cell's terrain or its land texture is wrong\n";
            return EXIT_FAILURE;
        }

        // The door pair leads both ways, and the lever carries its script.
        const auto inside = world->cell(0x0000'0300);
        const auto lever = world->base(0x0000'0104);
        if (!inside || inside->doors.size() != 1 || inside->doors[0].destination != 0x0000'0213 ||
            origin->doors.size() != 1 || origin->doors[0].destination != 0x0000'0303 ||
            origin->locks.size() != 1 || inside->scripts.size() != 1 ||
            inside->links.size() != 1 || !lever || lever->scripts.size() != 1) {
            std::cerr << "world.fb: the doors, lock or lever script are wrong\n";
            return EXIT_FAILURE;
        }
        const auto quest = world->quest(0x0000'0150);
        if (!quest || quest->scripts.size() != 2 || quest->fragments.size() != 2 ||
            quest->aliases.size() != 1 || quest->aliases[0]->forced != 0x0000'0304 ||
            quest->aliases[0]->scripts.size() != 1 || !world->global(0x0000'0140)) {
            std::cerr << "world.fb: the quest or its global is wrong\n";
            return EXIT_FAILURE;
        }
        const auto east = world->cell(0x0000'0220);
        if (origin->navmeshes.size() != 1 || !east || east->navmeshes.size() != 1 ||
            inside->navmeshes.size() != 1 || inside->navmeshes[0].doors.size() != 1 ||
            origin->navmeshes[0].links.size() != 1 ||
            origin->navmeshes[0].links[0].navmesh != east->navmeshes[0].id) {
            std::cerr << "world.fb: the navmeshes are wrong\n";
            return EXIT_FAILURE;
        }
        const auto npc = world->npc(0x0000'0164);
        const auto work = world->package(0x0000'0172);
        const auto templ = world->package(0x0000'0170);
        if (!npc || npc->packages != std::vector<std::uint32_t>{0x0000'0172, 0x0000'0171} || !work ||
            work->template_ != 0x0000'0170 || work->hour != 8 || !templ ||
            templ->branches.size() != 4 || templ->branches[3]->procedure != "Sandbox") {
            std::cerr << "world.fb: the AI packages are wrong\n";
            return EXIT_FAILURE;
        }

        if (!keep_data) {
            fs::remove_all(out / "testpack");
        }

        std::cout << "pack:      " << pack_dir << "\n"
                  << "forms:     " << result->snapshot->forms << " in "
                  << result->snapshot->file_bytes << " bytes\n"
                  << "assets:    " << result->pack.distinct_assets << " distinct, "
                  << result->pack.index_entries << " index entries ("
                  << result->pack.meshes << " mesh, " << result->pack.textures << " texture, "
                  << result->pack.scripts << " script, " << result->pack.lod << " LOD, "
                  << result->pack.animations << " animation)\n"
                  << "deduped:   " << result->pack.deduped << "\n"
                  << "deferred:  " << result->pack.deferred << "\n"
                  << "failed:    " << result->pack.failed << "\n"
                  << "warnings:  " << result->pack.warnings << "\n"
                  << "verified:  records.fb reopens, blob hash matches, all "
                  << index->entries().size() << " vpath.idx entries resolve, "
                  << world_refs << " world.fb references place known bases, terrain and navmeshes resolve\n";

        // Every input is meant to convert; a failure means the pack is broken.
        if (result->pack.failed != 0) {
            std::cerr << "refusing to claim success: " << result->pack.failed
                      << " input(s) failed to convert. See " << (pack_dir / "report.json")
                      << "\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "testpack: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
