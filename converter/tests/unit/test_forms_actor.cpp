// SPDX-License-Identifier: GPL-3.0-or-later
//
// RACE, ARMO, ARMA, OTFT and LVLI: what an actor looks like.
#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/forms_actor.hpp"

#include "../support/esm_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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

ByteWriter u32(std::uint32_t v) {
    ByteWriter w;
    w.u32(v);
    return w;
}

ByteWriter empty() {
    return ByteWriter{};
}

} // namespace

TEST_CASE("RACE: skeleton, body parts and behaviour per sex, by section", "[record][forms][race]") {
    ByteWriter p;
    write_field(p, "EDID", text("NordRace"));
    write_field(p, "WNAM", u32(0x000D64B2)); // SkinNaked
    ByteWriter bod2;
    bod2.u32(0x00000004);
    bod2.u32(2);
    write_field(p, "BOD2", bod2);
    ByteWriter data;
    for (int i = 0; i < 16; ++i) {
        data.u8(0);
    }
    data.f32(1.0f);   // male height
    data.f32(0.95f);  // female height
    data.f32(1.0f);
    data.f32(1.0f);
    data.u32(0x1);    // playable
    for (int i = 36; i < 164; ++i) {
        data.u8(0);
    }
    write_field(p, "DATA", data);
    write_field(p, "MNAM", empty());
    write_field(p, "ANAM", text("Actors\\Character\\Character Assets\\skeleton.nif"));
    ByteWriter modt;
    modt.u32(0);
    modt.u32(0);
    modt.u32(0);
    write_field(p, "MODT", modt);
    write_field(p, "FNAM", empty());
    write_field(p, "ANAM", text("Actors\\Character\\Character Assets Female\\skeleton_female.nif"));
    // Head data: an INDX the body section's reader must not take.
    write_field(p, "NAM0", empty());
    write_field(p, "MNAM", empty());
    write_field(p, "INDX", u32(0));
    write_field(p, "HEAD", u32(0x00051615));
    write_field(p, "NAM1", empty());
    write_field(p, "MNAM", empty());
    write_field(p, "INDX", u32(0));
    write_field(p, "MODL", text("Actors\\Character\\Character Assets\\MaleBody_1.nif"));
    write_field(p, "INDX", u32(3));
    write_field(p, "MODL", text("Actors\\Character\\Character Assets\\MaleFeet_1.nif"));
    write_field(p, "FNAM", empty());
    write_field(p, "INDX", u32(0));
    write_field(p, "MODL", text("Actors\\Character\\Character Assets\\FemaleBody_1.nif"));
    write_field(p, "NAM3", empty());
    write_field(p, "MNAM", empty());
    write_field(p, "MODL", text("Actors\\Character\\DefaultMale.hkx"));
    write_field(p, "FNAM", empty());
    write_field(p, "MODL", text("Actors\\Character\\DefaultFemale.hkx"));
    ByteWriter tint;
    tint.u16(7);
    write_field(p, "TINI", tint);

    Tally tally;
    io::SpanReader reader{p.span(), "race"};
    const auto race = record::parse_race(reader, {.localized = false, .tally = &tally});
    REQUIRE(race.has_value());
    CHECK(tally.unhandled_.empty());
    CHECK(tally.leftover_.empty());
    CHECK(race->editor_id == "NordRace");
    CHECK(race->skin.value == 0x000D64B2);
    CHECK(race->body.slots == 4);
    CHECK_THAT(race->height[1], Catch::Matchers::WithinAbs(0.95, 1e-6));
    CHECK(race->flags == 1);
    CHECK(race->sexes[0].skeleton == "Actors\\Character\\Character Assets\\skeleton.nif");
    CHECK(race->sexes[1].skeleton == "Actors\\Character\\Character Assets Female\\skeleton_female.nif");
    REQUIRE(race->sexes[0].body_parts.size() == 2);
    CHECK(race->sexes[0].body_parts[1].index == 3);
    CHECK(race->sexes[0].body_parts[1].model == "Actors\\Character\\Character Assets\\MaleFeet_1.nif");
    REQUIRE(race->sexes[1].body_parts.size() == 1);
    CHECK(race->sexes[0].head_parts.size() == 1);
    CHECK(race->sexes[0].behaviour == "Actors\\Character\\DefaultMale.hkx");
    CHECK(race->sexes[1].behaviour == "Actors\\Character\\DefaultFemale.hkx");
    // Not interpreted, not lost: the skeleton's MODT, the head INDX and TINI.
    CHECK(race->other.size() == 3);
}

TEST_CASE("ARMA: a model per sex and view, the race and further races", "[record][forms][arma]") {
    ByteWriter p;
    write_field(p, "EDID", text("NakedTorso"));
    ByteWriter bod2;
    bod2.u32(0x4);
    bod2.u32(2);
    write_field(p, "BOD2", bod2);
    write_field(p, "RNAM", u32(0x00013746));
    ByteWriter dnam;
    dnam.u8(0);
    dnam.u8(0);
    dnam.u8(2); // weight slider: _0/_1 variants
    dnam.u8(2);
    dnam.u16(0);
    dnam.u8(0);
    dnam.u8(0);
    dnam.f32(0.0f);
    write_field(p, "DNAM", dnam);
    write_field(p, "MOD2", text("Actors\\Character\\Character Assets\\MaleBody_1.nif"));
    write_field(p, "MOD3", text("Actors\\Character\\Character Assets\\FemaleBody_1.nif"));
    write_field(p, "MOD4", text("Actors\\Character\\Character Assets\\1stPersonMaleBody_1.nif"));
    write_field(p, "MODL", u32(0x00013741));
    write_field(p, "MODL", u32(0x00013742));

    Tally tally;
    io::SpanReader reader{p.span(), "arma"};
    const auto arma = record::parse_armor_addon(reader, {.localized = false, .tally = &tally});
    REQUIRE(arma.has_value());
    CHECK(tally.unhandled_.empty());
    CHECK(tally.leftover_.empty());
    CHECK(arma->race.value == 0x00013746);
    CHECK(arma->male_weight_slider == 2);
    CHECK(arma->male_model.path == "Actors\\Character\\Character Assets\\MaleBody_1.nif");
    CHECK(arma->female_model.path == "Actors\\Character\\Character Assets\\FemaleBody_1.nif");
    CHECK(arma->male_first_person.path.find("1stPerson") != std::string::npos);
    CHECK(arma->additional_races.size() == 2);
}

TEST_CASE("ARMO: its addons, slots and both BODT sizes", "[record][forms][armo]") {
    for (const bool short_bodt : {false, true}) {
        ByteWriter p;
        write_field(p, "EDID", text("ArmorIronCuirass"));
        ByteWriter bodt;
        bodt.u32(0x4);
        bodt.u32(0);
        if (!short_bodt) {
            bodt.u32(1);
        }
        write_field(p, "BODT", bodt);
        write_field(p, "RNAM", u32(0x00000019));
        write_field(p, "MODL", u32(0x00012E4A));
        write_field(p, "MOD2", text("Armor\\Iron\\Male\\CuirassGND.nif"));
        ByteWriter data;
        data.u32(125);
        data.f32(20.0f);
        write_field(p, "DATA", data);
        write_field(p, "DNAM", u32(2500));

        Tally tally;
        io::SpanReader reader{p.span(), "armo"};
        const auto armo = record::parse_armor(reader, {.localized = false, .tally = &tally});
        REQUIRE(armo.has_value());
        CHECK(tally.unhandled_.empty());
        CHECK(tally.leftover_.empty());
        CHECK(armo->body.slots == 4);
        CHECK(armo->body.armor_type == (short_bodt ? 0u : 1u));
        REQUIRE(armo->addons.size() == 1);
        CHECK(armo->addons[0].value == 0x00012E4A);
        CHECK(armo->value == 125);
        CHECK(armo->armor_rating == 2500);
    }
}

TEST_CASE("OTFT and LVLI: an outfit's items and a leveled list's entries", "[record][forms][otft]") {
    ByteWriter outfit;
    write_field(outfit, "EDID", text("FarmClothesOutfit01"));
    ByteWriter inam;
    inam.u32(0x0001BE1A);
    inam.u32(0x0010D2B4);
    write_field(outfit, "INAM", inam);
    io::SpanReader r1{outfit.span(), "otft"};
    const auto otft = record::parse_outfit(r1, {.localized = false, .tally = nullptr});
    REQUIRE(otft.has_value());
    CHECK(otft->items.size() == 2);

    ByteWriter list;
    write_field(list, "EDID", text("LItemFarmClothes"));
    ByteWriter lvld;
    lvld.u8(0);
    write_field(list, "LVLD", lvld);
    ByteWriter lvlf;
    lvlf.u8(1);
    write_field(list, "LVLF", lvlf);
    ByteWriter llct;
    llct.u8(1);
    write_field(list, "LLCT", llct);
    ByteWriter lvlo;
    lvlo.u16(1);
    lvlo.u16(0);
    lvlo.u32(0x0001BE1A);
    lvlo.u16(1);
    lvlo.u16(0);
    write_field(list, "LVLO", lvlo);
    Tally tally;
    io::SpanReader r2{list.span(), "lvli"};
    const auto lvli = record::parse_leveled_item(r2, {.localized = false, .tally = &tally});
    REQUIRE(lvli.has_value());
    CHECK(tally.unhandled_.empty());
    REQUIRE(lvli->entries.size() == 1);
    CHECK(lvli->entries[0].reference.value == 0x0001BE1A);
}
