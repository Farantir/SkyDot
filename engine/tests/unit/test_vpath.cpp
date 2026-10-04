// SPDX-License-Identifier: GPL-3.0-or-later
//
// Virtual path normalization. vpath.idx is keyed by what the converter's
// normalize_vpath writes, so the first six cases are the converter's own
// (converter/tests/unit/test_vpath.cpp), copied: both implementations stay
// pinned to the same behaviour. A path that normalizes differently here finds
// no asset, silently.
#include "assets/vpath.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <string_view>

using namespace skydot;

TEST_CASE("separators are normalized to forward slashes", "[vpath]") {
    CHECK(normalize_vpath("meshes\\clutter\\foo.nif") == "meshes/clutter/foo.nif");
    CHECK(normalize_vpath("meshes/clutter/foo.nif") == "meshes/clutter/foo.nif");
    // Mods mix separators, even within one path.
    CHECK(normalize_vpath("meshes\\clutter/foo.nif") == "meshes/clutter/foo.nif");
}

TEST_CASE("case is folded, ASCII only", "[vpath]") {
    CHECK(normalize_vpath("TEXTURES\\FOO\\BAR.DDS") == "textures/foo/bar.dds");
    CHECK(normalize_vpath("MiXeD/CaSe.NiF") == "mixed/case.nif");

    // Locale-independent: 'I' always becomes 'i', never Turkish dotless i.
    CHECK(normalize_vpath("MESHES/INTERFACE/I.NIF") == "meshes/interface/i.nif");

    // Non-ASCII bytes pass through unchanged, so they hash consistently.
    const std::string umlaut = "textures/f\xc3\x9c\x62\x61r.dds";
    CHECK(normalize_vpath("textures/f\xc3\x9c\x62\x61r.dds") == umlaut);
}

TEST_CASE("redundant separators collapse", "[vpath]") {
    CHECK(normalize_vpath("meshes//clutter///foo.nif") == "meshes/clutter/foo.nif");
    CHECK(normalize_vpath("meshes\\\\clutter\\foo.nif") == "meshes/clutter/foo.nif");
}

TEST_CASE("leading and trailing separators are dropped", "[vpath]") {
    CHECK(normalize_vpath("/meshes/foo.nif") == "meshes/foo.nif");
    CHECK(normalize_vpath("\\meshes\\foo.nif") == "meshes/foo.nif");
    CHECK(normalize_vpath("meshes/subdir/") == "meshes/subdir");
    CHECK(normalize_vpath("///") == "");
    CHECK(normalize_vpath("") == "");
}

TEST_CASE("'.' components are dropped and '..' is left alone", "[vpath]") {
    CHECK(normalize_vpath("./meshes/foo.nif") == "meshes/foo.nif");
    CHECK(normalize_vpath("meshes/./clutter/foo.nif") == "meshes/clutter/foo.nif");

    // '..' is kept: vpaths are map keys, and collapsing would merge distinct
    // entries.
    CHECK(normalize_vpath("meshes/../foo.nif") == "meshes/../foo.nif");

    // A leading dot in a file name is not a component.
    CHECK(normalize_vpath("meshes/.hidden") == "meshes/.hidden");
}

TEST_CASE("normalization is idempotent", "[vpath]") {
    const auto input = GENERATE(as<std::string_view>{}, "MESHES\\Foo\\\\Bar.NIF",
                                "./a/b/", "\\\\x\\y", "", "///", "a", "a/.hidden",
                                "meshes/../x", "textures/f\xc3\x9c.dds");
    const auto once = normalize_vpath(input);
    CHECK(normalize_vpath(once) == once);
}

TEST_CASE("only ASCII letters change case; UTF-8 passes through byte for byte", "[vpath]") {
    // "Ä" is C3 84: a lowercase of it (ä, C3 A4) would be a different key.
    CHECK(normalize_vpath("Meshes\\Ä\\Pot.nif") == "meshes/Ä/pot.nif");

    // Turkish İ (C4 B0) and ı (C4 B1) are left alone; the ASCII around them
    // is folded, and neither becomes an ASCII i.
    CHECK(normalize_vpath("TEXTURES/İSTANBUL/ISI.DDS") == "textures/İstanbul/isi.dds");
    CHECK(normalize_vpath("Textures/ıstanbul.dds") == "textures/ıstanbul.dds");
}

TEST_CASE("ascii_lower folds A-Z and nothing else", "[vpath]") {
    CHECK(ascii_lower('A') == 'a');
    CHECK(ascii_lower('Z') == 'z');
    CHECK(ascii_lower('a') == 'a');
    // The characters on either side of the ranges.
    CHECK(ascii_lower('@') == '@');
    CHECK(ascii_lower('[') == '[');
    CHECK(ascii_lower('0') == '0');
    CHECK(ascii_lower('\\') == '\\');
    // Bytes above 0x7F, signed char or not, are kept.
    CHECK(ascii_lower('\xc3') == '\xc3');
    CHECK(ascii_lower('\x84') == '\x84');

    CHECK(ascii_lower(std::string_view("Skyrim.ESM")) == "skyrim.esm");
    CHECK(ascii_lower(std::string_view("\xc3\x84RGER")) == "\xc3\x84rger");
    CHECK(ascii_lower(std::string_view()).empty());
}
