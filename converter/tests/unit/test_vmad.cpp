// SPDX-License-Identifier: GPL-3.0-or-later
//
// VMAD decoding. Real data is covered by `bethconv forms` and the corpus
// census (every VMAD in three installs and the FUS list, none failed or
// leftover); these cover both object layouts, the version without status
// bytes, fragment data left unread and the error paths.
#include "bethconv/record/forms.hpp"
#include "bethconv/record/vmad.hpp"

#include "../support/esm_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace bethconv;
using namespace bethconv::test;
using record::ScriptPropertyType;

namespace {

void wstring(ByteWriter& w, std::string_view s) {
    w.u16(static_cast<std::uint16_t>(s.size()));
    w.raw(s);
}

void header(ByteWriter& w, std::int16_t version, std::int16_t format, std::uint16_t scripts) {
    w.u16(static_cast<std::uint16_t>(version));
    w.u16(static_cast<std::uint16_t>(format));
    w.u16(scripts);
}

/// A property header in the version 4/5 layout.
void property(ByteWriter& w, std::string_view name, std::uint8_t type) {
    wstring(w, name);
    w.u8(type);
    w.u8(1); // edited
}

io::SpanReader reader_over(const ByteWriter& w) {
    return io::SpanReader{w.span(), "test-fixture"};
}

} // namespace

TEST_CASE("VMAD version 5 decodes every property type", "[record][vmad]") {
    ByteWriter w;
    header(w, 5, 2, 1);
    wstring(w, "defaultLeverScript");
    w.u8(0);
    w.u16(10);
    property(w, "Door", 1);
    w.u16(0);
    w.u16(0xFFFF); // alias -1
    w.u32(0x0001A2B3);
    property(w, "Message", 2);
    wstring(w, "Pull");
    property(w, "Count", 3);
    w.u32(static_cast<std::uint32_t>(-7));
    property(w, "Delay", 4);
    w.f32(1.5F);
    property(w, "Once", 5);
    w.u8(1);
    property(w, "Targets", 11);
    w.u32(2);
    w.u16(0);
    w.u16(3); // alias 3 of the quest
    w.u32(0x00000D62);
    w.u16(0);
    w.u16(0xFFFF);
    w.u32(0x00000014);
    property(w, "Names", 12);
    w.u32(2);
    wstring(w, "a");
    wstring(w, "");
    property(w, "Stages", 13);
    w.u32(1);
    w.u32(10);
    property(w, "Weights", 14);
    w.u32(0);
    property(w, "Flags", 15);
    w.u32(3);
    w.u8(0);
    w.u8(2); // any non-zero byte is true
    w.u8(1);

    auto body = reader_over(w);
    const auto data = record::read_script_data(body);
    REQUIRE(data.has_value());
    CHECK(body.at_end());
    CHECK(data->version == 5);
    REQUIRE(data->scripts.size() == 1);
    const auto& script = data->scripts[0];
    CHECK(script.name == "defaultLeverScript");
    REQUIRE(script.properties.size() == 10);

    const auto& door = script.properties[0];
    CHECK(door.type == ScriptPropertyType::object);
    CHECK(door.status == 1);
    REQUIRE(door.objects.size() == 1);
    CHECK(door.objects[0].form == record::FormId{0x0001A2B3});
    CHECK(door.objects[0].alias == -1);

    CHECK(script.properties[1].strings == std::vector<std::string>{"Pull"});
    CHECK(script.properties[2].integers == std::vector<std::int32_t>{-7});
    CHECK(script.properties[3].floats == std::vector<float>{1.5F});
    CHECK(script.properties[4].integers == std::vector<std::int32_t>{1});

    const auto& targets = script.properties[5];
    CHECK(record::is_array(targets.type));
    REQUIRE(targets.objects.size() == 2);
    CHECK(targets.objects[0].alias == 3);
    CHECK(targets.objects[1].form == record::FormId{0x14});

    CHECK(script.properties[6].strings == std::vector<std::string>{"a", ""});
    CHECK(script.properties[7].integers == std::vector<std::int32_t>{10});
    CHECK(script.properties[8].floats.empty());
    CHECK(script.properties[9].integers == std::vector<std::int32_t>{0, 1, 1});
}

TEST_CASE("VMAD version 3 has no status bytes and object format 1 puts the FormID first",
          "[record][vmad]") {
    ByteWriter w;
    header(w, 3, 1, 2);
    wstring(w, "First");
    w.u16(1);
    wstring(w, "Target");
    w.u8(1);
    w.u32(0x00012345);
    w.u16(0xFFFF);
    w.u16(0);
    wstring(w, "Second");
    w.u16(0);

    auto body = reader_over(w);
    const auto data = record::read_script_data(body);
    REQUIRE(data.has_value());
    CHECK(body.at_end());
    REQUIRE(data->scripts.size() == 2);
    CHECK(data->scripts[0].status == 0);
    REQUIRE(data->scripts[0].properties.size() == 1);
    CHECK(data->scripts[0].properties[0].objects[0].form == record::FormId{0x00012345});
    CHECK(data->scripts[0].properties[0].objects[0].alias == -1);
    CHECK(data->scripts[1].name == "Second");
}

TEST_CASE("VMAD leaves fragment data after the scripts unread", "[record][vmad]") {
    ByteWriter w;
    header(w, 5, 2, 0);
    w.u8(2); // a quest's fragment header would start here
    w.u16(0);

    auto body = reader_over(w);
    const auto data = record::read_script_data(body);
    REQUIRE(data.has_value());
    CHECK(data->empty());
    CHECK(body.remaining() == 3);
}

TEST_CASE("a VMAD property of type 0 has no value", "[record][vmad]") {
    ByteWriter w;
    header(w, 5, 2, 1);
    wstring(w, "S");
    w.u8(0);
    w.u16(2);
    property(w, "Pages", 0);
    property(w, "Count", 3);
    w.u32(4);
    auto body = reader_over(w);
    const auto data = record::read_script_data(body);
    REQUIRE(data.has_value());
    REQUIRE(data->scripts[0].properties.size() == 2);
    CHECK(data->scripts[0].properties[0].type == ScriptPropertyType::none);
    CHECK(data->scripts[0].properties[1].integers == std::vector<std::int32_t>{4});
    CHECK(body.at_end());
}

TEST_CASE("VMAD errors are reported, not read past", "[record][vmad]") {
    SECTION("an unknown version") {
        ByteWriter w;
        header(w, 1, 2, 0);
        auto body = reader_over(w);
        const auto data = record::read_script_data(body);
        REQUIRE_FALSE(data.has_value());
        CHECK(data.error().kind == io::ErrorKind::unsupported);
    }
    SECTION("an unknown object format") {
        ByteWriter w;
        header(w, 5, 3, 0);
        auto body = reader_over(w);
        const auto data = record::read_script_data(body);
        REQUIRE_FALSE(data.has_value());
        CHECK(data.error().kind == io::ErrorKind::unsupported);
    }
    SECTION("an unknown property type") {
        ByteWriter w;
        header(w, 5, 2, 1);
        wstring(w, "S");
        w.u8(0);
        w.u16(1);
        property(w, "P", 6);
        auto body = reader_over(w);
        const auto data = record::read_script_data(body);
        REQUIRE_FALSE(data.has_value());
        CHECK(data.error().kind == io::ErrorKind::bad_value);
    }
    SECTION("an array count larger than the field") {
        ByteWriter w;
        header(w, 5, 2, 1);
        wstring(w, "S");
        w.u8(0);
        w.u16(1);
        property(w, "P", 13);
        w.u32(0xFFFF'FFFFu);
        auto body = reader_over(w);
        const auto data = record::read_script_data(body);
        REQUIRE_FALSE(data.has_value());
        CHECK(data.error().kind == io::ErrorKind::truncated);
    }
    SECTION("more scripts declared than present") {
        ByteWriter w;
        header(w, 5, 2, 3);
        wstring(w, "S");
        w.u8(0);
        w.u16(0);
        auto body = reader_over(w);
        const auto data = record::read_script_data(body);
        REQUIRE_FALSE(data.has_value());
        CHECK(data.error().kind == io::ErrorKind::truncated);
    }
    SECTION("a string running past the end") {
        ByteWriter w;
        header(w, 5, 2, 1);
        w.u16(40);
        w.raw("short");
        auto body = reader_over(w);
        CHECK_FALSE(record::read_script_data(body).has_value());
    }
}

TEST_CASE("REFR reads its scripts and lock", "[record][vmad][refr]") {
    ByteWriter payload;
    ByteWriter vmad;
    header(vmad, 5, 2, 1);
    wstring(vmad, "TrapLever");
    vmad.u8(0);
    vmad.u16(0);
    write_field(payload, "VMAD", vmad);
    ByteWriter xloc;
    xloc.u8(255); // needs a key
    xloc.u8(0);
    xloc.u16(0);
    xloc.u32(0x00012345);
    xloc.u8(0x04);
    xloc.u8(0);
    xloc.u16(0);
    xloc.u32(0);
    xloc.u32(0);
    write_field(payload, "XLOC", xloc);

    record::RecordHeader head;
    head.type = record::FourCC{"REFR"};
    auto body = reader_over(payload);
    const auto refr = record::parse_reference(head, body, {.localized = false, .tally = nullptr});
    REQUIRE(refr.has_value());
    REQUIRE(refr->scripts.scripts.size() == 1);
    CHECK(refr->scripts.scripts[0].name == "TrapLever");
    REQUIRE(refr->lock.has_value());
    CHECK(refr->lock->level == 255);
    CHECK(refr->lock->key == record::FormId{0x00012345});
    CHECK(refr->lock->flags == 0x04);
}
