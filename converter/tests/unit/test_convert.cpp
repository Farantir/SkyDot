// SPDX-License-Identifier: GPL-3.0-or-later
//
// `bethconv convert` over a synthetic mount. test_pack_writer.cpp covers the
// writer; this covers the layer above: which paths become assets, of which
// kind, what filter and limit do, and whether ConvertOptions reach the content
// hash. Three files and a two-plugin order, no game data.
#include "bethconv/pack/asset_store.hpp"
#include "bethconv/pack/convert.hpp"

#include "../support/dds_builder.hpp"
#include "../support/esm_builder.hpp"
#include "../support/hkx_builder.hpp"
#include "../support/nif_builder.hpp"
#include "../support/pex_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using bethconv::test::TempDir;
using bethconv::testing::DdsSpec;
using namespace bethconv::pack;

namespace {

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::byte> a_nif() {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    auto* shape = builder.add_shape("Cube", bethconv::test::make_cube());
    builder.add_shader(shape, "textures/fixture.dds", "textures/fixture_n.dds");
    return builder.bytes();
}

std::vector<std::byte> a_dds() {
    return bethconv::testing::build_dds(DdsSpec{.width = 8, .height = 8, .mips = 4});
}

std::vector<std::byte> an_hkx() {
    bethconv::test::HkxBuilder b(8);
    b.add_skeleton("NPC Root [Root]", {{"NPC Root [Root]", -1, {0, 0, 0}, {0, 0, 0, 1}}});
    return b.bytes();
}

std::vector<std::byte> a_pex() {
    return bethconv::testing::build_script(bethconv::testing::empty_script("Fixture"));
}

/// A minimal plugin, so `convert` has a load order for records.fb.
void write_plugin(const std::filesystem::path& path, const std::vector<std::string>& masters) {
    bethconv::test::ByteWriter file;
    bethconv::test::write_tes4(file, 0, masters);
    write_file(path, file.span());
}

/// A data folder with one plugin, one file of each convertible kind and two
/// unconverted inputs.
struct Fixture {
    TempDir dir;
    bethconv::archive::ArchiveSet set;

    Fixture() {
        write_plugin(dir.path() / "Fixture.esm", {});
        write_file(dir.path() / "meshes/fixture.nif", a_nif());
        write_file(dir.path() / "textures/fixture.dds", a_dds());
        write_file(dir.path() / "scripts/fixture.pex", a_pex());
        // Not converted yet; the manifest must count it.
        write_file(dir.path() / "sound/fixture.fuz", a_dds());
        write_file(dir.path() / "sound/fixture.wav", a_dds());
        write_file(dir.path() / "meshes/actors/fixture.hkx", an_hkx());
        REQUIRE(set.mount_loose(dir.path(), 0).has_value());
    }

    bethconv::record::LoadOrder order() const {
        bethconv::record::PluginList list;
        list.plugins.push_back(bethconv::record::ListedPlugin{.name = "Fixture.esm",
                                                              .active = true});
        return bethconv::record::LoadOrder::build(
            dir.path(), list,
            bethconv::record::LoadOrderOptions{.active_only = true,
                                               .add_implicit_masters = false,
                                               .always_loaded = {}});
    }
};

ConvertOptions options_for(const std::filesystem::path& out) {
    ConvertOptions options;
    options.out = out;
    options.converter = "bethconv-test";
    return options;
}

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
}

/// The set of asset files on disk, by their `<bb>/<hash><ext>` names.
/// Stored assets as "<hex><ext>", whichever layout the pack uses.
std::set<std::string> assets_in(const std::filesystem::path& root) {
    std::set<std::string> names;
    if (std::filesystem::exists(root / "assets.idx")) {
        std::ifstream in(root / "assets.idx", std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes;
        for (const char c : raw) {
            bytes.push_back(static_cast<std::byte>(c));
        }
        const auto index = AssetIndex::parse(bytes, "assets.idx");
        REQUIRE(index.has_value());
        for (const auto& entry : index->entries) {
            names.insert(entry.hash.hex() + std::string(extension_of(entry.kind)));
        }
        return names;
    }
    const auto dir = root / "assets";
    if (!std::filesystem::is_directory(dir)) {
        return names;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            names.insert(entry.path().filename().string());
        }
    }
    return names;
}

} // namespace

TEST_CASE("one pass over a mount produces a whole pack", "[convert]") {
    Fixture fixture;
    TempDir out;
    const auto pack = out.path() / "pack";

    const auto result = convert(fixture.set, fixture.order(), options_for(pack));
    REQUIRE(result.has_value());

    // One of each kind, no failures.
    CHECK(result->pack.meshes == 1);
    CHECK(result->pack.textures == 1);
    CHECK(result->pack.scripts == 1);
    CHECK(result->pack.animations == 1);
    CHECK(result->pack.failed == 0);
    CHECK(result->pack.converted == 4);

    // Every pack file.
    CHECK(std::filesystem::exists(pack / "manifest.json"));
    CHECK(std::filesystem::exists(pack / "vpath.idx"));
    CHECK(std::filesystem::exists(pack / "report.json"));
    CHECK(std::filesystem::exists(pack / "records.fb"));
    CHECK(assets_in(pack).size() == 4);

    REQUIRE(result->snapshot.has_value());
    REQUIRE(result->merge.has_value());
}

TEST_CASE("assets use the .glb, .dds, .pexfb and .animfb extensions",
          "[convert]") {
    Fixture fixture;
    TempDir out;
    const auto pack = out.path() / "pack";
    REQUIRE(convert(fixture.set, fixture.order(), options_for(pack)).has_value());

    std::set<std::string> extensions;
    for (const auto& name : assets_in(pack)) {
        extensions.insert(name.substr(name.rfind('.')));
    }
    // `.dds`, not `.ktx2`; scripts decoded into `.pexfb`, Havok files into
    // `.animfb`.
    CHECK(extensions == std::set<std::string>{".animfb", ".dds", ".glb", ".pexfb"});
}

TEST_CASE("an input this pass does not convert is counted, not dropped silently",
          "[convert]") {
    Fixture fixture;
    TempDir out;
    const auto pack = out.path() / "pack";
    const auto result = convert(fixture.set, fixture.order(), options_for(pack));
    REQUIRE(result.has_value());

    // Three: the plugin itself is a loose file the asset pass does not convert,
    // so `.esm` shows up in the deferred counts. records.fb comes from the load
    // order, not this walk.
    CHECK(result->pack.deferred == 3);
    CHECK(result->pack.deferred_kinds == 3);

    const auto report = read_json(pack / "report.json");
    REQUIRE(report.contains("deferred"));
    CHECK(report["deferred"].contains(".fuz"));
    CHECK(report["deferred"].contains(".wav"));
    CHECK(report["deferred"].contains(".esm"));
}

TEST_CASE("each pass can be turned off on its own", "[convert]") {
    Fixture fixture;
    TempDir out;

    SECTION("no meshes") {
        auto options = options_for(out.path() / "pack");
        options.convert_meshes = false;
        const auto result = convert(fixture.set, fixture.order(), options);
        REQUIRE(result.has_value());
        CHECK(result->pack.meshes == 0);
        CHECK(result->pack.textures == 1);
        CHECK(result->pack.scripts == 1);
    }

    SECTION("no records") {
        auto options = options_for(out.path() / "pack");
        options.write_records = false;
        const auto result = convert(fixture.set, fixture.order(), options);
        REQUIRE(result.has_value());
        CHECK_FALSE(result->snapshot.has_value());
        CHECK_FALSE(std::filesystem::exists(out.path() / "pack" / "records.fb"));
    // Assets-only still writes assets (for re-testing without a merge).
        CHECK(result->pack.converted == 4);
    }
}

TEST_CASE("the filter and the limit decide the work list, reproducibly",
          "[convert]") {
    Fixture fixture;
    TempDir out;

    SECTION("a filter is a substring of the virtual path") {
        auto options = options_for(out.path() / "filtered");
        options.filter = "textures/";
        const auto result = convert(fixture.set, fixture.order(), options);
        REQUIRE(result.has_value());
        CHECK(result->pack.textures == 1);
        CHECK(result->pack.meshes == 0);
        CHECK(result->pack.scripts == 0);
    }

    SECTION("a limited run stops in the same place twice") {
    // Compares full lists, so an unordered walk fails even with the right
    // count.
        auto first = options_for(out.path() / "a");
        first.limit = 2;
        auto second = options_for(out.path() / "b");
        second.limit = 2;

        const auto a = convert(fixture.set, fixture.order(), first);
        const auto b = convert(fixture.set, fixture.order(), second);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(a->considered == 2);
        CHECK(b->considered == 2);
        CHECK(assets_in(out.path() / "a") == assets_in(out.path() / "b"));
    }
}

TEST_CASE("a settings change renames every asset it could have affected",
          "[convert]") {
    Fixture fixture;
    TempDir out;

    auto first = options_for(out.path() / "a");
    auto second = options_for(out.path() / "b");
    // A change in the seventh decimal, enough to move a vertex but no count.
    second.mesh_write.unit_scale = first.mesh_write.unit_scale + 1e-7f;

    REQUIRE(convert(fixture.set, fixture.order(), first).has_value());
    REQUIRE(convert(fixture.set, fixture.order(), second).has_value());

    const auto a = assets_in(out.path() / "a");
    const auto b = assets_in(out.path() / "b");
    REQUIRE(a.size() == 4);
    REQUIRE(b.size() == 4);

    // Only the mesh moved: fingerprints are per kind.
    std::vector<std::string> shared;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                          std::back_inserter(shared));
    CHECK(shared.size() == 3);
    for (const auto& name : shared) {
        CHECK(name.find(".glb") == std::string::npos);
    }
}

TEST_CASE("the converter version is in the manifest and in every asset name",
          "[convert]") {
    Fixture fixture;
    TempDir out;

    auto first = options_for(out.path() / "a");
    auto second = options_for(out.path() / "b");
    second.converter = "bethconv-test-next";

    REQUIRE(convert(fixture.set, fixture.order(), first).has_value());
    REQUIRE(convert(fixture.set, fixture.order(), second).has_value());

    const auto manifest = read_json(out.path() / "b" / "manifest.json");
    CHECK(manifest["converter"] == "bethconv-test-next");

    // Nothing shared: a newer converter reconverts everything, so two packs
    // never collide.
    std::vector<std::string> shared;
    const auto a = assets_in(out.path() / "a");
    const auto b = assets_in(out.path() / "b");
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                          std::back_inserter(shared));
    CHECK(shared.empty());
}

TEST_CASE("two paths with the same bytes convert once", "[convert]") {
    Fixture fixture;
    // A second name for the same texture, as Skyrim does constantly.
    write_file(fixture.dir.path() / "textures/fixture_copy.dds", a_dds());
    bethconv::archive::ArchiveSet set;
    REQUIRE(set.mount_loose(fixture.dir.path(), 0).has_value());

    TempDir out;
    const auto pack = out.path() / "pack";
    const auto result = convert(set, fixture.order(), options_for(pack));
    REQUIRE(result.has_value());

    CHECK(result->pack.deduped == 1);
    CHECK(result->pack.distinct_assets == 4);
    CHECK(result->pack.index_entries == 5);
    CHECK(assets_in(pack).size() == 4);
}

TEST_CASE("the manifest records the order the records came from", "[convert]") {
    Fixture fixture;
    TempDir out;
    const auto pack = out.path() / "pack";
    REQUIRE(convert(fixture.set, fixture.order(), options_for(pack)).has_value());

    const auto manifest = read_json(pack / "manifest.json");
    CHECK(manifest["pack_format_version"] == k_pack_format_version);
    REQUIRE(manifest["load_order"].size() == 1);
    CHECK(manifest["load_order"][0] == "Fixture.esm");

    // The plugin is always hashed (records.fb depends on it).
    bool plugin_hashed = false;
    for (const auto& source : manifest["source_hashes"]) {
        if (source["kind"] == "plugin") {
            plugin_hashed = source.contains("hash");
        }
    }
    CHECK(plugin_hashed);
}

TEST_CASE("two runs over one mount produce the same pack", "[convert]") {
    Fixture fixture;
    TempDir out;

    // Determinism one layer above the writer, where the order is decided and
    // a clock is in scope.
    REQUIRE(convert(fixture.set, fixture.order(), options_for(out.path() / "a")).has_value());
    REQUIRE(convert(fixture.set, fixture.order(), options_for(out.path() / "b")).has_value());

    for (const auto& name : {"manifest.json", "vpath.idx", "report.json"}) {
        std::ifstream first(out.path() / "a" / name, std::ios::binary);
        std::ifstream second(out.path() / "b" / name, std::ios::binary);
        const std::string a{std::istreambuf_iterator<char>(first),
                            std::istreambuf_iterator<char>()};
        const std::string b{std::istreambuf_iterator<char>(second),
                            std::istreambuf_iterator<char>()};
        INFO(name);
        CHECK(a == b);
    }
    CHECK(assets_in(out.path() / "a") == assets_in(out.path() / "b"));
}
