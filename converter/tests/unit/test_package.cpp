// SPDX-License-Identifier: GPL-3.0-or-later
//
// PACK, FLST and CTDA: what an actor does and when.
#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/conditions.hpp"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/forms_game.hpp"

#include "../support/esm_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace bethconv;
using namespace bethconv::test;
using record::FourCC;

namespace {

class Tally final : public record::FieldTally {
public:
    void unhandled(FourCC, FourCC field, std::uint32_t) override { unhandled_.push_back(field.to_string()); }
    void leftover(FourCC, FourCC field, std::size_t) override { leftover_.push_back(field.to_string()); }
    std::vector<std::string> unhandled_;
    std::vector<std::string> leftover_;
};

ByteWriter text(const std::string& s) {
    ByteWriter w;
    w.zstring(s);
    return w;
}

ByteWriter u8(std::uint8_t v) {
    ByteWriter w;
    w.u8(v);
    return w;
}

ByteWriter u32(std::uint32_t v) {
    ByteWriter w;
    w.u32(v);
    return w;
}

ByteWriter triple(std::int32_t type, std::uint32_t value, std::int32_t third) {
    ByteWriter w;
    w.u32(static_cast<std::uint32_t>(type));
    w.u32(value);
    w.u32(static_cast<std::uint32_t>(third));
    return w;
}

/// A CTDA with a GLOB's FormID as its comparison value (flag 0x04).
ByteWriter ctda_global(std::uint8_t type, std::uint32_t global, std::uint16_t function,
                       std::uint32_t p1, std::uint32_t p2, std::uint32_t run_on,
                       std::uint32_t reference) {
    ByteWriter w;
    w.u8(type);
    w.u8(0);
    w.u8(0);
    w.u8(0);
    w.u32(global);
    w.u16(function);
    w.u16(0);
    w.u32(p1);
    w.u32(p2);
    w.u32(run_on);
    w.u32(reference);
    w.u32(0xFFFFFFFF);
    return w;
}

/// A CTDA: `type` byte, float comparison value, function, two parameters,
/// run on, reference, parameter 3.
ByteWriter ctda(std::uint8_t type, float value, std::uint16_t function, std::uint32_t p1,
                std::uint32_t p2 = 0, std::uint32_t run_on = 0, std::uint32_t reference = 0) {
    ByteWriter w;
    w.u8(type);
    w.u8(0);
    w.u8(0);
    w.u8(0);
    w.f32(value);
    w.u16(function);
    w.u16(0);
    w.u32(p1);
    w.u32(p2);
    w.u32(run_on);
    w.u32(reference);
    w.u32(0xFFFFFFFF);
    return w;
}

void pkdt(ByteWriter& p, std::uint32_t flags, std::uint8_t type) {
    ByteWriter w;
    w.u32(flags);
    w.u8(type);
    w.u8(0);
    w.u8(2);
    w.u8(0);
    w.u16(0x16D);
    w.u16(0);
    write_field(p, "PKDT", w);
}

void psdt(ByteWriter& p, std::int8_t hour, std::uint32_t minutes) {
    ByteWriter w;
    w.u8(0xFF); // any month
    w.u8(0xFF); // any day
    w.u8(0);
    w.u8(static_cast<std::uint8_t>(hour));
    w.u8(0xFF);
    w.u8('i'); // vanilla leaves garbage in the unused bytes
    w.u8('v');
    w.u8('e');
    w.u32(minutes);
    write_field(p, "PSDT", w);
}

void pkcu(ByteWriter& p, std::uint32_t inputs, std::uint32_t templ) {
    ByteWriter w;
    w.u32(inputs);
    w.u32(templ);
    w.u32(3);
    write_field(p, "PKCU", w);
}

void events(ByteWriter& p) {
    for (const char* marker : {"POBA", "POEA", "POCA"}) {
        write_field(p, marker, ByteWriter{});
        write_field(p, "INAM", u32(0));
        ByteWriter schr;
        for (int i = 0; i < 20; ++i) {
            schr.u8(0);
        }
        write_field(p, "SCHR", schr);
        ByteWriter pdto;
        pdto.u32(0);
        pdto.u32(0);
        write_field(p, "PDTO", pdto);
    }
}

} // namespace

TEST_CASE("PACK template: inputs by key, procedure tree, public names", "[record][forms][package]") {
    // Shaped like Skyrim.esm's Sandbox template (0001C254), cut down.
    ByteWriter p;
    write_field(p, "EDID", text("Sandbox"));
    pkdt(p, 0, record::Package::k_type_template);
    psdt(p, -1, 0);
    pkcu(p, 3, 0);
    write_field(p, "ANAM", text("Location"));
    write_field(p, "PLDT", triple(3, 0, 512)); // near editor location
    write_field(p, "ANAM", text("Bool"));
    write_field(p, "CNAM", u8(1));
    write_field(p, "ANAM", text("Float"));
    ByteWriter f;
    f.f32(50.0F);
    write_field(p, "CNAM", f);
    write_field(p, "UNAM", u8(0));
    write_field(p, "UNAM", u8(0x0E));
    write_field(p, "UNAM", u8(0x1D));
    write_field(p, "XNAM", u8(0x20));
    write_field(p, "ANAM", text("Sequence"));
    write_field(p, "CITC", u32(0));
    ByteWriter prcb;
    prcb.u32(2);
    prcb.u32(0);
    write_field(p, "PRCB", prcb);
    write_field(p, "ANAM", text("Procedure"));
    write_field(p, "CITC", u32(1));
    write_field(p, "CTDA", ctda(0x00, 1.0F, 612, 0x0E)); // GetNumericPackageData
    write_field(p, "PNAM", text("UnlockDoors"));
    write_field(p, "FNAM", u32(0));
    write_field(p, "PKC2", u8(0));
    write_field(p, "ANAM", text("Procedure"));
    write_field(p, "CITC", u32(0));
    write_field(p, "PNAM", text("Sandbox"));
    write_field(p, "FNAM", u32(1));
    write_field(p, "PKC2", u8(0));
    write_field(p, "PKC2", u8(0x1D));
    ByteWriter pfo2;
    pfo2.u32(0x40);
    pfo2.u32(0);
    pfo2.u16(0);
    pfo2.u16(0);
    pfo2.u8(1);
    pfo2.u8(0);
    pfo2.u8(0);
    pfo2.u8(0);
    write_field(p, "PFO2", pfo2);
    write_field(p, "UNAM", u8(0));
    write_field(p, "BNAM", text("Location"));
    write_field(p, "PNAM", u32(1));
    write_field(p, "UNAM", u8(0x0E));
    write_field(p, "BNAM", text("Unlock On Arrival?"));
    write_field(p, "PNAM", u32(1));
    events(p);

    Tally tally;
    io::SpanReader reader{p.span(), "pack"};
    const auto pack = record::parse_package(reader, {.localized = false, .tally = &tally});
    REQUIRE(pack.has_value());
    CHECK(tally.unhandled_.empty());
    CHECK(tally.leftover_.empty());
    CHECK(pack->editor_id == "Sandbox");
    CHECK(pack->type == record::Package::k_type_template);
    CHECK(pack->preferred_speed == 2);
    CHECK(pack->interrupt_flags == 0x16D);
    CHECK(pack->schedule.hour == -1);
    CHECK(pack->input_count == 3);
    REQUIRE(pack->inputs.size() == 3);
    CHECK(pack->inputs[0].type == "Location");
    REQUIRE(pack->inputs[0].location.has_value());
    CHECK(pack->inputs[0].location->type == 3);
    CHECK(pack->inputs[0].location->radius == 512);
    CHECK(pack->inputs[0].key == 0);
    CHECK(pack->inputs[1].value.size() == 1);
    CHECK(pack->inputs[1].key == 0x0E);
    CHECK(pack->inputs[2].key == 0x1D);
    CHECK(pack->marker == 0x20);
    REQUIRE(pack->branches.size() == 3);
    CHECK(pack->branches[0].type == "Sequence");
    CHECK(pack->branches[0].has_root);
    CHECK(pack->branches[0].branch_count == 2);
    CHECK(pack->branches[1].procedure == "UnlockDoors");
    CHECK(pack->branches[1].declared_condition_count == 1);
    REQUIRE(pack->branches[1].conditions.size() == 1);
    CHECK(pack->branches[1].conditions[0].function == 612);
    CHECK(pack->branches[2].procedure == "Sandbox");
    CHECK(pack->branches[2].success_completes);
    CHECK(pack->branches[2].input_keys == std::vector<std::uint8_t>{0, 0x1D});
    REQUIRE(pack->branches[2].flag_overrides.size() == 1);
    CHECK(pack->branches[2].flag_overrides[0].set_flags == 0x40);
    CHECK(pack->branches[2].flag_overrides[0].speed == 1);
    REQUIRE(pack->public_inputs.size() == 2);
    CHECK(pack->public_inputs[1].key == 0x0E);
    CHECK(pack->public_inputs[1].name == "Unlock On Arrival?");
    CHECK(pack->on_begin.present);
    CHECK(pack->on_end.present);
    CHECK(pack->on_change.present);
    CHECK(pack->on_change.legacy.size() == 20);
}

TEST_CASE("PACK instance: schedule, conditions, template, own values", "[record][forms][package]") {
    // Shaped like TutorialBlacksmithingAlvorSitPackage8x12 (0010538F).
    ByteWriter p;
    write_field(p, "EDID", text("AlvorSit8x12"));
    pkdt(p, 0x4, record::Package::k_type_package);
    psdt(p, 8, 720);
    write_field(p, "CTDA", ctda(0x00, 1.0F, 566, 0x0010'5385)); // GetIsCurrentPackage-like
    write_field(p, "QNAM", u32(0x0010'5385));
    write_field(p, "IDLF", u8(1));
    write_field(p, "IDLC", u8(1));
    ByteWriter idlt;
    idlt.f32(5.0F);
    write_field(p, "IDLT", idlt);
    write_field(p, "IDLA", u32(0x0001'2345));
    pkcu(p, 1, 0x000A'9277);
    write_field(p, "ANAM", text("SingleRef"));
    write_field(p, "PTDA", triple(0, 0x0010'538E, 0));
    write_field(p, "UNAM", u8(0x10));
    write_field(p, "XNAM", u8(0x11));
    events(p);

    Tally tally;
    io::SpanReader reader{p.span(), "pack"};
    const auto pack = record::parse_package(reader, {.localized = false, .tally = &tally});
    REQUIRE(pack.has_value());
    CHECK(tally.unhandled_.empty());
    CHECK(tally.leftover_.empty());
    CHECK(pack->has(record::Package::Flag::must_complete));
    CHECK(pack->schedule.hour == 8);
    CHECK(pack->schedule.duration == 720);
    REQUIRE(pack->conditions.size() == 1);
    CHECK(pack->owner_quest.value == 0x0010'5385);
    CHECK(pack->idle_timer == 5.0F);
    CHECK(pack->idles.size() == 1);
    CHECK(pack->template_package.value == 0x000A'9277);
    REQUIRE(pack->inputs.size() == 1);
    REQUIRE(pack->inputs[0].target.has_value());
    CHECK(pack->inputs[0].target->value == 0x0010'538E);
    CHECK(pack->inputs[0].key == 0x10);
    CHECK(pack->branches.empty());
}

TEST_CASE("PACK fields out of place fail instead of landing anywhere", "[record][forms][package]") {
    const auto fails = [](const ByteWriter& p) {
        io::SpanReader reader{p.span(), "pack"};
        return !record::parse_package(reader, {.localized = false}).has_value();
    };
    SECTION("a value before any input") {
        ByteWriter p;
        pkcu(p, 1, 0);
        write_field(p, "PLDT", triple(0, 0, 0));
        CHECK(fails(p));
    }
    SECTION("more keys than inputs") {
        ByteWriter p;
        pkcu(p, 1, 0);
        write_field(p, "ANAM", text("Bool"));
        write_field(p, "CNAM", u8(0));
        write_field(p, "UNAM", u8(0));
        write_field(p, "UNAM", u8(1));
        CHECK(fails(p));
    }
    SECTION("a condition string before any condition") {
        ByteWriter p;
        write_field(p, "CIS1", text("Variable01"));
        CHECK(fails(p));
    }
    SECTION("a truncated schedule") {
        ByteWriter p;
        ByteWriter w;
        w.u32(0);
        write_field(p, "PSDT", w);
        CHECK(fails(p));
    }
    SECTION("a branch field before any branch is tallied, not misread") {
        ByteWriter p;
        pkcu(p, 0, 0);
        write_field(p, "XNAM", u8(0));
        write_field(p, "PRCB", triple(0, 0, 0));
        Tally tally;
        io::SpanReader reader{p.span(), "pack"};
        CHECK(record::parse_package(reader, {.localized = false, .tally = &tally}).has_value());
        CHECK(tally.unhandled_ == std::vector<std::string>{"PRCB"});
    }
}

TEST_CASE("FLST: forms in order", "[record][forms][package]") {
    ByteWriter p;
    write_field(p, "EDID", text("DefaultPackages"));
    write_field(p, "LNAM", u32(0x0001'C254));
    write_field(p, "LNAM", u32(0x0001'C255));
    io::SpanReader reader{p.span(), "flst"};
    const auto list = record::parse_form_list(reader, {.localized = false});
    REQUIRE(list.has_value());
    CHECK(list->editor_id == "DefaultPackages");
    REQUIRE(list->forms.size() == 2);
    CHECK(list->forms[1].value == 0x0001'C255);
}

TEST_CASE("CTDA: decoded, with parameter kinds from the function table", "[record][conditions]") {
    std::vector<record::Condition> list;
    std::uint32_t declared = 0;
    std::optional<io::ParseError> failure;
    const auto feed = [&](const char* tag, const ByteWriter& payload) {
        ByteWriter f;
        write_field(f, tag, payload);
        io::SpanReader reader{f.span(), "ctda"};
        auto header = record::read_field_header(reader);
        REQUIRE(header.has_value());
        auto body = reader.subreader(header->data_size);
        REQUIRE(body.has_value());
        return record::read_ctda_field(list, declared, *header, *body, failure);
    };
    CHECK(feed("CITC", u32(3)));
    CHECK(declared == 3);
    // GetIsID 0x1234 == 1, ORed with the next.
    CHECK(feed("CTDA", ctda(0x01, 1.0F, 72, 0x1234)));
    // GetDistance to an alias (use aliases) < 300.
    CHECK(feed("CTDA", ctda(0x80 | 0x02, 300.0F, 1, 5)));
    // GetVMQuestVariable on a reference, comparing with a GLOB.
    CHECK(feed("CTDA", ctda_global(0x04, 0x0000'0ABC, 629, 0x0005'0000, 0, 2, 0x0001'4000)));
    CHECK(feed("CIS2", text("::Done_var")));
    CHECK_FALSE(feed("EDID", text("x")));
    REQUIRE_FALSE(failure.has_value());
    REQUIRE(list.size() == 3);

    CHECK(list[0].comparison() == 0);
    CHECK((list[0].type & record::Condition::k_or) != 0);
    CHECK(record::condition_param(list[0], 1) == record::ConditionParam::form);
    CHECK(record::condition_param_is_form(list[0], 1));
    CHECK_FALSE(record::condition_param_is_form(list[0], 2));

    CHECK(list[1].comparison() == 4);
    CHECK(list[1].value == 300.0F);
    CHECK(record::condition_param(list[1], 1) == record::ConditionParam::alias);
    CHECK_FALSE(record::condition_param_is_form(list[1], 1));

    // The comparison value is a GLOB's FormID, not a float.
    CHECK(list[2].value_global.value == 0x0000'0ABC);
    CHECK(list[2].run_on == 2);
    CHECK(list[2].reference.value == 0x0001'4000);
    CHECK(list[2].string2 == "::Done_var");
    CHECK(record::condition_param(list[2], 2) == record::ConditionParam::string);

    const auto* f = record::condition_function(359);
    REQUIRE(f != nullptr);
    CHECK(f->name == "GetInCurrentLoc");
    CHECK(record::condition_function(9999) == nullptr);
    record::Condition unknown;
    unknown.function = 9999;
    CHECK(record::condition_param(unknown, 1) == record::ConditionParam::none);
}
