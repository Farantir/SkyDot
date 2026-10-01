// SPDX-License-Identifier: GPL-3.0-or-later
//
// Escaping of game strings for JSON.
//
// The glTF writer needs "no byte below 0x20" (Blender rejects the file
// otherwise); pack_writer needs valid UTF-8 (`nlohmann::json::dump()` throws).
// Everything else must pass through unchanged.
#include "bethconv/io/json_text.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using bethconv::io::json_text;
using bethconv::io::utf8_sequence_length;

TEST_CASE("a control byte becomes an escape", "[json_text]") {
    // 67 vanilla SE FaceGen meshes name their normal map `textures\<0x08>NOR`.
    CHECK(json_text(std::string("textures/") + '\x08' + "nor") == "textures/%08nor");
    CHECK(json_text(std::string(1, '\0')) == "%00");
    CHECK(json_text("\t\n\r") == "%09%0A%0D");
    CHECK(json_text(std::string(1, '\x1F')) == "%1F");
    // 0x20 is the first byte JSON allows unescaped.
    CHECK(json_text(" ") == " ");
}

TEST_CASE("well-formed UTF-8 is left exactly as it was", "[json_text]") {
    // Valid UTF-8 above ASCII passes through.
    CHECK(json_text("textures/caf\xC3\xA9/wood.dds") == "textures/caf\xC3\xA9/wood.dds");
    CHECK(json_text("\xE6\x97\xA5\xE6\x9C\xAC") == "\xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(json_text("\xF0\x9F\x97\xBF") == "\xF0\x9F\x97\xBF"); // U+1F5FF, four bytes
    CHECK(json_text("plain/ascii_name-1.dds") == "plain/ascii_name-1.dds");
    CHECK(json_text("") == "");
}

TEST_CASE("a byte that is not part of a UTF-8 sequence becomes an escape", "[json_text]") {
    // cp1252 `café` is a lone 0xE9, a lead byte without continuations.
    CHECK(json_text("caf\xE9") == "caf%E9");
    // A continuation byte with no lead.
    CHECK(json_text("\x80") == "%80");
    // Continuations cut off by the end of the string.
    CHECK(json_text("\xE6\x97") == "%E6%97");
    CHECK(json_text("\xF0") == "%F0");
    // Bytes after a bad one are unaffected.
    CHECK(json_text("a\xE9/b.dds") == "a%E9/b.dds");
}

TEST_CASE("the UTF-8 validator rejects what a naive decoder would accept",
          "[json_text]") {
    // Table 3-7 rows that exist to be rejected; a naive "count the leading
    // ones" decoder accepts them all.
    //
    // Overlong encodings:
    CHECK(utf8_sequence_length("\xC0\xAF", 0) == 0);         // overlong '/'
    CHECK(utf8_sequence_length("\xC1\xBF", 0) == 0);         // overlong 0x7F
    CHECK(utf8_sequence_length("\xE0\x80\xAF", 0) == 0);     // overlong, 3-byte
    CHECK(utf8_sequence_length("\xF0\x80\x80\xAF", 0) == 0); // overlong, 4-byte
    // UTF-16 surrogates:
    CHECK(utf8_sequence_length("\xED\xA0\x80", 0) == 0); // U+D800
    CHECK(utf8_sequence_length("\xED\xBF\xBF", 0) == 0); // U+DFFF
    CHECK(utf8_sequence_length("\xED\x9F\xBF", 0) == 3); // U+D7FF, the last legal one
    // Above U+10FFFF:
    CHECK(utf8_sequence_length("\xF4\x90\x80\x80", 0) == 0);
    CHECK(utf8_sequence_length("\xF5\x80\x80\x80", 0) == 0);
    CHECK(utf8_sequence_length("\xF4\x8F\xBF\xBF", 0) == 4); // U+10FFFF itself
    // 0xFE and 0xFF never occur.
    CHECK(utf8_sequence_length("\xFE", 0) == 0);
    CHECK(utf8_sequence_length("\xFF", 0) == 0);
    // At or past the end: 0, no read.
    CHECK(utf8_sequence_length("ab", 2) == 0);
    CHECK(utf8_sequence_length("", 0) == 0);
}

TEST_CASE("escaping is reversible, which is what makes it preservation",
          "[json_text]") {
    // Encoding is reversible: unescaping yields the original bytes.
    const auto unescape = [](std::string_view text) {
        const auto value = [](char c) {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            return c - 'A' + 10;
        };
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '%' && i + 2 < text.size()) {
                out.push_back(static_cast<char>(value(text[i + 1]) * 16 + value(text[i + 2])));
                i += 2;
            } else {
                out.push_back(text[i]);
            }
        }
        return out;
    };

    for (const std::string& original : {std::string("textures/") + '\x08' + "nor",
                                        std::string("caf\xE9/wood.dds"),
                                        std::string("textures/caf\xC3\xA9/wood.dds"),
                                        std::string("\xE6\x97")}) {
        CHECK(unescape(json_text(original)) == original);
    }
}

TEST_CASE("a literal percent is not an escape this produces", "[json_text]") {
    // json_text leaves '%' alone, so input that already contains an escape
    // does not round-trip. The glTF writer doubles percents for that reason.
    CHECK(json_text("a%08b") == "a%08b");
}
