// SPDX-License-Identifier: GPL-3.0-or-later
//
// .STRINGS layer tests. Real vanilla tables parse cleanly (`bethconv strings`),
// so these cover what they cannot:
//
//   1. directories that do not fit or point outside the data;
//   2. wrong framing: missing terminators, lengths past the end;
//   3. invalid UTF-8 (none in SE, 7 entries in LE);
//   4. the file naming rule, which is a convention with nothing to check
//      against.
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/strings.hpp"

#include "../support/strings_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace bethconv;
using bethconv::io::ErrorKind;
using bethconv::testing::build_string_table;
using bethconv::testing::StringEntry;
using bethconv::testing::StringTableSpec;
using record::StringKind;

namespace {

record::StringTable parsed(const StringTableSpec& spec) {
    const auto bytes = build_string_table(spec);
    auto table = record::StringTable::parse(bytes, "fixture", spec.kind);
    REQUIRE(table.has_value());
    return std::move(*table);
}

ErrorKind rejected(const StringTableSpec& spec) {
    const auto bytes = build_string_table(spec);
    auto table = record::StringTable::parse(bytes, "fixture", spec.kind);
    REQUIRE_FALSE(table.has_value());
    return table.error().kind;
}

/// The text a table holds for `id`, or "<none>" so a miss reads in the message.
std::string text_of(const record::StringTable& table, std::uint32_t id) {
    const auto* found = table.find(id);
    return found != nullptr ? *found : std::string("<none>");
}

} // namespace

// ---- framing --------------------------------------------------------------

TEST_CASE("a .strings entry is a bare NUL-terminated run", "[record][strings]") {
    const auto table = parsed({.kind = StringKind::plain,
                               .entries = {{.id = 1, .text = "The Ratway Vaults"},
                                           {.id = 2, .text = "Windhelm Stable Services"}}});
    CHECK(table.size() == 2);
    CHECK(text_of(table, 1) == "The Ratway Vaults");
    CHECK(text_of(table, 2) == "Windhelm Stable Services");
    CHECK(table.find(3) == nullptr);
}

TEST_CASE("a .dlstrings length prefix counts the terminator", "[record][strings]") {
    // The length includes the NUL. Reading `length` bytes of text would add a
    // trailing byte to every string.
    for (const auto kind : {StringKind::description, StringKind::dialogue}) {
        const auto table = parsed({.kind = kind, .entries = {{.id = 0x44, .text = "Hello"}}});
        CHECK(text_of(table, 0x44) == "Hello");
        CHECK(text_of(table, 0x44).size() == 5);
    }
}

TEST_CASE("a length-prefixed entry may be empty", "[record][strings]") {
    // Length 0 is no entry; `length - 1` would underflow.
    const auto table = parsed({.kind = StringKind::description,
                               .entries = {{.id = 7,
                                            .text = "",
                                            .override_length = true,
                                            .length = 0}}});
    CHECK(text_of(table, 7).empty());
}

TEST_CASE("trailing whitespace in a table entry is text, not padding",
          "[record][strings]") {
    // Trailing spaces in dialogue are meaningful, so length-prefixed entries
    // are read verbatim.
    const auto table =
        parsed({.kind = StringKind::dialogue, .entries = {{.id = 1, .text = "Wait...  "}}});
    CHECK(text_of(table, 1) == "Wait...  ");
}

// ---- the directory --------------------------------------------------------

TEST_CASE("identical strings are stored once and shared by several ids",
          "[record][strings]") {
    // 8,601 of 30,301 vanilla entries share an offset.
    const auto table = parsed({.kind = StringKind::plain,
                               .entries = {{.id = 10, .text = "Chest"},
                                           {.id = 20, .text = "", .alias_of = 0},
                                           {.id = 30, .text = "Barrel"}}});
    CHECK(table.size() == 3);
    CHECK(text_of(table, 10) == "Chest");
    CHECK(text_of(table, 20) == "Chest");
    CHECK(table.stats().entries == 3);
    CHECK(table.stats().distinct_strings == 2);
}

TEST_CASE("the directory is not in offset order", "[record][strings]") {
    // Vanilla directories are not sorted by offset. With entries pointing
    // backwards, each id must still find its own text.
    StringTableSpec spec{.kind = StringKind::plain,
                         .entries = {{.id = 1, .text = "first"},
                                     {.id = 2, .text = "second"},
                                     {.id = 3, .text = "third"}}};
    auto reordered = spec;
    reordered.entries = {spec.entries[2], spec.entries[1], spec.entries[0]};
    const auto table = parsed(reordered);
    CHECK(text_of(table, 1) == "first");
    CHECK(text_of(table, 2) == "second");
    CHECK(text_of(table, 3) == "third");
}

TEST_CASE("a directory that does not fit is refused, not clamped",
          "[record][strings]") {
    // A count of 1,000 in a two-entry file; clamping would read data as
    // directory entries.
    CHECK(rejected({.kind = StringKind::plain,
                    .entries = {{.id = 1, .text = "a"}, {.id = 2, .text = "b"}},
                    .override_count = true,
                    .count = 1000}) == ErrorKind::truncated);
}

TEST_CASE("an absurd entry count is refused before it is allocated for",
          "[record][strings]") {
    // The cap must trigger as too_large before any huge reservation.
    CHECK(rejected({.kind = StringKind::plain,
                    .entries = {{.id = 1, .text = "a"}},
                    .override_count = true,
                    .count = 0xFFFF'FFFFu}) == ErrorKind::too_large);
}

TEST_CASE("a data section shorter than the header claims is refused",
          "[record][strings]") {
    CHECK(rejected({.kind = StringKind::plain,
                    .entries = {{.id = 1, .text = "a"}},
                    .override_data_size = true,
                    .data_size = 4096}) == ErrorKind::truncated);
}

TEST_CASE("an entry offset outside the data section is refused",
          "[record][strings]") {
    CHECK(rejected({.kind = StringKind::plain,
                    .entries = {{.id = 1,
                                 .text = "a",
                                 .override_offset = true,
                                 .offset = 0xFFFF'0000u}}}) == ErrorKind::out_of_range);
}

TEST_CASE("a header cut off mid-field is refused", "[record][strings]") {
    CHECK(rejected({.kind = StringKind::plain,
                    .entries = {{.id = 1, .text = "a"}},
                    .truncate_to = 6}) == ErrorKind::truncated);
}

TEST_CASE("a .strings entry with no terminator is refused", "[record][strings]") {
    // Last entry runs to the end without a NUL; must fail, not shorten.
    CHECK(rejected({.kind = StringKind::plain,
                    .entries = {{.id = 1, .text = "unterminated", .omit_terminator = true}}}) ==
          ErrorKind::unterminated);
}

TEST_CASE("a length prefix reaching past the data section is refused",
          "[record][strings]") {
    CHECK(rejected({.kind = StringKind::description,
                    .entries = {{.id = 1,
                                 .text = "short",
                                 .override_length = true,
                                 .length = 5000}}}) == ErrorKind::truncated);
}

// ---- encoding -------------------------------------------------------------

TEST_CASE("well-formed UTF-8 survives untouched", "[record][strings]") {
    // Two-, three- and four-byte sequences: é, ’ and an astral-plane character.
    const std::string text = "caf\xC3\xA9 they\xE2\x80\x99re \xF0\x9F\x97\xBA";
    const auto table = parsed({.kind = StringKind::plain, .entries = {{.id = 1, .text = text}}});
    CHECK(text_of(table, 1) == text);
    CHECK(table.stats().repaired == 0);
}

TEST_CASE("ill-formed UTF-8 is repaired and counted", "[record][strings]") {
    // A lone 0xFF becomes U+FFFD and is counted.
    const auto table = parsed({.kind = StringKind::plain,
                               .entries = {{.id = 1, .text = "bad\xFF" "end"},
                                           {.id = 2, .text = "clean"}}});
    CHECK(text_of(table, 1) == "bad\xEF\xBF\xBD" "end");
    CHECK(text_of(table, 2) == "clean");
    CHECK(table.stats().repaired == 1);
}

TEST_CASE("the UTF-8 check rejects what a naive one accepts", "[record][strings]") {
    // Each has the right number of continuation bytes and is still invalid.
    const std::vector<std::string> bad = {
        "\xC0\xAF",         // overlong '/'
        "\xE0\x80\xAF",     // overlong, three-byte form
        "\xED\xA0\x80",     // U+D800, a surrogate
        "\xF4\x90\x80\x80", // U+110000, past the top of Unicode
        "\xC3",             // truncated two-byte sequence
    };
    for (const auto& text : bad) {
        const auto table =
            parsed({.kind = StringKind::plain, .entries = {{.id = 1, .text = text}}});
        INFO("input bytes: " << table.stats().entries);
        CHECK(table.stats().repaired == 1);
        CHECK(text_of(table, 1).find('\xEF') != std::string::npos);
    }
}

// ---- the naming rule ------------------------------------------------------

TEST_CASE("a plugin's tables are named after its stem, lowercased",
          "[record][strings]") {
    // Checked against all vanilla and CC plugins: the stem keeps punctuation and
    // leading underscores; only case changes.
    CHECK(record::string_table_path("Skyrim.esm", "english", StringKind::plain) ==
          "strings/skyrim_english.strings");
    CHECK(record::string_table_path("ccBGSSSE001-Fish.esm", "english", StringKind::plain) ==
          "strings/ccbgssse001-fish_english.strings");
    CHECK(record::string_table_path("_ResourcePack.esl", "english", StringKind::description) ==
          "strings/_resourcepack_english.dlstrings");
    CHECK(record::string_table_path("Update.esm", "GERMAN", StringKind::dialogue) ==
          "strings/update_german.ilstrings");
    // Only the last dot starts the extension ("My Mod v1.2.esp").
    CHECK(record::string_table_path("My Mod v1.2.esp", "english", StringKind::plain) ==
          "strings/my mod v1.2_english.strings");
}

TEST_CASE("the three tables partition the id space", "[record][strings]") {
    // Vanilla tables share no ids, so a lookup with the wrong kind misses.
    record::StringSource source;
    source.set(StringKind::plain,
               parsed({.kind = StringKind::plain, .entries = {{.id = 1, .text = "a name"}}}));
    source.set(StringKind::description,
               parsed({.kind = StringKind::description,
                       .entries = {{.id = 2, .text = "a description"}}}));

    CHECK(source.size() == 2);
    CHECK(source.find(1, StringKind::plain) != nullptr);
    CHECK(source.find(1, StringKind::description) == nullptr);
    CHECK(source.find(2, StringKind::description) != nullptr);
    CHECK(source.find(2, StringKind::plain) == nullptr);
    CHECK(source.find(3, StringKind::plain) == nullptr);
}

// ---- loading a plugin's set ----------------------------------------------

TEST_CASE("a missing table is normal; a broken one costs only itself",
          "[record][strings]") {
    // A missing table is fine. A present but broken one is a problem, but the
    // other two still load.
    const auto good = build_string_table(
        {.kind = StringKind::plain, .entries = {{.id = 1, .text = "Whiterun"}}});
    const auto broken = build_string_table({.kind = StringKind::description,
                                            .entries = {{.id = 2, .text = "x"}},
                                            .override_count = true,
                                            .count = 1000});

    std::vector<io::ParseError> problems;
    const auto source = record::load_string_source(
        [&](std::string_view vpath) -> std::optional<std::vector<std::byte>> {
            if (vpath == "strings/mod_english.strings") {
                return good;
            }
            if (vpath == "strings/mod_english.dlstrings") {
                return broken;
            }
            return std::nullopt; // this mod has no .ilstrings
        },
        "Mod.esp", "english", &problems);

    CHECK(problems.size() == 1);
    CHECK(problems.front().origin == "strings/mod_english.dlstrings");
    CHECK(source.find(1, StringKind::plain) != nullptr);
    CHECK(source.table(StringKind::description).empty());
    CHECK(source.table(StringKind::dialogue).empty());
    CHECK_FALSE(source.empty());
}

// ---- what a field does with all this -------------------------------------

TEST_CASE("a localized field resolves through the plugin's own tables",
          "[record][strings][forms]") {
    record::StringSource source;
    source.set(StringKind::plain,
               parsed({.kind = StringKind::plain, .entries = {{.id = 0x1234, .text = "Riverwood"}}}));

    const auto field = build_string_table({.kind = StringKind::plain, .entries = {}});
    (void)field;

    // A localized FULL: a 4-byte index.
    io::ByteWriter body;
    body.put(std::uint32_t{0x1234});
    const auto bytes = body.take();

    SECTION("resolved, and the index is kept as provenance") {
        const record::FormContext ctx{.localized = true, .strings = &source};
        io::SpanReader reader(bytes, "fixture");
        const auto value = record::read_lstring(reader, ctx);
        REQUIRE(value.has_value());
        CHECK(value->text == "Riverwood");
        CHECK(value->id == 0x1234);
        CHECK_FALSE(value->is_id);
        CHECK(value->from_table());
    }

    SECTION("no tables attached: the index survives as an index") {
        const record::FormContext ctx{.localized = true, .strings = nullptr};
        io::SpanReader reader(bytes, "fixture");
        const auto value = record::read_lstring(reader, ctx);
        REQUIRE(value.has_value());
        CHECK(value->is_id);
        CHECK(value->id == 0x1234);
        CHECK(value->to_string() == "#00001234");
    }

    SECTION("tables attached but the index is absent: still an index") {
        record::StringSource other;
        other.set(StringKind::plain,
                  parsed({.kind = StringKind::plain, .entries = {{.id = 9, .text = "elsewhere"}}}));
        const record::FormContext ctx{.localized = true, .strings = &other};
        io::SpanReader reader(bytes, "fixture");
        const auto value = record::read_lstring(reader, ctx);
        REQUIRE(value.has_value());
        CHECK(value->is_id);
        CHECK(value->id == 0x1234);
    }
}

TEST_CASE("index 0 is 'no string', not a lookup", "[record][strings][forms]") {
    // Index 0 means no name; looking it up would flag most records as
    // unresolved.
    record::StringSource source;
    source.set(StringKind::plain,
               parsed({.kind = StringKind::plain, .entries = {{.id = 1, .text = "something"}}}));

    io::ByteWriter body;
    body.put(std::uint32_t{0});
    const auto bytes = body.take();

    const record::FormContext ctx{.localized = true, .strings = &source};
    io::SpanReader reader(bytes, "fixture");
    const auto value = record::read_lstring(reader, ctx);
    REQUIRE(value.has_value());
    CHECK(value->empty());
    CHECK_FALSE(value->is_id);
    CHECK_FALSE(value->from_table());
}

TEST_CASE("a non-localized plugin's string field is still text",
          "[record][strings][forms]") {
    // Inline FULL text: no vanilla master uses it (all are localized), so this
    // is the only coverage.
    record::StringSource source;
    io::ByteWriter body;
    for (const char c : std::string("Riverwood")) {
        body.put(static_cast<std::uint8_t>(c));
    }
    body.put(static_cast<std::uint8_t>(0));
    const auto bytes = body.take();

    const record::FormContext ctx{.localized = false, .strings = &source};
    io::SpanReader reader(bytes, "fixture");
    const auto value = record::read_lstring(reader, ctx);
    REQUIRE(value.has_value());
    CHECK(value->text == "Riverwood");
    CHECK(value->id == 0);
    CHECK_FALSE(value->is_id);
    CHECK_FALSE(value->from_table());
}
