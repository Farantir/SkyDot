// SPDX-License-Identifier: GPL-3.0-or-later
//
// Field definition tests. `bethconv forms` already covers valid real data
// (957,133 SE and 721,343 LE records, no failures). These cover what it cannot:
//
//   1. truncated and over-long fields: the record fails, not the file, and no
//      read goes past the end;
//   2. the accepted size variants (STAT DNAM 8/12, CELL DATA 1/2, XCLC 8/12,
//      XLKR 4/8), which are too rare to target in real data;
//   3. undefined fields being tallied instead of failing.
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/form_census.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/forms_world.hpp"

#include "../support/esm_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <span>
#include <string>
#include <vector>

using namespace bethconv;
using namespace bethconv::test;
using record::FourCC;

namespace {

/// Records what a parse did not understand, so a test can assert on it.
class Tally final : public record::FieldTally {
public:
    struct Note {
        std::string record;
        std::string field;
        std::size_t size;
    };

    void unhandled(FourCC rec, FourCC field, std::uint32_t size) override {
        unhandled_.push_back({rec.to_string(), field.to_string(), size});
    }
    void leftover(FourCC rec, FourCC field, std::size_t bytes) override {
        leftover_.push_back({rec.to_string(), field.to_string(), bytes});
    }

    [[nodiscard]] const std::vector<Note>& unhandled() const noexcept { return unhandled_; }
    [[nodiscard]] const std::vector<Note>& leftover() const noexcept { return leftover_; }

    [[nodiscard]] bool saw_unhandled(std::string_view field) const {
        return std::ranges::any_of(unhandled_,
                                   [&](const Note& n) { return n.field == field; });
    }
    [[nodiscard]] bool saw_leftover(std::string_view field) const {
        return std::ranges::any_of(leftover_,
                                   [&](const Note& n) { return n.field == field; });
    }

private:
    std::vector<Note> unhandled_;
    std::vector<Note> leftover_;
};

/// A reader over a test payload. The ByteWriter must outlive it.
io::SpanReader reader_over(const ByteWriter& w) {
    return io::SpanReader{w.span(), "test-fixture"};
}

/// A record header, for the one parse that needs one.
record::RecordHeader refr_header(std::uint32_t flags = 0) {
    record::RecordHeader header;
    header.type = FourCC{"REFR"};
    header.flags = flags;
    header.form_id = record::FormId{0x00012345};
    return header;
}

ByteWriter vec3(float x, float y, float z) {
    ByteWriter w;
    w.f32(x);
    w.f32(y);
    w.f32(z);
    return w;
}

} // namespace

// ---- field readers --------------------------------------------------------

TEST_CASE("a zstring with no NUL is unterminated, not silently truncated",
          "[record][forms][field_reader]") {
    ByteWriter w;
    w.raw("meshes/clutter/common/bucket01.nif"); // no terminator
    auto reader = reader_over(w);

    const auto text = record::read_zstring(reader);
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error().kind == io::ErrorKind::unterminated);
}

TEST_CASE("an empty string field is an empty string, not an error",
          "[record][forms][field_reader]") {
    // Blank MODL occurs in mods; it must not fail the record.
    ByteWriter w;
    auto reader = reader_over(w);

    const auto text = record::read_zstring(reader);
    REQUIRE(text.has_value());
    CHECK(text->empty());
}

TEST_CASE("a localized string field shorter than its index fails",
          "[record][forms][field_reader]") {
    ByteWriter w;
    w.u16(0x1234); // two bytes where a 4-byte STRINGS index belongs
    auto reader = reader_over(w);

    const record::FormContext ctx{.localized = true, .tally = nullptr};
    const auto value = record::read_lstring(reader, ctx);
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().kind == io::ErrorKind::truncated);
}

TEST_CASE("the same four bytes are an index or text depending on the plugin",
          "[record][forms][field_reader]") {
    // SE is localized and LE is not, but only a fixture can send the same bytes
    // through both paths.
    ByteWriter w;
    w.raw("ab");
    w.u8(0);
    w.u8(0);

    {
        auto reader = reader_over(w);
        const auto value = record::read_lstring(reader, {.localized = true, .tally = nullptr});
        REQUIRE(value.has_value());
        CHECK(value->is_id);
        CHECK(value->id == 0x00006261u);
        CHECK(value->text.empty());
    }
    {
        auto reader = reader_over(w);
        const auto value = record::read_lstring(reader, {.localized = false, .tally = nullptr});
        REQUIRE(value.has_value());
        CHECK_FALSE(value->is_id);
        CHECK(value->text == "ab");
    }
}

TEST_CASE("an unresolved STRINGS index does not print as an empty string",
          "[record][forms][field_reader]") {
    // An unresolved index must stay distinguishable from an empty name.
    const record::LString id{.text = {}, .id = 0x0000012Cu, .is_id = true};
    const record::LString empty{};

    CHECK(id.to_string() == "#0000012C");
    CHECK_FALSE(id.empty());
    CHECK(empty.empty());
    CHECK(empty.to_string().empty());
}

TEST_CASE("object bounds one byte short fail rather than read a stray zero",
          "[record][forms][field_reader]") {
    ByteWriter w;
    for (int i = 0; i < 5; ++i) {
        w.u16(1);
    }
    w.u8(0); // eleven bytes where OBND is twelve
    auto reader = reader_over(w);

    const auto bounds = record::read_object_bounds(reader);
    REQUIRE_FALSE(bounds.has_value());
    CHECK(bounds.error().kind == io::ErrorKind::truncated);
}

TEST_CASE("a FormID array that is not a whole number of FormIDs is corruption",
          "[record][forms][field_reader]") {
    ByteWriter w;
    w.u32(0x0001A26F);
    w.u16(0x1234); // a trailing half FormID
    auto reader = reader_over(w);

    const auto forms = record::read_formid_array(reader);
    REQUIRE_FALSE(forms.has_value());
    // bad_value, not truncated: the field is complete but its length is
    // impossible.
    CHECK(forms.error().kind == io::ErrorKind::bad_value);
}

TEST_CASE("an empty FormID array is empty, not an error", "[record][forms][field_reader]") {
    ByteWriter w;
    auto reader = reader_over(w);

    const auto forms = record::read_formid_array(reader);
    REQUIRE(forms.has_value());
    CHECK(forms->empty());
}

// ---- STAT -----------------------------------------------------------------

TEST_CASE("STAT DNAM is read at both the sizes Bethesda wrote", "[record][forms][stat]") {
    // 8 bytes in Skyrim.esm's 9,720 STATs, 12 in the 711 written later. The
    // extra word is kept raw.
    SECTION("the 8-byte form leaves no extra") {
        ByteWriter payload;
        ByteWriter edid;
        edid.zstring("TestStatic");
        write_field(payload, "EDID", edid);
        ByteWriter dnam;
        dnam.f32(45.0F);
        dnam.u32(0x0010C1B2);
        write_field(payload, "DNAM", dnam);

        Tally tally;
        auto reader = reader_over(payload);
        const auto stat = record::parse_static(reader, {.localized = false, .tally = &tally});
        REQUIRE(stat.has_value());
        CHECK(stat->editor_id == "TestStatic");
        CHECK(stat->max_angle == 45.0F);
        CHECK(stat->material.value == 0x0010C1B2u);
        CHECK(stat->dnam_extra.empty());
        CHECK(tally.leftover().empty());
    }

    SECTION("the 12-byte form keeps the fourth word verbatim") {
        ByteWriter payload;
        ByteWriter dnam;
        dnam.f32(45.0F);
        dnam.u32(0x0010C1B2);
        dnam.u32(0xDEADBEEF);
        write_field(payload, "DNAM", dnam);

        Tally tally;
        auto reader = reader_over(payload);
        const auto stat = record::parse_static(reader, {.localized = false, .tally = &tally});
        REQUIRE(stat.has_value());
        CHECK(stat->dnam_extra.size() == 4);
        // Kept as bytes; no meaning is assigned.
        CHECK(stat->dnam_extra[0] == std::byte{0xEF});
        CHECK(stat->dnam_extra[3] == std::byte{0xDE});
        // Consumed, so not reported as leftover.
        CHECK(tally.leftover().empty());
    }
}

TEST_CASE("a STAT DNAM too short for its first two words fails the record",
          "[record][forms][stat]") {
    ByteWriter payload;
    ByteWriter dnam;
    dnam.f32(45.0F); // the MATO FormID is missing
    write_field(payload, "DNAM", dnam);

    Tally tally;
    auto reader = reader_over(payload);
    const auto stat = record::parse_static(reader, {.localized = false, .tally = &tally});
    REQUIRE_FALSE(stat.has_value());
    CHECK(stat.error().kind == io::ErrorKind::truncated);
}

TEST_CASE("a STAT MNAM short of its four fixed slots fails rather than guessing",
          "[record][forms][stat]") {
    // MNAM is 1,040 bytes (four 260-byte slots) on all 824 vanilla STATs that
    // have it; a short one cannot be split.
    ByteWriter payload;
    ByteWriter mnam;
    for (std::size_t i = 0; i < 600; ++i) {
        mnam.u8(0);
    }
    write_field(payload, "MNAM", mnam);

    Tally tally;
    auto reader = reader_over(payload);
    const auto stat = record::parse_static(reader, {.localized = false, .tally = &tally});
    REQUIRE_FALSE(stat.has_value());
    CHECK(stat.error().kind == io::ErrorKind::truncated);
}

TEST_CASE("STAT MNAM trims its NUL padding and tolerates empty LOD slots",
          "[record][forms][stat]") {
    ByteWriter payload;
    ByteWriter mnam;
    const char* const slots[] = {"lod/whiterun_lod0.nif", "lod/whiterun_lod1.nif", "", ""};
    for (const char* slot : slots) {
        const std::string text{slot};
        mnam.raw(text);
        for (std::size_t i = text.size(); i < record::Static::k_lod_path_size; ++i) {
            mnam.u8(0);
        }
    }
    write_field(payload, "MNAM", mnam);

    Tally tally;
    auto reader = reader_over(payload);
    const auto stat = record::parse_static(reader, {.localized = false, .tally = &tally});
    REQUIRE(stat.has_value());
    CHECK(stat->lod_models[0] == "lod/whiterun_lod0.nif");
    CHECK(stat->lod_models[1] == "lod/whiterun_lod1.nif");
    CHECK(stat->lod_models[2].empty());
    CHECK(stat->lod_models[3].empty());
    CHECK(tally.leftover().empty());
}

TEST_CASE("a field with no definition is tallied and the record still parses",
          "[record][forms][stat]") {
    // Unknown fields must not fail the record.
    ByteWriter payload;
    ByteWriter edid;
    edid.zstring("TestStatic");
    write_field(payload, "EDID", edid);
    ByteWriter vmad;
    vmad.u32(0);
    vmad.u32(0);
    write_field(payload, "VMAD", vmad);
    ByteWriter obnd;
    for (int i = 0; i < 6; ++i) {
        obnd.u16(static_cast<std::uint16_t>(i));
    }
    write_field(payload, "OBND", obnd);

    Tally tally;
    auto reader = reader_over(payload);
    const auto stat = record::parse_static(reader, {.localized = false, .tally = &tally});
    REQUIRE(stat.has_value());
    CHECK(stat->editor_id == "TestStatic");
    // Fields after the unknown one are still read.
    CHECK(stat->bounds.z2 == 5);
    REQUIRE(tally.unhandled().size() == 1);
    CHECK(tally.unhandled()[0].record == "STAT");
    CHECK(tally.unhandled()[0].field == "VMAD");
    CHECK(tally.unhandled()[0].size == 8);
}

TEST_CASE("a defined field longer than its definition is reported as leftover",
          "[record][forms][stat]") {
    // The leftover check must fire on a fixture, or its silence on real data
    // means nothing.
    ByteWriter payload;
    ByteWriter obnd;
    for (int i = 0; i < 6; ++i) {
        obnd.u16(0);
    }
    obnd.u32(0); // four bytes OBND does not have
    write_field(payload, "OBND", obnd);

    Tally tally;
    auto reader = reader_over(payload);
    const auto stat = record::parse_static(reader, {.localized = false, .tally = &tally});
    REQUIRE(stat.has_value()); // over-long is a report, not a failure
    REQUIRE(tally.leftover().size() == 1);
    CHECK(tally.leftover()[0].record == "STAT");
    CHECK(tally.leftover()[0].field == "OBND");
    CHECK(tally.leftover()[0].size == 4);
}

TEST_CASE("a parse with no tally costs nothing and still succeeds",
          "[record][forms][stat]") {
    // Conversion passes no tally; the parser must not require one.
    ByteWriter payload;
    ByteWriter vmad;
    vmad.u32(0);
    write_field(payload, "VMAD", vmad);

    auto reader = reader_over(payload);
    const auto stat = record::parse_static(reader, {.localized = false, .tally = nullptr});
    CHECK(stat.has_value());
}

// ---- CELL -----------------------------------------------------------------

TEST_CASE("CELL DATA is read at one byte and at two", "[record][forms][cell]") {
    // 2 bytes 17,237 times, 1 byte 331 times in Skyrim.esm (high byte omitted
    // when zero).
    SECTION("two bytes carry the high flags") {
        ByteWriter payload;
        ByteWriter data;
        data.u16(0x0101); // interior + use sky lighting
        write_field(payload, "DATA", data);

        Tally tally;
        auto reader = reader_over(payload);
        const auto cell = record::parse_cell(reader, {.localized = false, .tally = &tally});
        REQUIRE(cell.has_value());
        CHECK(cell->flags == 0x0101);
        CHECK(cell->is_interior());
        CHECK(cell->has(record::Cell::Flag::use_sky_lighting));
        CHECK(tally.leftover().empty());
    }

    SECTION("one byte is not truncation") {
        ByteWriter payload;
        ByteWriter data;
        data.u8(0x01);
        write_field(payload, "DATA", data);

        Tally tally;
        auto reader = reader_over(payload);
        const auto cell = record::parse_cell(reader, {.localized = false, .tally = &tally});
        REQUIRE(cell.has_value());
        CHECK(cell->flags == 0x0001);
        CHECK(cell->is_interior());
        CHECK(tally.leftover().empty());
    }
}

TEST_CASE("a CELL DATA of zero bytes fails the record", "[record][forms][cell]") {
    ByteWriter payload;
    write_field(payload, "DATA", ByteWriter{});

    Tally tally;
    auto reader = reader_over(payload);
    const auto cell = record::parse_cell(reader, {.localized = false, .tally = &tally});
    REQUIRE_FALSE(cell.has_value());
    CHECK(cell.error().kind == io::ErrorKind::truncated);
}

TEST_CASE("CELL XCLC is read with and without its 1.70 flags word",
          "[record][forms][cell]") {
    SECTION("12 bytes, the SE form") {
        ByteWriter payload;
        ByteWriter xclc;
        xclc.u32(static_cast<std::uint32_t>(-13));
        xclc.u32(9);
        xclc.u32(1);
        write_field(payload, "XCLC", xclc);

        Tally tally;
        auto reader = reader_over(payload);
        const auto cell = record::parse_cell(reader, {.localized = false, .tally = &tally});
        REQUIRE(cell.has_value());
        REQUIRE(cell->grid.has_value());
        CHECK(cell->grid->x == -13);
        CHECK(cell->grid->y == 9);
        CHECK(cell->grid->flags == 1u);
        CHECK(tally.leftover().empty());
    }

    SECTION("8 bytes, the pre-SE form, is a grid without flags") {
        ByteWriter payload;
        ByteWriter xclc;
        xclc.u32(static_cast<std::uint32_t>(-13));
        xclc.u32(9);
        write_field(payload, "XCLC", xclc);

        Tally tally;
        auto reader = reader_over(payload);
        const auto cell = record::parse_cell(reader, {.localized = false, .tally = &tally});
        REQUIRE(cell.has_value());
        REQUIRE(cell->grid.has_value());
        CHECK(cell->grid->x == -13);
        CHECK(cell->grid->flags == 0u);
        CHECK(tally.leftover().empty());
    }
}

TEST_CASE("an interior cell has no grid at all", "[record][forms][cell]") {
    // No XCLC means interior; it must stay nullopt, not (0,0), which is a real
    // Tamriel cell.
    ByteWriter payload;
    ByteWriter data;
    data.u16(0x0001);
    write_field(payload, "DATA", data);

    auto reader = reader_over(payload);
    const auto cell = record::parse_cell(reader, {.localized = false, .tally = nullptr});
    REQUIRE(cell.has_value());
    CHECK_FALSE(cell->grid.has_value());
}

TEST_CASE("CELL XCLR is an array of regions and a partial one is caught",
          "[record][forms][cell]") {
    SECTION("three regions") {
        ByteWriter payload;
        ByteWriter xclr;
        xclr.u32(0x0001A26F);
        xclr.u32(0x0001A270);
        xclr.u32(0x0001A271);
        write_field(payload, "XCLR", xclr);

        auto reader = reader_over(payload);
        const auto cell = record::parse_cell(reader, {.localized = false, .tally = nullptr});
        REQUIRE(cell.has_value());
        REQUIRE(cell->regions.size() == 3);
        CHECK(cell->regions[2].value == 0x0001A271u);
    }

    SECTION("a trailing partial FormID costs the record") {
        ByteWriter payload;
        ByteWriter xclr;
        xclr.u32(0x0001A26F);
        xclr.u8(0x12);
        write_field(payload, "XCLR", xclr);

        auto reader = reader_over(payload);
        const auto cell = record::parse_cell(reader, {.localized = false, .tally = nullptr});
        REQUIRE_FALSE(cell.has_value());
        CHECK(cell.error().kind == io::ErrorKind::bad_value);
    }
}

TEST_CASE("CELL XCLL is preserved verbatim at either documented length",
          "[record][forms][cell]") {
    // 92 bytes 589 times and 64 once in vanilla; kept raw.
    for (const std::size_t length : {std::size_t{64}, std::size_t{92}}) {
        ByteWriter payload;
        ByteWriter xcll;
        for (std::size_t i = 0; i < length; ++i) {
            xcll.u8(static_cast<std::uint8_t>(i));
        }
        write_field(payload, "XCLL", xcll);

        Tally tally;
        auto reader = reader_over(payload);
        const auto cell = record::parse_cell(reader, {.localized = false, .tally = &tally});
        REQUIRE(cell.has_value());
        CHECK(cell->lighting.size() == length);
        CHECK(tally.leftover().empty());
    }
}

// ---- REFR -----------------------------------------------------------------

TEST_CASE("a REFR carries position, rotation and its base object",
          "[record][forms][refr]") {
    ByteWriter payload;
    ByteWriter name;
    name.u32(0x0001A26F);
    write_field(payload, "NAME", name);
    ByteWriter data;
    data.raw(vec3(1.0F, 2.0F, 3.0F).span());
    data.raw(vec3(0.0F, 0.0F, 1.5F).span());
    write_field(payload, "DATA", data);

    Tally tally;
    auto reader = reader_over(payload);
    const auto header = refr_header();
    const auto refr = record::parse_reference(header, reader,
                                              {.localized = false, .tally = &tally});
    REQUIRE(refr.has_value());
    CHECK(refr->base.value == 0x0001A26Fu);
    CHECK(refr->position == record::Vec3{1.0F, 2.0F, 3.0F});
    CHECK(refr->rotation == record::Vec3{0.0F, 0.0F, 1.5F});
    CHECK(tally.leftover().empty());
}

TEST_CASE("a REFR DATA short of its six floats fails the record",
          "[record][forms][refr]") {
    ByteWriter payload;
    ByteWriter data;
    data.raw(vec3(1.0F, 2.0F, 3.0F).span());
    data.f32(0.0F); // rotation is one float, not three
    write_field(payload, "DATA", data);

    auto reader = reader_over(payload);
    const auto header = refr_header();
    const auto refr = record::parse_reference(header, reader,
                                              {.localized = false, .tally = nullptr});
    REQUIRE_FALSE(refr.has_value());
    CHECK(refr.error().kind == io::ErrorKind::truncated);
}

TEST_CASE("a REFR without XSCL is scale 1.0, never 0", "[record][forms][refr]") {
    // 494,263 of 693,333 vanilla REFRs have no XSCL and rely on the 1.0
    // default; 0 would collapse them.
    ByteWriter payload;
    ByteWriter name;
    name.u32(0x0001A26F);
    write_field(payload, "NAME", name);

    auto reader = reader_over(payload);
    const auto header = refr_header();
    const auto refr = record::parse_reference(header, reader,
                                              {.localized = false, .tally = nullptr});
    REQUIRE(refr.has_value());
    CHECK(refr->scale == 1.0F);
}

TEST_CASE("REFR XLKR is read at both 8 and 4 bytes", "[record][forms][refr]") {
    // 8 bytes 12,467 times, 4 bytes ten times (keyword omitted). Reading the
    // short form as a keyword would invert the link.
    SECTION("keyword and target") {
        ByteWriter payload;
        ByteWriter xlkr;
        xlkr.u32(0x0001CB84);
        xlkr.u32(0x0002F3A1);
        write_field(payload, "XLKR", xlkr);

        Tally tally;
        auto reader = reader_over(payload);
        const auto header = refr_header();
        const auto refr = record::parse_reference(header, reader,
                                                  {.localized = false, .tally = &tally});
        REQUIRE(refr.has_value());
        REQUIRE(refr->linked_references.size() == 1);
        CHECK(refr->linked_references[0].keyword.value == 0x0001CB84u);
        CHECK(refr->linked_references[0].target.value == 0x0002F3A1u);
        CHECK(tally.leftover().empty());
    }

    SECTION("target only, keyword left null") {
        ByteWriter payload;
        ByteWriter xlkr;
        xlkr.u32(0x0002F3A1);
        write_field(payload, "XLKR", xlkr);

        Tally tally;
        auto reader = reader_over(payload);
        const auto header = refr_header();
        const auto refr = record::parse_reference(header, reader,
                                                  {.localized = false, .tally = &tally});
        REQUIRE(refr.has_value());
        REQUIRE(refr->linked_references.size() == 1);
        CHECK(refr->linked_references[0].keyword.value == 0u);
        CHECK(refr->linked_references[0].target.value == 0x0002F3A1u);
        CHECK(tally.leftover().empty());
    }
}

TEST_CASE("repeated XLKR fields accumulate rather than overwrite",
          "[record][forms][refr]") {
    ByteWriter payload;
    for (std::uint32_t target : {0x0002F3A1u, 0x0002F3A2u}) {
        ByteWriter xlkr;
        xlkr.u32(0x0001CB84);
        xlkr.u32(target);
        write_field(payload, "XLKR", xlkr);
    }

    auto reader = reader_over(payload);
    const auto header = refr_header();
    const auto refr = record::parse_reference(header, reader,
                                              {.localized = false, .tally = nullptr});
    REQUIRE(refr.has_value());
    REQUIRE(refr->linked_references.size() == 2);
    CHECK(refr->linked_references[1].target.value == 0x0002F3A2u);
}

TEST_CASE("REFR placement flags come from the record header, not a field",
          "[record][forms][refr]") {
    // "Initially disabled" and "persistent" are header bits, so parse_reference
    // needs the header.
    ByteWriter payload;
    ByteWriter name;
    name.u32(0x0001A26F);
    write_field(payload, "NAME", name);

    SECTION("clear by default") {
        auto reader = reader_over(payload);
        const auto header = refr_header();
        const auto refr = record::parse_reference(header, reader,
                                                  {.localized = false, .tally = nullptr});
        REQUIRE(refr.has_value());
        CHECK_FALSE(refr->initially_disabled);
        CHECK_FALSE(refr->persistent);
        CHECK_FALSE(refr->deleted);
    }

    SECTION("set when the header says so") {
        const std::uint32_t flags =
            static_cast<std::uint32_t>(record::RecordFlag::initially_disabled) |
            static_cast<std::uint32_t>(record::RecordFlag::persistent) |
            static_cast<std::uint32_t>(record::RecordFlag::deleted);
        auto reader = reader_over(payload);
        const auto header = refr_header(flags);
        const auto refr = record::parse_reference(header, reader,
                                                  {.localized = false, .tally = nullptr});
        REQUIRE(refr.has_value());
        CHECK(refr->initially_disabled);
        CHECK(refr->persistent);
        CHECK(refr->deleted);
    }
}

TEST_CASE("REFR XESP and XTEL are absent optionals when the fields are absent",
          "[record][forms][refr]") {
    ByteWriter payload;
    ByteWriter name;
    name.u32(0x0001A26F);
    write_field(payload, "NAME", name);

    auto reader = reader_over(payload);
    const auto header = refr_header();
    const auto refr = record::parse_reference(header, reader,
                                              {.localized = false, .tally = nullptr});
    REQUIRE(refr.has_value());
    CHECK_FALSE(refr->enable_parent.has_value());
    CHECK_FALSE(refr->teleport.has_value());
    CHECK_FALSE(refr->primitive.has_value());
}

TEST_CASE("a truncated XTEL leaves the teleport unset rather than half-read",
          "[record][forms][refr]") {
    // XTEL is 32 bytes on all 1,722 vanilla references that have it. A partial
    // read is worse than none.
    ByteWriter payload;
    ByteWriter xtel;
    xtel.u32(0x0001A26F);
    xtel.raw(vec3(1.0F, 2.0F, 3.0F).span());
    write_field(payload, "XTEL", xtel); // 16 bytes, missing rotation and flags

    auto reader = reader_over(payload);
    const auto header = refr_header();
    const auto refr = record::parse_reference(header, reader,
                                              {.localized = false, .tally = nullptr});
    REQUIRE_FALSE(refr.has_value());
    CHECK(refr.error().kind == io::ErrorKind::truncated);
}

TEST_CASE("a failed record stops adding to the census", "[record][forms][refr]") {
    // After a failure, later fields are not tallied: they would pollute the
    // census of what converted records lack (this XRGD would join the 8,601
    // real ones).
    ByteWriter payload;
    ByteWriter data;
    data.f32(1.0F); // DATA wants 24 bytes
    write_field(payload, "DATA", data);
    ByteWriter xrgd;
    xrgd.u32(0);
    write_field(payload, "XRGD", xrgd); // undefined, and would be tallied
    ByteWriter xrds;
    xrds.f32(1.0F);
    xrds.f32(2.0F);
    write_field(payload, "XRDS", xrds); // defined and over-long, would be leftover
    ByteWriter edid;
    edid.raw("NoTerminatorHere");
    write_field(payload, "EDID", edid); // and would fail with a different kind

    Tally tally;
    auto reader = reader_over(payload);
    const auto header = refr_header();
    const auto refr = record::parse_reference(header, reader,
                                              {.localized = false, .tally = &tally});
    REQUIRE_FALSE(refr.has_value());
    CHECK(tally.unhandled().empty());
    CHECK(tally.leftover().empty());
    // The error is DATA's truncation, not the unterminated EDID after it.
    CHECK(refr.error().kind == io::ErrorKind::truncated);
}


// ---- DOOR / LIGH / WRLD ---------------------------------------------------

TEST_CASE("a DOOR carries its sounds and decodes its flag byte",
          "[record][forms][door]") {
    ByteWriter payload;
    ByteWriter edid;
    edid.zstring("WhiterunDoor");
    write_field(payload, "EDID", edid);
    ByteWriter fnam;
    fnam.u8(0x02); // automatic
    write_field(payload, "FNAM", fnam);
    ByteWriter snam;
    snam.u32(0x000C7A50);
    write_field(payload, "SNAM", snam);

    Tally tally;
    auto reader = reader_over(payload);
    const auto door = record::parse_door(reader, {.localized = false, .tally = &tally});
    REQUIRE(door.has_value());
    CHECK(door->editor_id == "WhiterunDoor");
    CHECK(door->has(record::Door::Flag::automatic));
    CHECK_FALSE(door->has(record::Door::Flag::sliding));
    CHECK(door->open_sound.value == 0x000C7A50u);
    CHECK(tally.leftover().empty());
}

TEST_CASE("a LIGH DATA one word short fails rather than shifting every field",
          "[record][forms][ligh]") {
    // DATA is 48 bytes on all 435 vanilla lights. Reading 44 as 48 would not
    // fail, just misassign values.
    ByteWriter payload;
    ByteWriter data;
    for (int i = 0; i < 11; ++i) {
        data.u32(0);
    }
    write_field(payload, "DATA", data);

    auto reader = reader_over(payload);
    const auto light = record::parse_light(reader, {.localized = false, .tally = nullptr});
    REQUIRE_FALSE(light.has_value());
    CHECK(light.error().kind == io::ErrorKind::truncated);
}

TEST_CASE("a LIGH keeps its colour word and flags intact", "[record][forms][ligh]") {
    ByteWriter payload;
    ByteWriter data;
    data.u32(0);           // time
    data.u32(512);         // radius
    data.u32(0xFF8040A0);  // colour
    data.u32(0x0009);      // dynamic | flicker
    for (int i = 0; i < 6; ++i) {
        data.f32(0.0F);
    }
    data.u32(25);   // value
    data.f32(1.0F); // weight
    write_field(payload, "DATA", data);
    ByteWriter fnam;
    fnam.f32(1.5F);
    write_field(payload, "FNAM", fnam);

    Tally tally;
    auto reader = reader_over(payload);
    const auto light = record::parse_light(reader, {.localized = false, .tally = &tally});
    REQUIRE(light.has_value());
    CHECK(light->radius == 512u);
    CHECK(light->colour == 0xFF8040A0u);
    CHECK(light->has(record::Light::Flag::dynamic));
    CHECK(light->has(record::Light::Flag::flicker));
    CHECK_FALSE(light->has(record::Light::Flag::spot_light));
    CHECK(light->fade == 1.5F);
    CHECK(tally.leftover().empty());
}

TEST_CASE("a WRLD carries its bounds and its optional heights",
          "[record][forms][wrld]") {
    ByteWriter payload;
    ByteWriter edid;
    edid.zstring("Tamriel");
    write_field(payload, "EDID", edid);
    ByteWriter nam0;
    nam0.f32(-131072.0F);
    nam0.f32(-131072.0F);
    write_field(payload, "NAM0", nam0);
    ByteWriter nam9;
    nam9.f32(131072.0F);
    nam9.f32(131072.0F);
    write_field(payload, "NAM9", nam9);
    ByteWriter dnam;
    dnam.f32(-27000.0F);
    dnam.f32(-14000.0F);
    write_field(payload, "DNAM", dnam);

    Tally tally;
    auto reader = reader_over(payload);
    const auto wrld = record::parse_worldspace(reader, {.localized = false, .tally = &tally});
    REQUIRE(wrld.has_value());
    CHECK(wrld->editor_id == "Tamriel");
    CHECK(wrld->min_x == -131072.0F);
    CHECK(wrld->max_y == 131072.0F);
    REQUIRE(wrld->default_land_height.has_value());
    CHECK(*wrld->default_land_height == -27000.0F);
    CHECK(*wrld->default_water_height == -14000.0F);
    CHECK_FALSE(wrld->centre_cell.has_value());
    CHECK(tally.leftover().empty());
}

TEST_CASE("WRLD WCTR is two int16 cell coordinates, not two floats",
          "[record][forms][wrld]") {
    // 4 bytes on all 19 that have it: two int16. As floats it would need 8;
    // as one int32 it would be wrong.
    ByteWriter payload;
    ByteWriter wctr;
    wctr.u16(static_cast<std::uint16_t>(-9));
    wctr.u16(3);
    write_field(payload, "WCTR", wctr);

    Tally tally;
    auto reader = reader_over(payload);
    const auto wrld = record::parse_worldspace(reader, {.localized = false, .tally = &tally});
    REQUIRE(wrld.has_value());
    REQUIRE(wrld->centre_cell.has_value());
    CHECK(wrld->centre_cell->first == -9);
    CHECK(wrld->centre_cell->second == 3);
    CHECK(tally.leftover().empty());
}

TEST_CASE("a truncated WRLD DNAM leaves both heights unset",
          "[record][forms][wrld]") {
    ByteWriter payload;
    ByteWriter dnam;
    dnam.f32(-27000.0F); // the water height is missing
    write_field(payload, "DNAM", dnam);

    auto reader = reader_over(payload);
    const auto wrld = record::parse_worldspace(reader, {.localized = false, .tally = nullptr});
    REQUIRE_FALSE(wrld.has_value());
    CHECK(wrld.error().kind == io::ErrorKind::truncated);
}

// ---- the type set ---------------------------------------------------------

TEST_CASE("the defined-type set and its lookup agree", "[record][forms]") {
    // 6 + 15 + 10 + 8. Pinned so a new definition missing from the list fails
    // here.
    CHECK(record::defined_types().size() == 39);
    for (const auto type : record::defined_types()) {
        CHECK(record::is_defined_type(type));
    }
    // No duplicates; they would be dispatched once and counted twice.
    std::set<std::uint32_t> seen;
    for (const auto type : record::defined_types()) {
        CHECK(seen.insert(type.value).second);
    }
    // Not defined yet.
    CHECK_FALSE(record::is_defined_type(FourCC{"INFO"}));
    CHECK_FALSE(record::is_defined_type(FourCC{"DIAL"}));
}

TEST_CASE("every defined type has a census definition", "[record][forms]") {
    // The census dispatches from a separate table. An empty payload must be
    // accepted by every definition, so this catches listed-but-unwired types.
    for (const auto type : record::defined_types()) {
        record::FormCensus census;
        record::RecordHeader header;
        header.type = type;
        const record::RecordContext ctx{.header = header, .groups = {}};
        io::SpanReader empty{std::span<const std::byte>{}, "empty"};
        census.on_record(ctx, empty);
        INFO("record type " << type.to_string());
        CHECK(census.total_failed() == 0);
        CHECK(census.total_parsed() == 1);
    }
}

// ---- the walk itself ------------------------------------------------------

TEST_CASE("a field header running past the payload is caught by the walk",
          "[record][forms]") {
    // A per-type parse must propagate structural errors, not return a partial
    // struct.
    ByteWriter payload;
    payload.tag("EDID");
    payload.u16(64); // claims 64 bytes; four follow
    payload.u32(0);

    auto reader = reader_over(payload);
    const auto stat = record::parse_static(reader, {.localized = false, .tally = nullptr});
    REQUIRE_FALSE(stat.has_value());
}

TEST_CASE("a payload with no fields at all is an empty record, not an error",
          "[record][forms]") {
    // Mod tools empty deleted records; they must not be errors.
    ByteWriter payload;
    auto reader = reader_over(payload);
    const auto stat = record::parse_static(reader, {.localized = false, .tally = nullptr});
    REQUIRE(stat.has_value());
    CHECK(stat->editor_id.empty());
    CHECK(stat->model.empty());
}

namespace {

/// Two triangles making a square, the second linked across its far edge.
test::NavSpec square_navmesh() {
    test::NavSpec nav;
    nav.world = 0x3C;
    nav.grid_x = -2;
    nav.grid_y = 5;
    nav.vertices = {{0, 0, 0}, {100, 0, 0}, {100, 100, 10}, {0, 100, 10}};
    nav.triangles = {
        {.vertices = {0, 1, 2}, .edges = {-1, -1, 1}, .flags = 0},
        {.vertices = {0, 2, 3}, .edges = {0, 0, -1}, .flags = 0x0402},
    };
    nav.links = {{.type = 0, .navmesh = 0x0001'2345, .triangle = 7}};
    nav.doors = {{.triangle = 1, .door = 0x0000'0ABC}};
    return nav;
}

} // namespace

TEST_CASE("NVNM decodes vertices, triangles, links and doors", "[record][forms][navmesh]") {
    const auto bytes = test::nvnm(square_navmesh());
    const auto nav = record::decode_nav_mesh_geometry(bytes.span());
    REQUIRE(nav.has_value());
    CHECK(nav->world.value == 0x3C);
    CHECK(nav->grid_x == -2);
    CHECK(nav->grid_y == 5);
    REQUIRE(nav->vertices.size() == 4);
    CHECK(nav->vertices[2] == record::Vec3{100, 100, 10});
    REQUIRE(nav->triangles.size() == 2);
    CHECK(nav->triangles[1].vertices == std::array<std::uint16_t, 3>{0, 2, 3});
    CHECK(nav->triangles[1].edges == std::array<std::int16_t, 3>{0, 0, -1});
    CHECK(nav->triangles[1].flags == 0x0402);
    REQUIRE(nav->edge_links.size() == 1);
    CHECK(nav->edge_links[0].navmesh.value == 0x0001'2345);
    CHECK(nav->edge_links[0].triangle == 7);
    REQUIRE(nav->doors.size() == 1);
    CHECK(nav->doors[0].door.value == 0x0ABC);

    test::NavSpec interior = square_navmesh();
    interior.world = 0;
    interior.cell = 0x0000'0D0D;
    const auto in = record::decode_nav_mesh_geometry(test::nvnm(interior).span());
    REQUIRE(in.has_value());
    CHECK(in->world.is_null());
    CHECK(in->cell.value == 0x0D0D);
}

TEST_CASE("NVNM with bad indices, another version or extra bytes fails",
          "[record][forms][navmesh]") {
    auto nav = square_navmesh();
    nav.triangles[0].vertices[2] = 4;
    CHECK(record::decode_nav_mesh_geometry(test::nvnm(nav).span()).error().kind ==
          io::ErrorKind::corrupt);

    nav = square_navmesh();
    nav.triangles[0].flags = 0x0001; // edge 0 a link, but -1
    CHECK(record::decode_nav_mesh_geometry(test::nvnm(nav).span()).error().kind ==
          io::ErrorKind::corrupt);

    nav = square_navmesh();
    nav.triangles[1].edges[0] = 2; // only one link
    CHECK(record::decode_nav_mesh_geometry(test::nvnm(nav).span()).error().kind ==
          io::ErrorKind::corrupt);

    auto bytes = test::nvnm(square_navmesh());
    bytes.patch_u32(0, 11);
    CHECK(record::decode_nav_mesh_geometry(bytes.span()).error().kind ==
          io::ErrorKind::unsupported);

    bytes = test::nvnm(square_navmesh());
    bytes.u8(0);
    CHECK(record::decode_nav_mesh_geometry(bytes.span()).error().kind == io::ErrorKind::bad_value);

    const auto whole = test::nvnm(square_navmesh());
    for (std::size_t cut = 0; cut < whole.bytes().size(); cut += 7) {
        CHECK_FALSE(record::decode_nav_mesh_geometry(whole.span().first(cut)).has_value());
    }
}
