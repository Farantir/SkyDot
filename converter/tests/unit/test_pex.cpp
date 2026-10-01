// SPDX-License-Identifier: GPL-3.0-or-later
//
// PEX header and string table checks. Scripts are copied verbatim, so this
// parser is the only thing keeping truncated scripts out of the pack. The
// string table catches files whose header looks fine.
#include "bethconv/script/pex.hpp"

#include "../support/pex_builder.hpp"

#include <catch2/catch_test_macros.hpp>

using bethconv::testing::PexSpec;
using bethconv::testing::build_pex;
using bethconv::testing::truncate_pex;
using namespace bethconv::script;

TEST_CASE("a Skyrim script reports what it says about itself", "[pex]") {
    PexSpec spec;
    spec.source_file = "ConsoleUtil.psc";
    spec.username = "ryan_";
    spec.machine = "DESKTOP-O1MSN1T";
    spec.strings = {"ConsoleUtil", "", "GetState", "GotoState"};
    const auto file = build_pex(spec);

    const auto info = parse_pex(file, "consoleutil.pex");
    REQUIRE(info.has_value());
    CHECK(info->big_endian);
    CHECK(info->major_version == 3);
    CHECK(info->minor_version == 2);
    CHECK(info->game_id == 1);
    CHECK(info->game == PexGame::skyrim);
    CHECK(info->convertible());
    CHECK(info->source_file == "ConsoleUtil.psc");
    CHECK(info->username == "ryan_");
    CHECK(info->machine == "DESKTOP-O1MSN1T");
    CHECK(info->string_count == 4);
    CHECK(info->file_bytes == file.size());
}

TEST_CASE("the parser stops at the end of the string table", "[pex]") {
    // Everything after the string table stays unread; `parsed_bytes` must not
    // reach the end of the file.
    PexSpec spec;
    spec.trailing_bytes = 512;
    const auto file = build_pex(spec);

    const auto info = parse_pex(file, "fixture.pex");
    REQUIRE(info.has_value());
    CHECK(info->parsed_bytes < file.size());
    CHECK(info->unparsed_bytes() == 512);
}

TEST_CASE("a Fallout 4 script is recognized, not rejected", "[pex]") {
    // Little-endian magic and gameID 2 are reported as Fallout 4, not "bad
    // magic".
    PexSpec spec;
    spec.little_endian = true;
    spec.game_id = 2;
    const auto file = build_pex(spec);

    const auto info = parse_pex(file, "fallout.pex");
    REQUIRE(info.has_value());
    CHECK_FALSE(info->big_endian);
    CHECK(info->game == PexGame::fallout4);
    CHECK_FALSE(info->convertible());
}

TEST_CASE("an unknown gameID parses and is recorded verbatim", "[pex]") {
    PexSpec spec;
    spec.game_id = 77;
    const auto info = parse_pex(build_pex(spec), "odd.pex");
    REQUIRE(info.has_value());
    CHECK(info->game_id == 77);
    CHECK(info->game == PexGame::unknown);
    CHECK_FALSE(info->convertible());
}

TEST_CASE("a minor version bump is recorded, not refused", "[pex]") {
    // All 896 corpus files are 3.2, but a copied file is not refused over its
    // version. Fails if someone adds a version check.
    PexSpec spec;
    spec.minor = 9;
    const auto info = parse_pex(build_pex(spec), "future.pex");
    REQUIRE(info.has_value());
    CHECK(info->minor_version == 9);
    CHECK(info->convertible());
}

TEST_CASE("something that is not a script is refused", "[pex]") {
    std::vector<std::byte> junk(64, std::byte{0x00});
    const auto info = parse_pex(junk, "junk.pex");
    REQUIRE_FALSE(info.has_value());
    CHECK(info.error().kind == bethconv::io::ErrorKind::bad_magic);
}

TEST_CASE("an empty file is refused without reading anything", "[pex]") {
    const auto info = parse_pex({}, "empty.pex");
    REQUIRE_FALSE(info.has_value());
    CHECK(info.error().kind == bethconv::io::ErrorKind::truncated);
}

TEST_CASE("a header cut short is refused", "[pex]") {
    const auto file = build_pex(PexSpec{});
    for (const std::size_t keep : {4u, 8u, 14u, 20u, 30u}) {
        const auto info = parse_pex(truncate_pex(file, keep), "cut.pex");
        CAPTURE(keep);
        REQUIRE_FALSE(info.has_value());
        CHECK(info.error().kind == bethconv::io::ErrorKind::truncated);
    }
}

TEST_CASE("a string table that claims more entries than it holds is refused", "[pex]") {
    // Header is fine, but the file is not a valid script.
    PexSpec spec;
    spec.strings = {"One", "Two"};
    spec.declared_string_count = 900;
    spec.trailing_bytes = 8;

    const auto info = parse_pex(build_pex(spec), "lying-table.pex");
    REQUIRE_FALSE(info.has_value());
    CHECK(info.error().kind == bethconv::io::ErrorKind::truncated);
}

TEST_CASE("a table entry whose length runs off the end is refused", "[pex]") {
    auto file = build_pex(PexSpec{.strings = {"Fixture"}, .trailing_bytes = 0});
    // Cut inside the last string so the failure is the entry length, not the
    // count.
    const auto info = parse_pex(truncate_pex(file, file.size() - 3), "cut-entry.pex");
    REQUIRE_FALSE(info.has_value());
    CHECK(info.error().kind == bethconv::io::ErrorKind::truncated);
}

TEST_CASE("a script with an empty string table is legal", "[pex]") {
    PexSpec spec;
    spec.strings.clear();
    const auto info = parse_pex(build_pex(spec), "bare.pex");
    REQUIRE(info.has_value());
    CHECK(info->string_count == 0);
    CHECK(info->convertible());
}
