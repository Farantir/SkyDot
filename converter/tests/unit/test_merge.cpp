// SPDX-License-Identifier: GPL-3.0-or-later
//
// Merge tests for cases vanilla installs do not contain (real load orders are
// covered by the corpus harness):
//
//   * an override chain three plugins deep;
//   * a deleting override;
//   * a record injected into a master's FormID space;
//   * a FormID naming a missing master, which must cost exactly one record;
//   * an override with a different record type, which is ignored.
#include "bethconv/record/merge.hpp"

#include "../support/esm_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace bethconv;
using namespace bethconv::record;
using bethconv::test::ByteWriter;
using bethconv::test::TempDir;

namespace {

constexpr std::uint32_t k_flag_master = 0x0000'0001;
constexpr std::uint32_t k_flag_deleted = 0x0000'0020;
constexpr std::uint32_t k_flag_light = 0x0000'0200;

/// One record to place in a fixture plugin.
struct Rec {
    std::string type;
    std::uint32_t form_id{}; ///< Plugin-local, as on disk.
    std::string editor_id;
    std::uint32_t flags{};
};

/// A plugin: a TES4 header, then one top GRUP per record type.
void make_plugin(const TempDir& dir, std::string_view name, std::uint32_t flags,
                 const std::vector<std::string>& masters, const std::vector<Rec>& records) {
    ByteWriter file;
    bethconv::test::write_tes4(file, flags, masters);

    // Group by type, keeping the caller's order.
    std::vector<std::string> order;
    std::map<std::string, std::vector<const Rec*>> by_type;
    for (const auto& rec : records) {
        if (!by_type.contains(rec.type)) {
            order.push_back(rec.type);
        }
        by_type[rec.type].push_back(&rec);
    }

    for (const auto& type : order) {
        ByteWriter children;
        for (const Rec* rec : by_type[type]) {
            ByteWriter payload;
            ByteWriter edid;
            edid.zstring(rec->editor_id);
            bethconv::test::write_field(payload, "EDID", edid);
            bethconv::test::write_record(children, rec->type, rec->form_id, payload.span(),
                                         rec->flags);
        }
        ByteWriter label;
        label.tag(type);
        std::uint32_t raw = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            raw |= static_cast<std::uint32_t>(
                       static_cast<unsigned char>(label.bytes()[i]))
                   << (i * 8);
        }
        bethconv::test::write_group(file, raw, 0, children.span());
    }

    std::ofstream out(dir / name, std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

/// A CELL with children, laid out as in real files: `GRUP top:CELL > interior
/// block > interior sub-block > CELL > cell_children(CELL) >
/// cell_temporary_children(CELL) > REFR`.
void make_cell_plugin(const TempDir& dir, std::string_view name, std::uint32_t flags,
                      const std::vector<std::string>& masters, std::uint32_t cell_form,
                      const std::vector<std::uint32_t>& refr_forms) {
    ByteWriter refrs;
    for (const auto form : refr_forms) {
        ByteWriter payload;
        ByteWriter edid;
        edid.zstring("refr");
        bethconv::test::write_field(payload, "EDID", edid);
        bethconv::test::write_record(refrs, "REFR", form, payload.span());
    }
    ByteWriter temporary;
    bethconv::test::write_group(temporary, cell_form, 9, refrs.span());
    ByteWriter cell_children;
    bethconv::test::write_group(cell_children, cell_form, 6, temporary.span());

    ByteWriter cell_and_children;
    ByteWriter payload;
    ByteWriter edid;
    edid.zstring("cell");
    bethconv::test::write_field(payload, "EDID", edid);
    bethconv::test::write_record(cell_and_children, "CELL", cell_form, payload.span());
    cell_and_children.raw(cell_children.span());

    ByteWriter sub_block;
    bethconv::test::write_group(sub_block, 0, 3, cell_and_children.span());
    ByteWriter block;
    bethconv::test::write_group(block, 0, 2, sub_block.span());

    ByteWriter file;
    bethconv::test::write_tes4(file, flags, masters);
    ByteWriter label;
    label.tag("CELL");
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        raw |= static_cast<std::uint32_t>(static_cast<unsigned char>(label.bytes()[i])) << (i * 8);
    }
    bethconv::test::write_group(file, raw, 0, block.span());

    std::ofstream out(dir / name, std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

/// An exterior worldspace: `GRUP top:WRLD > WRLD > world_children(WRLD) > ext
/// block > ext sub-block > CELL > cell_children(CELL) >
/// cell_temporary_children(CELL) > REFR`.
///
/// The only layout where innermost and outermost form-labelled groups differ;
/// in an interior cell both carry the CELL label.
void make_exterior_plugin(const TempDir& dir, std::string_view name, std::uint32_t flags,
                          const std::vector<std::string>& masters, std::uint32_t world_form,
                          std::uint32_t cell_form, std::uint32_t refr_form) {
    ByteWriter refrs;
    {
        ByteWriter payload;
        ByteWriter edid;
        edid.zstring("exterior-refr");
        bethconv::test::write_field(payload, "EDID", edid);
        bethconv::test::write_record(refrs, "REFR", refr_form, payload.span());
    }
    ByteWriter temporary;
    bethconv::test::write_group(temporary, cell_form, 9, refrs.span());
    ByteWriter cell_children;
    bethconv::test::write_group(cell_children, cell_form, 6, temporary.span());

    ByteWriter cell_and_children;
    {
        ByteWriter payload;
        ByteWriter edid;
        edid.zstring("exterior-cell");
        bethconv::test::write_field(payload, "EDID", edid);
        bethconv::test::write_record(cell_and_children, "CELL", cell_form, payload.span());
    }
    cell_and_children.raw(cell_children.span());

    // Exterior block labels are packed int16 grid coordinates, not FormIDs,
    // hence label_is_form() lists types explicitly.
    ByteWriter sub_block;
    bethconv::test::write_group(sub_block, 0x0001'0002, 5, cell_and_children.span());
    ByteWriter block;
    bethconv::test::write_group(block, 0x0000'0000, 4, sub_block.span());
    ByteWriter world_children;
    bethconv::test::write_group(world_children, world_form, 1, block.span());

    ByteWriter world_and_children;
    {
        ByteWriter payload;
        ByteWriter edid;
        edid.zstring("a-worldspace");
        bethconv::test::write_field(payload, "EDID", edid);
        bethconv::test::write_record(world_and_children, "WRLD", world_form, payload.span());
    }
    world_and_children.raw(world_children.span());

    ByteWriter file;
    bethconv::test::write_tes4(file, flags, masters);
    ByteWriter label;
    label.tag("WRLD");
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        raw |= static_cast<std::uint32_t>(static_cast<unsigned char>(label.bytes()[i])) << (i * 8);
    }
    bethconv::test::write_group(file, raw, 0, world_and_children.span());

    std::ofstream out(dir / name, std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

PluginList listed(const std::vector<std::string>& names) {
    PluginList list;
    for (const auto& name : names) {
        list.plugins.push_back(ListedPlugin{.name = name, .active = true});
    }
    return list;
}

LoadOrderOptions plain() {
    return LoadOrderOptions{.active_only = true, .add_implicit_masters = false, .always_loaded = {}};
}

/// Collects winning records from the second pass, to compare with the index.
class Collector final : public MergedRecordSink {
public:
    struct Seen {
        FormId form;
        std::string type;
        std::string editor_id;
    };

    void on_record(const MergedRecord& merged, const RecordContext&, io::SpanReader& data,
                   const FormContext&) override {
        std::string editor_id;
        // The payload is one EDID field; read it directly so the test does not
        // depend on a type definition.
        if (auto tag = data.tag(); tag && *tag == FourCC{"EDID"}) {
            if (auto size = data.get<std::uint16_t>()) {
                if (auto body = data.subreader(*size)) {
                    if (auto text = body->zstring()) {
                        editor_id = *text;
                    }
                }
            }
        }
        seen.push_back(Seen{.form = merged.form,
                            .type = merged.type.to_string(),
                            .editor_id = std::move(editor_id)});
    }

    std::vector<Seen> seen;
};

} // namespace

// ---- the one rule ---------------------------------------------------------

TEST_CASE("the last plugin to write a form wins", "[record][merge]") {
    TempDir dir;
    make_plugin(dir, "Base.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "from-base"}});
    // 0x00000800 in an ESP whose first master is Base.esm: index 0 is
    // Base.esm, so this is an override. (0x01 would be the ESP's own record.)
    make_plugin(dir, "Middle.esp", 0, {"Base.esm"},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "from-middle"}});
    make_plugin(dir, "Top.esp", 0, {"Base.esm"},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "from-top"}});

    const auto order =
        LoadOrder::build(dir.path(), listed({"Base.esm", "Middle.esp", "Top.esp"}), plain());
    REQUIRE(order.problems().empty());

    const auto world = MergedWorld::build(order);
    CHECK(world.stats().visited == 3);
    CHECK(world.stats().forms == 1);
    CHECK(world.stats().collapsed == 2);

    const auto* record = world.find(FormId{0x0000'0800});
    REQUIRE(record != nullptr);
    CHECK(record->overrides == 2);
    CHECK(record->winner == 2); // Top.esp
    CHECK(record->owner == 0);  // Base.esm
    CHECK_FALSE(record->injected);

    // The second pass must return the winner's bytes, not the first plugin's;
    // no count would notice.
    Collector collector;
    world.for_each_record(collector);
    REQUIRE(collector.seen.size() == 1);
    CHECK(collector.seen.front().editor_id == "from-top");
}

TEST_CASE("the walk and the index agree about how many forms there are",
          "[record][merge]") {
    // The second pass must visit exactly as many records as the index has
    // forms, even when most records are overrides.
    TempDir dir;
    make_plugin(dir, "Base.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "a"},
                 {.type = "STAT", .form_id = 0x0000'0801, .editor_id = "b"},
                 {.type = "DOOR", .form_id = 0x0000'0802, .editor_id = "c"}});
    // Two overrides (slot 0) and one own record (slot 1).
    make_plugin(dir, "Over.esp", 0, {"Base.esm"},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "a2"},
                 {.type = "DOOR", .form_id = 0x0000'0802, .editor_id = "c2"},
                 {.type = "STAT", .form_id = 0x0100'0900, .editor_id = "new"}});

    const auto order = LoadOrder::build(dir.path(), listed({"Base.esm", "Over.esp"}), plain());
    const auto world = MergedWorld::build(order);
    CHECK(world.stats().visited == 6);
    CHECK(world.stats().forms == 4);
    CHECK(world.stats().collapsed == 2);

    Collector collector;
    world.for_each_record(collector);
    CHECK(collector.seen.size() == world.stats().forms);
}

// ---- what vanilla cannot show us -----------------------------------------

TEST_CASE("a deleting override stays in the world, flagged", "[record][merge]") {
    // Vanilla never deletes; a modded list does 278 times. The form must stay
    // in the index, marked deleted.
    TempDir dir;
    make_plugin(dir, "Base.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "doomed"}});
    make_plugin(dir, "Killer.esp", 0, {"Base.esm"},
                {{.type = "STAT",
                  .form_id = 0x0000'0800,
                  .editor_id = "",
                  .flags = k_flag_deleted}});

    const auto order = LoadOrder::build(dir.path(), listed({"Base.esm", "Killer.esp"}), plain());
    const auto world = MergedWorld::build(order);

    CHECK(world.stats().forms == 1);
    CHECK(world.stats().deleted == 1);
    const auto* record = world.find(FormId{0x0000'0800});
    REQUIRE(record != nullptr);
    CHECK(record->deleted);
    CHECK(record->winner == 1);
}

TEST_CASE("a record injected into a master's space is not an override",
          "[record][merge]") {
    // Vanilla SE has 11, a 295-plugin list 287. Unlike an override, the owner
    // never writes the form; only known after the full walk.
    TempDir dir;
    make_plugin(dir, "Base.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "base-own"}});
    // Slot 0 is Base.esm's space, and Base.esm has no such record.
    make_plugin(dir, "Injector.esp", 0, {"Base.esm"},
                {{.type = "KYWD", .form_id = 0x0000'0999, .editor_id = "injected"}});

    const auto order = LoadOrder::build(dir.path(), listed({"Base.esm", "Injector.esp"}), plain());
    const auto world = MergedWorld::build(order);

    CHECK(world.stats().forms == 2);
    CHECK(world.stats().injected == 1);
    CHECK(world.stats().collapsed == 0);

    const auto* injected = world.find(FormId{0x0000'0999});
    REQUIRE(injected != nullptr);
    CHECK(injected->injected);
    CHECK(injected->owner == 0);  // Base.esm's space
    CHECK(injected->winner == 1); // Injector.esp's bytes
    CHECK(injected->overrides == 0);

    const auto* own = world.find(FormId{0x0000'0800});
    REQUIRE(own != nullptr);
    CHECK_FALSE(own->injected);
}

TEST_CASE("the owner writing a form later clears the injected mark",
          "[record][merge]") {
    // The mark is provisional: a form first seen from a non-owner looks
    // injected until the owner shows up.
    TempDir dir;
    make_plugin(dir, "Base.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "owner-version"}});
    make_plugin(dir, "Early.esp", 0, {"Base.esm"},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "early"}});

    // Non-owner listed first. A real order cannot do this (masters load first),
    // but it is the state the merge sees mid-walk.
    const auto order = LoadOrder::build(dir.path(), listed({"Early.esp", "Base.esm"}), plain());
    const auto world = MergedWorld::build(order);

    const auto* record = world.find(FormId{0x0100'0800});
    REQUIRE(record != nullptr);
    CHECK_FALSE(record->injected);
    CHECK(world.stats().injected == 0);
}

TEST_CASE("an unresolvable FormID costs one record, not the file",
          "[record][merge]") {
    // Like vanilla Skyrim.esm's GMST 0x0123C00E: index 1 without masters.
    TempDir dir;
    make_plugin(dir, "Lonely.esm", k_flag_master, {},
                {{.type = "GMST", .form_id = 0x0123'C00E, .editor_id = "impossible"},
                 {.type = "STAT", .form_id = 0x0000'0800, .editor_id = "fine"}});

    const auto order = LoadOrder::build(dir.path(), listed({"Lonely.esm"}), plain());
    const auto world = MergedWorld::build(order);

    CHECK(world.stats().visited == 2);
    CHECK(world.stats().unresolved == 1);
    CHECK(world.stats().forms == 1);
    CHECK(world.find(FormId{0x0000'0800}) != nullptr);
    CHECK_FALSE(world.problems().empty());

    // Nor in the second pass.
    Collector collector;
    world.for_each_record(collector);
    REQUIRE(collector.seen.size() == 1);
    CHECK(collector.seen.front().editor_id == "fine");
}

TEST_CASE("an override that changes a record's type is reported, not applied",
          "[record][merge]") {
    // The first type is kept and the conflict reported; retyping would corrupt
    // consumers.
    TempDir dir;
    make_plugin(dir, "Base.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "a static"}});
    make_plugin(dir, "Confused.esp", 0, {"Base.esm"},
                {{.type = "DOOR", .form_id = 0x0000'0800, .editor_id = "now a door"}});

    const auto order = LoadOrder::build(dir.path(), listed({"Base.esm", "Confused.esp"}), plain());
    const auto world = MergedWorld::build(order);

    const auto* record = world.find(FormId{0x0000'0800});
    REQUIRE(record != nullptr);
    CHECK(record->type == FourCC{"STAT"});
    REQUIRE_FALSE(world.problems().empty());
    CHECK(world.problems().front().find("STAT") != std::string::npos);
    CHECK(world.problems().front().find("DOOR") != std::string::npos);
}

TEST_CASE("a form that changes type keeps its first writer", "[record][merge]") {
    // Applying the later record's flags and payload under the first type would
    // hand an ACTI body to a STAT parser; the later record must not count at all.
    TempDir dir;
    make_plugin(dir, "Base.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "the-static"}});
    make_plugin(dir, "Confused.esp", 0, {"Base.esm"},
                {{.type = "ACTI",
                  .form_id = 0x0000'0800,
                  .editor_id = "the-activator",
                  .flags = k_flag_deleted}});

    const auto order = LoadOrder::build(dir.path(), listed({"Base.esm", "Confused.esp"}), plain());
    const auto world = MergedWorld::build(order);

    CHECK(world.stats().visited == 2);
    CHECK(world.stats().forms == 1);
    CHECK(world.stats().collapsed == 0);
    CHECK(world.stats().deleted == 0);
    CHECK(world.stats().type_conflicts == 1);
    CHECK(world.report().find("ignored for changing a form's type") != std::string::npos);

    const auto* record = world.find(FormId{0x0000'0800});
    REQUIRE(record != nullptr);
    CHECK(record->type == FourCC{"STAT"});
    CHECK(record->winner == 0); // Base.esm
    CHECK(record->overrides == 0);
    CHECK(record->flags == 0);
    CHECK_FALSE(record->deleted);

    // The note names the plugin that wrote the kept type, not the ignored one.
    REQUIRE(world.problems().size() == 1);
    CHECK(world.problems().front().find("STAT in Base.esm") != std::string::npos);
    CHECK(world.problems().front().find("ACTI in Confused.esp") != std::string::npos);

    // The second pass hands the sink the first writer's bytes.
    Collector collector;
    world.for_each_record(collector);
    REQUIRE(collector.seen.size() == 1);
    CHECK(collector.seen.front().type == "STAT");
    CHECK(collector.seen.front().editor_id == "the-static");
}

TEST_CASE("a clean merge does not mention type conflicts", "[record][merge]") {
    TempDir dir;
    make_plugin(dir, "Base.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "a"}});
    const auto order = LoadOrder::build(dir.path(), listed({"Base.esm"}), plain());
    const auto world = MergedWorld::build(order);

    CHECK(world.stats().type_conflicts == 0);
    CHECK(world.report().find("changing a form's type") == std::string::npos);
}

// ---- parents --------------------------------------------------------------

TEST_CASE("a child's parent is the innermost group that names one",
          "[record][merge]") {
    // A REFR under CELL > block > sub-block > cell_children >
    // cell_temporary_children belongs to the CELL. The label is plugin-local and
    // needs remapping like any FormID.
    TempDir dir;
    make_cell_plugin(dir, "World.esm", k_flag_master, {}, 0x0000'1000,
                     {0x0000'2000, 0x0000'2001});

    const auto order = LoadOrder::build(dir.path(), listed({"World.esm"}), plain());
    const auto world = MergedWorld::build(order);

    CHECK(world.stats().forms == 3);
    CHECK(world.stats().unparented == 0);

    const auto* cell = world.find(FormId{0x0000'1000});
    REQUIRE(cell != nullptr);
    CHECK(cell->parent.is_null()); // interior blocks name no form

    for (const auto form : {0x0000'2000u, 0x0000'2001u}) {
        const auto* refr = world.find(FormId{form});
        REQUIRE(refr != nullptr);
        CHECK(refr->parent == FormId{0x0000'1000});
    }
}

TEST_CASE("a parent label is remapped through the plugin that wrote it",
          "[record][merge]") {
    // An unremapped label would give children of an overridden cell a
    // plausible parent in the wrong plugin's space.
    TempDir dir;
    make_cell_plugin(dir, "Base.esm", k_flag_master, {}, 0x0000'1000, {0x0000'2000});
    // Same cell and reference as an ESP writes them: slot 0 is Base.esm.
    make_cell_plugin(dir, "Mod.esp", 0, {"Base.esm"}, 0x0000'1000, {0x0000'2000});

    const auto order = LoadOrder::build(dir.path(), listed({"Base.esm", "Mod.esp"}), plain());
    const auto world = MergedWorld::build(order);

    CHECK(world.stats().forms == 2);
    CHECK(world.stats().collapsed == 2);
    const auto* refr = world.find(FormId{0x0000'2000});
    REQUIRE(refr != nullptr);
    CHECK(refr->winner == 1);
    CHECK(refr->parent == FormId{0x0000'1000});
}

TEST_CASE("in a worldspace, innermost and outermost disagree -- innermost wins",
          "[record][merge]") {
    // An exterior REFR sits under both a WRLD-labelled and a CELL-labelled
    // group; the cell owns it. Only an exterior fixture can test this.
    TempDir dir;
    make_exterior_plugin(dir, "World.esm", k_flag_master, {}, 0x0000'1000, 0x0000'2000,
                         0x0000'3000);

    const auto order = LoadOrder::build(dir.path(), listed({"World.esm"}), plain());
    const auto world = MergedWorld::build(order);

    CHECK(world.stats().forms == 3);
    CHECK(world.stats().unparented == 0);

    const auto* wrld = world.find(FormId{0x0000'1000});
    REQUIRE(wrld != nullptr);
    CHECK(wrld->parent.is_null()); // top group label is a type tag

    const auto* cell = world.find(FormId{0x0000'2000});
    REQUIRE(cell != nullptr);
    CHECK(cell->parent == FormId{0x0000'1000}); // its worldspace

    const auto* refr = world.find(FormId{0x0000'3000});
    REQUIRE(refr != nullptr);
    CHECK(refr->parent == FormId{0x0000'2000}); // its cell, not its worldspace
}

TEST_CASE("a parent label is remapped, not copied", "[record][merge]") {
    // Local and global indices must differ or the bug hides: Mod.esp is at
    // position 2 with one master, so its own records have local index 1 and
    // global index 2.
    TempDir dir;
    make_plugin(dir, "A.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "a"}});
    make_plugin(dir, "B.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0801, .editor_id = "b"}});
    make_cell_plugin(dir, "Mod.esp", 0, {"A.esm"}, 0x0100'1000, {0x0100'2000});

    const auto order =
        LoadOrder::build(dir.path(), listed({"A.esm", "B.esm", "Mod.esp"}), plain());
    REQUIRE(order.entries().size() == 3);

    const auto world = MergedWorld::build(order);
    CHECK(world.stats().unparented == 0);

    // Local 0x01xxxxxx (one past a single-master list) -> global 0x02xxxxxx.
    const auto* cell = world.find(FormId{0x0200'1000});
    REQUIRE(cell != nullptr);
    const auto* refr = world.find(FormId{0x0200'2000});
    REQUIRE(refr != nullptr);
    CHECK(refr->parent == FormId{0x0200'1000});
    // Not the label as written.
    CHECK(world.find(FormId{0x0100'1000}) == nullptr);
}

// ---- the light space ------------------------------------------------------

TEST_CASE("a light plugin's records land in the 0xFE space", "[record][merge]") {
    // The compact space never appears on disk (0 of 622 plugins); the merge is
    // where it first appears.
    TempDir dir;
    make_plugin(dir, "Base.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "base"}});
    make_plugin(dir, "Small.esp", k_flag_light, {"Base.esm"},
                {{.type = "STAT", .form_id = 0x0100'0123, .editor_id = "light-own"}});

    const auto order = LoadOrder::build(dir.path(), listed({"Base.esm", "Small.esp"}), plain());
    REQUIRE(order.entries()[1].is_light);

    const auto world = MergedWorld::build(order);
    CHECK(world.stats().forms == 2);
    // First light plugin: compact index 0, so 0xFE000 | 0x123.
    CHECK(world.find(FormId{0xFE00'0123}) != nullptr);
    CHECK(world.find(FormId{0x0000'0800}) != nullptr);
}

// ---- filtering ------------------------------------------------------------

TEST_CASE("a type filter indexes only what was asked for", "[record][merge]") {
    // Callers placing only CELL/REFR should not index 40,928 INFO records.
    TempDir dir;
    make_plugin(dir, "Base.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "a"},
                 {.type = "DOOR", .form_id = 0x0000'0801, .editor_id = "b"},
                 {.type = "LIGH", .form_id = 0x0000'0802, .editor_id = "c"}});

    const auto order = LoadOrder::build(dir.path(), listed({"Base.esm"}), plain());
    MergeOptions options;
    options.types = {FourCC{"STAT"}, FourCC{"LIGH"}};
    const auto world = MergedWorld::build(order, options);

    CHECK(world.stats().visited == 2);
    CHECK(world.stats().forms == 2);
    CHECK(world.find(FormId{0x0000'0801}) == nullptr);

    // The second pass must apply the same filter.
    Collector collector;
    world.for_each_record(collector);
    CHECK(collector.seen.size() == 2);
}

// ---- strings, at merge time ----------------------------------------------

TEST_CASE("the second pass hands each record its own plugin's tables",
          "[record][merge][strings]") {
    // Two localized plugins using index 1 for different text, as in vanilla
    // (#00000001 is "The Ratway Vaults" in Skyrim.esm and "Auriel's Bow" in
    // Dawnguard.esm). Each record must be resolved with its own plugin's tables.
    TempDir dir;
    constexpr std::uint32_t k_flag_localized = 0x0000'0080;
    make_plugin(dir, "Base.esm", k_flag_master | k_flag_localized, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "a"}});
    make_plugin(dir, "Mod.esp", k_flag_localized, {"Base.esm"},
                {{.type = "STAT", .form_id = 0x0100'0900, .editor_id = "b"}});

    const auto order = LoadOrder::build(dir.path(), listed({"Base.esm", "Mod.esp"}), plain());

    std::vector<std::string> asked;
    MergeOptions options;
    options.strings = [&asked](std::string_view vpath)
        -> std::optional<std::vector<std::byte>> {
        asked.emplace_back(vpath);
        return std::nullopt;
    };
    const auto world = MergedWorld::build(order, options);

    // Each localized plugin is looked up by its own name, all three kinds.
    CHECK(asked.size() == 6);
    CHECK(std::ranges::find(asked, "strings/base_english.strings") != asked.end());
    CHECK(std::ranges::find(asked, "strings/mod_english.strings") != asked.end());
    CHECK(std::ranges::find(asked, "strings/mod_english.ilstrings") != asked.end());
    CHECK(world.stats().string_tables == 0); // none were provided
}

TEST_CASE("the merge does not re-litigate what the load order rejected",
          "[record][merge]") {
    // Unopenable plugins are dropped by LoadOrder::build as `unreadable`, so the
    // merge never sees them and consumers check one place.
    TempDir dir;
    make_plugin(dir, "Good.esm", k_flag_master, {},
                {{.type = "STAT", .form_id = 0x0000'0800, .editor_id = "fine"}});
    { // Not a plugin at all.
        std::ofstream out(dir / "Broken.esp", std::ios::binary);
        out << "not a plugin";
    }

    const auto order = LoadOrder::build(dir.path(), listed({"Good.esm", "Broken.esp"}), plain());
    REQUIRE(order.entries().size() == 1);
    REQUIRE(order.problems().size() == 1);
    CHECK(order.problems().front().kind == LoadOrderProblem::Kind::unreadable);

    const auto world = MergedWorld::build(order);
    CHECK(world.stats().plugins == 1);
    CHECK(world.stats().forms == 1);
    // MergeStats::unreadable is for plugins vanishing between the two steps.
    CHECK(world.stats().unreadable == 0);
    CHECK(world.problems().empty());
}
