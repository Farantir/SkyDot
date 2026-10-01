// SPDX-License-Identifier: GPL-3.0-or-later
//
// The view, without a game install. Three properties:
//
//   * URIs are rebased by exactly the document's depth: `../../` two levels
//     down, nothing at the root. Wrong prefixes load with no materials and no
//     error.
//   * Everything else survives: accessors, buffer and `extras` unchanged, the
//     BIN chunk byte-identical.
//   * Dangling references are counted.
#include "bethconv/pack/pack_view.hpp"

#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/pack/convert.hpp"
#include "bethconv/pack/vpath_index.hpp"

#include "../support/dds_builder.hpp"
#include "../support/esm_builder.hpp"
#include "../support/nif_builder.hpp"
#include "../support/pex_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
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

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out;
    out.reserve(raw.size());
    for (const char c : raw) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

/// A NIF naming one texture the mount has and one it lacks (common in real
/// data: meshes outliving their textures).
std::vector<std::byte> a_nif(std::string_view albedo, std::string_view normal) {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    auto* shape = builder.add_shape("Cube", bethconv::test::make_cube());
    builder.add_shader(shape, std::string(albedo), std::string(normal));
    return builder.bytes();
}

std::vector<std::byte> a_dds() {
    return bethconv::testing::build_dds(DdsSpec{.width = 8, .height = 8, .mips = 4});
}

void write_plugin(const std::filesystem::path& path) {
    bethconv::test::ByteWriter file;
    bethconv::test::write_tes4(file, 0, {});
    write_file(path, file.span());
}

/// One mount: two meshes at different depths, a texture and a script.
struct Fixture {
    TempDir dir;
    bethconv::archive::ArchiveSet set;

    Fixture() {
        write_plugin(dir.path() / "Fixture.esm");
        // Depth 2: needs `../../`.
        write_file(dir.path() / "meshes/clutter/apple01.nif",
                   a_nif("textures/clutter/apple01.dds", "textures/clutter/absent_n.dds"));
        // Depth 0: needs no prefix; an unconditional `../` would be wrong.
        write_file(dir.path() / "root.nif",
                   a_nif("textures/clutter/apple01.dds", "textures/clutter/absent_n.dds"));
        write_file(dir.path() / "textures/clutter/apple01.dds", a_dds());
        write_file(dir.path() / "scripts/fixture.pex", bethconv::testing::build_script(bethconv::testing::empty_script("Fixture")));
        REQUIRE(set.mount_loose(dir.path(), 0).has_value());
    }

    bethconv::record::LoadOrder order() const {
        bethconv::record::PluginList list;
        list.plugins.push_back(
            bethconv::record::ListedPlugin{.name = "Fixture.esm", .active = true});
        return bethconv::record::LoadOrder::build(
            dir.path(), list,
            bethconv::record::LoadOrderOptions{.active_only = true,
                                               .add_implicit_masters = false});
    }

    /// Build the pack the views are made from.
    ConvertResult build(const std::filesystem::path& out) const {
        ConvertOptions options;
        options.out = out;
        options.converter = "bethconv-test";
        options.write_records = false;
        auto result = convert(set, order(), options);
        REQUIRE(result.has_value());
        REQUIRE(result->pack.failed == 0);
        return *result;
    }
};

/// Parse a GLB's JSON chunk. Deliberately not reusing the code under test.
nlohmann::json glb_json(std::span<const std::byte> glb) {
    REQUIRE(glb.size() > 20);
    const auto word = [&](std::size_t at) {
        return static_cast<std::uint32_t>(glb[at]) |
               static_cast<std::uint32_t>(glb[at + 1]) << 8 |
               static_cast<std::uint32_t>(glb[at + 2]) << 16 |
               static_cast<std::uint32_t>(glb[at + 3]) << 24;
    };
    const std::uint32_t length = word(12);
    REQUIRE(20 + length <= glb.size());
    std::string text;
    for (std::size_t i = 0; i < length; ++i) {
        text.push_back(static_cast<char>(glb[20 + i]));
    }
    return nlohmann::json::parse(text);
}

/// Everything after the JSON chunk.
std::vector<std::byte> glb_tail(std::span<const std::byte> glb) {
    const auto word = [&](std::size_t at) {
        return static_cast<std::uint32_t>(glb[at]) |
               static_cast<std::uint32_t>(glb[at + 1]) << 8 |
               static_cast<std::uint32_t>(glb[at + 2]) << 16 |
               static_cast<std::uint32_t>(glb[at + 3]) << 24;
    };
    const std::size_t start = 20 + word(12);
    return {glb.begin() + static_cast<std::ptrdiff_t>(start), glb.end()};
}

std::vector<std::string> image_uris(const nlohmann::json& doc) {
    std::vector<std::string> out;
    if (!doc.contains("images")) {
        return out;
    }
    for (const auto& image : doc.at("images")) {
        if (image.contains("uri")) {
            out.push_back(image.at("uri").get<std::string>());
        }
    }
    return out;
}

} // namespace

TEST_CASE("the ascent prefix is one step per directory the path descends", "[view]") {
    CHECK(ascent_prefix("root.nif").empty());
    CHECK(ascent_prefix("meshes/apple.nif") == "../");
    CHECK(ascent_prefix("meshes/clutter/apple.nif") == "../../");
    CHECK(ascent_prefix("") .empty());
}

TEST_CASE("a view resolves a pack's textures for a consumer that only reads glTF", "[view]") {
    Fixture fixture;
    TempDir out;
    const auto pack = out.path() / "pack";
    const auto view = out.path() / "view";
    (void)fixture.build(pack);

    // In the pack, the URI is the uncorrected virtual path, so a consumer finds
    // nothing.
    const auto index = VpathIndex::read(pack / "vpath.idx");
    REQUIRE(index.has_value());
    const auto* mesh_entry = index->find("meshes/clutter/apple01.nif");
    REQUIRE(mesh_entry != nullptr);
    const auto packed = read_file(pack / std::filesystem::path(mesh_entry->asset_path()));
    CHECK(image_uris(glb_json(packed)) ==
          std::vector<std::string>{"textures/clutter/apple01.dds",
                                   "textures/clutter/absent_n.dds"});

    ViewOptions options;
    options.out = view;
    const auto result = materialize_view(pack, options);
    REQUIRE(result.has_value());
    CHECK(result->failures.empty());
    CHECK(result->stats.failed == 0);

    // Every index entry became a file at its virtual path, with the converted
    // extension.
    CHECK(std::filesystem::is_regular_file(view / "meshes/clutter/apple01.glb"));
    CHECK(std::filesystem::is_regular_file(view / "root.glb"));
    CHECK(std::filesystem::is_regular_file(view / "textures/clutter/apple01.dds"));
    CHECK(std::filesystem::is_regular_file(view / "scripts/fixture.pexfb"));
    CHECK(!std::filesystem::exists(view / "meshes/clutter/apple01.nif"));
    CHECK(result->stats.meshes == 2);
    CHECK(result->stats.textures == 1);
    CHECK(result->stats.scripts == 1);

    // Prefix for meshes/clutter/, and none at the root.
    CHECK(image_uris(glb_json(read_file(view / "meshes/clutter/apple01.glb"))) ==
          std::vector<std::string>{"../../textures/clutter/apple01.dds",
                                   "../../textures/clutter/absent_n.dds"});
    CHECK(image_uris(glb_json(read_file(view / "root.glb"))) ==
          std::vector<std::string>{"textures/clutter/apple01.dds",
                                   "textures/clutter/absent_n.dds"});

    // Each URI resolves to a file, or is counted as dangling.
    CHECK(std::filesystem::exists(view / "meshes/clutter" /
                                  "../../textures/clutter/apple01.dds"));
    CHECK(result->stats.image_refs == 4);
    CHECK(result->stats.image_refs_resolved == 2);
    CHECK(result->stats.image_refs_dangling == 2);
}

TEST_CASE("a rebase changes the image URIs and nothing else", "[view]") {
    Fixture fixture;
    TempDir out;
    const auto pack = out.path() / "pack";
    const auto view = out.path() / "view";
    (void)fixture.build(pack);

    const auto index = VpathIndex::read(pack / "vpath.idx");
    REQUIRE(index.has_value());
    const auto* entry = index->find("meshes/clutter/apple01.nif");
    REQUIRE(entry != nullptr);
    const auto packed = read_file(pack / std::filesystem::path(entry->asset_path()));

    ViewOptions options;
    options.out = view;
    REQUIRE(materialize_view(pack, options).has_value());
    const auto viewed = read_file(view / "meshes/clutter/apple01.glb");

    // The BIN chunk is copied, not rebuilt.
    CHECK(glb_tail(packed) == glb_tail(viewed));

    auto before = glb_json(packed);
    auto after = glb_json(viewed);
    // `extras` keep the pack-relative path; only `images[].uri` changes.
    CHECK(before.at("meshes") == after.at("meshes"));
    CHECK(before.at("accessors") == after.at("accessors"));
    CHECK(before.at("bufferViews") == after.at("bufferViews"));
    CHECK(before.at("materials") == after.at("materials"));
    before.erase("images");
    after.erase("images");
    CHECK(before == after);
}

TEST_CASE("a filtered view still carries the textures its meshes name", "[view]") {
    Fixture fixture;
    TempDir out;
    const auto pack = out.path() / "pack";
    const auto view = out.path() / "view";
    (void)fixture.build(pack);

    ViewOptions options;
    options.out = view;
    options.filter = "meshes/";
    const auto result = materialize_view(pack, options);
    REQUIRE(result.has_value());

    // The filter selected one mesh and no textures; its texture is added
    // anyway.
    CHECK(result->stats.considered == 1);
    CHECK(result->stats.meshes == 1);
    CHECK(result->stats.pulled_in == 1);
    CHECK(std::filesystem::is_regular_file(view / "textures/clutter/apple01.dds"));
    CHECK(!std::filesystem::exists(view / "scripts/fixture.pexfb"));
}

TEST_CASE("two meshes with one hash at one depth are written once", "[view]") {
    TempDir dir;
    TempDir out;
    const auto pack = out.path() / "pack";
    const auto view = out.path() / "view";

    write_plugin(dir.path() / "Fixture.esm");
    const auto nif = a_nif("textures/fixture.dds", "textures/fixture_n.dds");
    // Same bytes, same depth: one hash, one rewrite, two files.
    write_file(dir.path() / "meshes/one.nif", nif);
    write_file(dir.path() / "meshes/two.nif", nif);
    write_file(dir.path() / "textures/fixture.dds", a_dds());

    bethconv::archive::ArchiveSet set;
    REQUIRE(set.mount_loose(dir.path(), 0).has_value());
    bethconv::record::PluginList list;
    list.plugins.push_back(bethconv::record::ListedPlugin{.name = "Fixture.esm", .active = true});
    const auto order = bethconv::record::LoadOrder::build(
        dir.path(), list,
        bethconv::record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false});

    ConvertOptions convert_options;
    convert_options.out = pack;
    convert_options.converter = "bethconv-test";
    convert_options.write_records = false;
    REQUIRE(convert(set, order, convert_options).has_value());

    ViewOptions options;
    options.out = view;
    const auto result = materialize_view(pack, options);
    REQUIRE(result.has_value());
    CHECK(result->stats.meshes == 2);
    CHECK(result->stats.written == 1);
    CHECK(result->stats.mesh_links == 1);
    CHECK(read_file(view / "meshes/one.glb") == read_file(view / "meshes/two.glb"));
}

TEST_CASE("a view is rebuildable over itself", "[view]") {
    Fixture fixture;
    TempDir out;
    const auto pack = out.path() / "pack";
    const auto view = out.path() / "view";
    (void)fixture.build(pack);

    ViewOptions options;
    options.out = view;
    const auto first = materialize_view(pack, options);
    REQUIRE(first.has_value());
    const auto second = materialize_view(pack, options);
    REQUIRE(second.has_value());
    CHECK(second->stats.failed == 0);
    CHECK(second->stats.meshes == first->stats.meshes);
    CHECK(second->stats.textures == first->stats.textures);
    CHECK(read_file(view / "meshes/clutter/apple01.glb").size() > 0);
}

TEST_CASE("a malformed index is an error, not a partial view", "[view]") {
    TempDir out;
    const auto pack = out.path() / "pack";
    std::filesystem::create_directories(pack / "assets");

    {
        std::ofstream idx(pack / "vpath.idx");
        idx << index_header();
        idx << "meshes/a.nif\tnot-a-hash\tmesh\tloose\n";
    }
    ViewOptions options;
    options.out = out.path() / "view";
    const auto result = materialize_view(pack, options);
    REQUIRE(!result.has_value());
    CHECK(result.error().kind == bethconv::io::ErrorKind::corrupt);

    // A path escaping the view is refused.
    {
        std::ofstream idx(pack / "vpath.idx");
        idx << index_header();
        idx << format_index_line("../escape.dds", std::string(64, 'a'), AssetKind::texture,
                                 "loose");
    }
    const auto escaped = materialize_view(pack, options);
    REQUIRE(escaped.has_value());
    CHECK(escaped->stats.failed == 1);
    CHECK(escaped->stats.textures == 0);
    CHECK(!std::filesystem::exists(out.path() / "escape.dds"));
}

TEST_CASE("the index round-trips through its own reader", "[view]") {
    const std::string hex(64, '3');
    const auto text = index_header() +
                      format_index_line("textures/b.dds", hex, AssetKind::texture, "a.bsa") +
                      format_index_line("meshes/a.nif", hex, AssetKind::mesh, "b.bsa");
    const auto index = VpathIndex::parse(text, "vpath.idx");
    REQUIRE(index.has_value());
    CHECK(index->format_version() == k_pack_format_version);
    REQUIRE(index->entries().size() == 2);
    // Sorted on read, since `find` is a binary search and files may be edited.
    CHECK(index->entries().front().vpath == "meshes/a.nif");
    CHECK(index->find("textures/b.dds")->source == "a.bsa");
    CHECK(index->find("meshes/a.nif")->asset_path() == "assets/33/" + hex + ".glb");
    CHECK(index->find("nothing/here.nif") == nullptr);
}

TEST_CASE("textures named only in material extras are reported for the view", "[pack][view]") {
    // Effect palettes, glow and environment maps are not glTF images, but an
    // engine shader needs them in the view.
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    builder.add_shader(builder.add_shape("fire", bethconv::test::make_cube()),
                       "textures\\effects\\fxfire.dds", "textures\\effects\\fxfire_n.dds");
    auto model = bethconv::mesh::read_nif(builder.bytes(), "fire.nif");
    REQUIRE(model.has_value());
    model->materials[0].textures[3] = "textures\\effects\\gradients\\GradFlame01.dds";
    auto glb = bethconv::mesh::write_glb(
        *model, bethconv::mesh::WriteOptions{.texture_refs = bethconv::mesh::TextureRefs::pack_vpaths});
    REQUIRE(glb.has_value());

    const auto rebased = rebase_glb_uris(*glb, "../", "fire.glb");
    REQUIRE(rebased.has_value());
    CHECK(rebased->image_vpaths ==
          std::vector<std::string>{"textures/effects/fxfire.dds", "textures/effects/fxfire_n.dds"});
    CHECK(rebased->slot_vpaths ==
          std::vector<std::string>{"textures/effects/gradients/gradflame01.dds"});
}
