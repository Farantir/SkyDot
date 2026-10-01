// SPDX-License-Identifier: GPL-3.0-or-later
//
// Virtual path normalization. Every archive lookup uses it, and bugs show up as
// silently missing assets rather than crashes.
#include "bethconv/archive/vpath.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <string_view>

using namespace bethconv::archive;

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
    CHECK(is_normalized(once));
}

TEST_CASE("is_normalized rejects anything that would change", "[vpath]") {
    CHECK(is_normalized("meshes/clutter/foo.nif"));
    CHECK(is_normalized(""));
    CHECK_FALSE(is_normalized("Meshes/foo.nif"));
    CHECK_FALSE(is_normalized("meshes\\foo.nif"));
    CHECK_FALSE(is_normalized("/meshes/foo.nif"));
    CHECK_FALSE(is_normalized("meshes//foo.nif"));
}

TEST_CASE("vpath_extension takes the last dot in the filename", "[vpath]") {
    CHECK(vpath_extension("textures/foo/bar.dds") == "dds");
    CHECK(vpath_extension("meshes/foo.nif.bak") == "bak");
    CHECK(vpath_extension("meshes/foo") == "");
    CHECK(vpath_extension("") == "");

    // A dot in a directory name is not an extension (FaceGen uses
    // `facegendata/facetint/skyrim.esm/<formid>.dds`).
    CHECK(vpath_extension("facegendata/facetint/skyrim.esm/0004d8d5") == "");
    CHECK(vpath_extension("facegendata/facetint/skyrim.esm/0004d8d5.dds") == "dds");

    // A trailing dot gives an empty extension.
    CHECK(vpath_extension("meshes/foo.") == "");
}

TEST_CASE("vpath_top_folder is the routing key for the asset phases", "[vpath]") {
    CHECK(vpath_top_folder("textures/foo/bar.dds") == "textures");
    CHECK(vpath_top_folder("meshes/foo.nif") == "meshes");
    CHECK(vpath_top_folder("readme.txt") == "");
    CHECK(vpath_top_folder("") == "");
}

TEST_CASE("is_safe_relative rejects paths that escape an output directory", "[vpath]") {
    CHECK(is_safe_relative("meshes/clutter/foo.nif"));
    CHECK(is_safe_relative("textures/.hidden/a.dds"));

    // Archive names are untrusted; normalization keeps "..", so this check
    // is what stops `out / vpath` from escaping.
    CHECK_FALSE(is_safe_relative(normalize_vpath("..\\..\\etc\\passwd")));
    CHECK_FALSE(is_safe_relative("meshes/../../x.nif"));
    CHECK_FALSE(is_safe_relative("/etc/passwd"));
    CHECK_FALSE(is_safe_relative("c:/windows/x.dds"));
    CHECK_FALSE(is_safe_relative("meshes\\foo.nif"));
    CHECK_FALSE(is_safe_relative("meshes//foo.nif"));
    CHECK_FALSE(is_safe_relative("meshes/./foo.nif"));
    CHECK_FALSE(is_safe_relative(""));
}
