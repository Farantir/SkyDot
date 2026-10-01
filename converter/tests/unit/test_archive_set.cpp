// SPDX-License-Identifier: GPL-3.0-or-later
//
// ArchiveSet: precedence, conflicts, read dispatch, version-to-codec. Mistakes
// here silently convert the wrong version of an asset.
#include "bethconv/archive/archive_set.hpp"
#include "bethconv/archive/vpath.hpp"

#include "../support/bsa_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace bethconv::archive;
using bethconv::test::BsaEntry;
using bethconv::test::TempDir;
using bethconv::test::write_bsa;

namespace {

/// The bytes of a successful read, as a string, for readable comparisons.
std::string read_text(const ArchiveSet& set, std::string_view vpath) {
    auto bytes = set.read(vpath);
    REQUIRE(bytes.has_value());
    std::string out;
    out.reserve(bytes->size());
    for (const auto b : *bytes) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

} // namespace

TEST_CASE("a loose mount resolves paths however they are spelled", "[archive]") {
    const TempDir dir;
    dir.write("meshes/clutter/foo.nif", "loose-foo");

    ArchiveSet set;
    REQUIRE(set.mount_loose(dir.path(), 0) == 1);
    CHECK(set.unique_paths() == 1);

    // One file, four spellings.
    for (const auto* spelling : {"meshes/clutter/foo.nif", "Meshes\\Clutter\\Foo.NIF",
                                 "/meshes//clutter/foo.nif", "./meshes/clutter/foo.nif"}) {
        INFO("spelling: " << spelling);
        const auto resolution = set.resolve(spelling);
        REQUIRE(resolution.has_value());
        CHECK(resolution->vpath == "meshes/clutter/foo.nif");
        CHECK_FALSE(resolution->is_conflict());
        CHECK(read_text(set, spelling) == "loose-foo");
    }
}

TEST_CASE("an unprovided path resolves to nothing and reads as an error", "[archive]") {
    const TempDir dir;
    dir.write("meshes/foo.nif", "x");

    ArchiveSet set;
    REQUIRE(set.mount_loose(dir.path(), 0).has_value());

    CHECK_FALSE(set.resolve("meshes/bar.nif").has_value());

    const auto bytes = set.read("meshes/bar.nif");
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().kind == bethconv::io::ErrorKind::bad_value);
}

TEST_CASE("higher priority wins regardless of mount order", "[archive][precedence]") {
    const TempDir low;
    const TempDir high;
    low.write("meshes/foo.nif", "low");
    high.write("meshes/foo.nif", "high");

    SECTION("high mounted last") {
        ArchiveSet set;
        REQUIRE(set.mount_loose(low.path(), 0).has_value());
        REQUIRE(set.mount_loose(high.path(), 10).has_value());
        CHECK(read_text(set, "meshes/foo.nif") == "high");
    }

    SECTION("high mounted first") {
        ArchiveSet set;
        REQUIRE(set.mount_loose(high.path(), 10).has_value());
        REQUIRE(set.mount_loose(low.path(), 0).has_value());
        CHECK(read_text(set, "meshes/foo.nif") == "high");
    }
}

TEST_CASE("at equal priority the later mount wins", "[archive][precedence]") {
    const TempDir first;
    const TempDir second;
    first.write("meshes/foo.nif", "first");
    second.write("meshes/foo.nif", "second");

    ArchiveSet set;
    REQUIRE(set.mount_loose(first.path(), 0).has_value());
    REQUIRE(set.mount_loose(second.path(), 0).has_value());

    CHECK(read_text(set, "meshes/foo.nif") == "second");

    const auto resolution = set.resolve("meshes/foo.nif");
    REQUIRE(resolution.has_value());
    CHECK(resolution->is_conflict());
    REQUIRE(resolution->shadowed.size() == 1);
    CHECK(set.sources()[resolution->winner].path == second.path());
    CHECK(set.sources()[resolution->shadowed.front()].path == first.path());
}

TEST_CASE("at equal priority a loose file beats an archived one", "[archive][precedence]") {
    // The game's rule that makes loose-file overrides work; must hold in both
    // mount orders.
    const TempDir dir;
    const auto bsa_path = dir / "test.bsa";
    write_bsa(bsa_path, {BsaEntry{"meshes/foo.nif", "archived"}});

    const TempDir loose;
    loose.write("meshes/foo.nif", "loose");

    SECTION("archive mounted last") {
        ArchiveSet set;
        REQUIRE(set.mount_loose(loose.path(), 0).has_value());
        REQUIRE(set.mount_archive(bsa_path, 0).has_value());
        CHECK(read_text(set, "meshes/foo.nif") == "loose");
    }

    SECTION("loose mounted last") {
        ArchiveSet set;
        REQUIRE(set.mount_archive(bsa_path, 0).has_value());
        REQUIRE(set.mount_loose(loose.path(), 0).has_value());
        CHECK(read_text(set, "meshes/foo.nif") == "loose");
    }

    SECTION("but priority still outranks kind") {
        ArchiveSet set;
        REQUIRE(set.mount_loose(loose.path(), 0).has_value());
        REQUIRE(set.mount_archive(bsa_path, 5).has_value());
        CHECK(read_text(set, "meshes/foo.nif") == "archived");
    }
}

TEST_CASE("a three-way conflict is ordered best-first", "[archive][precedence]") {
    const TempDir a;
    const TempDir b;
    const TempDir c;
    a.write("meshes/foo.nif", "a");
    b.write("meshes/foo.nif", "b");
    c.write("meshes/foo.nif", "c");

    ArchiveSet set;
    REQUIRE(set.mount_loose(a.path(), 0).has_value());   // source 0
    REQUIRE(set.mount_loose(b.path(), 9).has_value());   // source 1, highest
    REQUIRE(set.mount_loose(c.path(), 0).has_value());   // source 2, beats a on order

    const auto resolution = set.resolve("meshes/foo.nif");
    REQUIRE(resolution.has_value());
    CHECK(resolution->winner == 1);
    REQUIRE(resolution->shadowed.size() == 2);
    CHECK(resolution->shadowed[0] == 2);
    CHECK(resolution->shadowed[1] == 0);
}

TEST_CASE("conflicts() reports every contested path and nothing else", "[archive]") {
    const TempDir a;
    const TempDir b;
    a.write("meshes/shared.nif", "a");
    a.write("meshes/only_a.nif", "a");
    b.write("meshes/shared.nif", "b");
    b.write("meshes/only_b.nif", "b");

    ArchiveSet set;
    REQUIRE(set.mount_loose(a.path(), 0) == 2);
    REQUIRE(set.mount_loose(b.path(), 0) == 2);

    CHECK(set.unique_paths() == 3);

    const auto conflicts = set.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts.front().vpath == "meshes/shared.nif");

    std::vector<std::string> visited;
    set.for_each([&](const Resolution& r) { visited.push_back(r.vpath); });
    std::ranges::sort(visited);
    CHECK(visited == std::vector<std::string>{"meshes/only_a.nif", "meshes/only_b.nif",
                                              "meshes/shared.nif"});
}

TEST_CASE("archive entries are reachable through normalized paths", "[archive]") {
    const TempDir dir;
    const auto bsa_path = dir / "test.bsa";
    // Backslashes in, forward slashes out.
    write_bsa(bsa_path, {BsaEntry{"Meshes\\Clutter\\Foo.nif", "archived-foo"},
                         BsaEntry{"textures/foo/bar.dds", "archived-bar"}});

    ArchiveSet set;
    const auto mounted = set.mount_archive(bsa_path, 0);
    REQUIRE(mounted.has_value());
    CHECK(*mounted == 2);
    REQUIRE(set.sources().size() == 1);
    CHECK(set.sources().front().kind == SourceKind::tes4);

    CHECK(read_text(set, "meshes/clutter/foo.nif") == "archived-foo");
    CHECK(read_text(set, "TEXTURES\\FOO\\BAR.DDS") == "archived-bar");
}

TEST_CASE("compressed entries round-trip on both codecs", "[archive]") {
    // v104 is zlib, v105 LZ4; rsm-bsa needs the version at decompress time,
    // carried by `SourceInfo::version`.
    const auto version = GENERATE(bsa::tes4::version::tes5, bsa::tes4::version::sse);
    const std::string payload(4096, 'z');

    const TempDir dir;
    const auto bsa_path = dir / "compressed.bsa";
    write_bsa(bsa_path, {BsaEntry{"meshes/big.nif", payload}}, version, /*compress=*/true);

    ArchiveSet set;
    REQUIRE(set.mount_archive(bsa_path, 0).has_value());
    CHECK(set.sources().front().version == static_cast<std::uint32_t>(version));
    CHECK(read_text(set, "meshes/big.nif") == payload);
}

TEST_CASE("a loose file whose name is not lowercase still reads", "[archive]") {
    // A mod ships textures/!_Rudy_Misc/, the index lowercases it, and opening
    // the lowercase name fails on a case-sensitive filesystem. This lost 5,480
    // of 27,508 textures on a 619-mod list. (On case-insensitive filesystems
    // this passes regardless.)
    const TempDir dir;
    dir.write("Textures/!_Rudy_Misc/Splashes01.DDS", "mixed-case");
    dir.write("textures/plain/lower.dds", "lower-case");

    ArchiveSet set;
    REQUIRE(set.mount_loose(dir.path(), 0) == 2);

    CHECK(read_text(set, "textures/!_rudy_misc/splashes01.dds") == "mixed-case");
    CHECK(read_text(set, "Textures\\!_Rudy_Misc\\Splashes01.DDS") == "mixed-case");
    // An all-lowercase file, with no recorded spelling, must still work.
    CHECK(read_text(set, "textures/plain/lower.dds") == "lower-case");
}

TEST_CASE("precedence still resolves when the disk spelling differs",
          "[archive][precedence]") {
    // The recorded spelling belongs to a source: with two directories differing
    // in case, the winner's file is read.
    const TempDir low;
    const TempDir high;
    low.write("meshes/Clutter/Foo.nif", "low");
    high.write("meshes/clutter/foo.nif", "high");

    ArchiveSet set;
    REQUIRE(set.mount_loose(low.path(), 0).has_value());
    REQUIRE(set.mount_loose(high.path(), 10).has_value());
    CHECK(set.unique_paths() == 1);
    CHECK(read_text(set, "meshes/clutter/foo.nif") == "high");

    ArchiveSet reversed;
    REQUIRE(reversed.mount_loose(high.path(), 0).has_value());
    REQUIRE(reversed.mount_loose(low.path(), 10).has_value());
    CHECK(read_text(reversed, "meshes/clutter/foo.nif") == "low");
}

TEST_CASE("a loose mount indexes nested directories", "[archive]") {
    const TempDir dir;
    dir.write("meshes/a/b/c/deep.nif", "deep");
    dir.write("root.txt", "shallow");

    ArchiveSet set;
    REQUIRE(set.mount_loose(dir.path(), 0) == 2);
    CHECK(read_text(set, "meshes/a/b/c/deep.nif") == "deep");
    CHECK(read_text(set, "root.txt") == "shallow");
}

TEST_CASE("bad mounts fail with a usable error instead of throwing", "[archive]") {
    const TempDir dir;

    SECTION("a file that is not an archive") {
        const auto path = dir.write("notreally.bsa", "this is not a BSA");
        ArchiveSet set;
        const auto mounted = set.mount_archive(path, 0);
        REQUIRE_FALSE(mounted.has_value());
        CHECK(mounted.error().kind == bethconv::io::ErrorKind::bad_magic);
        CHECK(set.sources().empty());
    }

    SECTION("an archive path that does not exist") {
        ArchiveSet set;
        const auto mounted = set.mount_archive(dir / "missing.bsa", 0);
        REQUIRE_FALSE(mounted.has_value());
        CHECK(set.sources().empty());
    }

    SECTION("a loose mount pointed at a file") {
        const auto path = dir.write("regular.txt", "x");
        ArchiveSet set;
        const auto mounted = set.mount_loose(path, 0);
        REQUIRE_FALSE(mounted.has_value());
        CHECK(mounted.error().kind == bethconv::io::ErrorKind::bad_value);
        CHECK(set.sources().empty());
    }
}

TEST_CASE("an empty set answers everything with nullopt", "[archive]") {
    const ArchiveSet set;
    CHECK(set.unique_paths() == 0);
    CHECK(set.sources().empty());
    CHECK(set.conflicts().empty());
    CHECK_FALSE(set.resolve("meshes/foo.nif").has_value());
}

TEST_CASE("an archive can name a path that climbs out of the output tree", "[archive]") {
    // Archive entry names are untrusted and normalization keeps "..", so a
    // resolved vpath can escape `out / vpath`. Writers must check
    // is_safe_relative before joining it onto an output directory.
    const TempDir dir;
    const auto bsa_path = dir / "evil.bsa";
    write_bsa(bsa_path, {BsaEntry{"..\\..\\escape\\x.dds", "payload"}});

    ArchiveSet set;
    REQUIRE(set.mount_archive(bsa_path, 0).has_value());
    std::vector<std::string> vpaths;
    set.for_each([&](const Resolution& r) { vpaths.push_back(r.vpath); });
    REQUIRE(vpaths.size() == 1);
    CHECK(vpaths.front().find("..") != std::string::npos);
    CHECK_FALSE(is_safe_relative(vpaths.front()));
}
