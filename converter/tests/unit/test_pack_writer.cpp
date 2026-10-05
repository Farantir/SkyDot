// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pack layout, built without any game file. The focus is on two properties
// nothing else would catch:
//
//   * Determinism: same input, byte-identical pack. Hash-table order or a
//     timestamp would pass every other test.
//   * The index names only existing assets: a failed conversion leaves no line
//     in `vpath.idx`.
#include "bethconv/pack/asset_store.hpp"
#include "bethconv/pack/pack_writer.hpp"

#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using bethconv::test::TempDir;
using namespace bethconv::pack;

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

PackOptions default_options(StoreLayout layout = StoreLayout::blob) {
    PackOptions options;
    options.layout = layout;
    options.converter = "bethconv-test";
    options.mesh_settings = "mesh/1";
    options.texture_settings = "texture/1";
    options.script_settings = "script/1";
    return options;
}

PackManifest default_manifest() {
    PackManifest manifest;
    manifest.converter = "bethconv-test";
    manifest.language = "english";
    manifest.load_order = {"Fixture.esm"};
    return manifest;
}

/// Reserve, "convert" (uppercase the source), store: the real pipeline's shape.
AssetSlot put(PackWriter& writer, std::string_view vpath, AssetKind kind,
              std::string_view source, std::string_view converted,
              std::string_view from = "Fixture.bsa") {
    const auto slot = writer.reserve(vpath, kind, bytes_of(source), from);
    if (slot.already_present) {
        writer.reuse(slot);
        return slot;
    }
    REQUIRE(writer.store(slot, bytes_of(converted)).has_value());
    return slot;
}

/// An asset's bytes as a reader of the pack sees them.
std::string stored_text(const std::filesystem::path& root, const AssetSlot& slot) {
    auto reader = AssetReader::open(root);
    REQUIRE(reader.has_value());
    const VpathEntry entry{
        .vpath = slot.vpath, .hex = slot.hash.hex(), .kind = slot.kind, .source = {}};
    auto bytes = reader->read(entry);
    REQUIRE(bytes.has_value());
    std::string out;
    for (const std::byte b : bytes->data) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

} // namespace

TEST_CASE("a blob pack is an index and one blob", "[pack]") {
    TempDir dir;
    auto writer = PackWriter::create(dir / "pack", default_options());
    REQUIRE(writer.has_value());
    const auto mesh = put(*writer, "meshes/a.nif", AssetKind::mesh, "nif-a", "glb-a");
    const auto texture = put(*writer, "textures/a.dds", AssetKind::texture, "dds-a", "dds-aa");
    REQUIRE(writer->finish(default_manifest()).has_value());

    const auto root = dir / "pack";
    CHECK(std::filesystem::exists(root / "assets.idx"));
    CHECK(std::filesystem::exists(root / "assets-0001.blob"));
    CHECK_FALSE(std::filesystem::exists(root / "assets"));
    CHECK(stored_text(root, mesh) == "glb-a");
    CHECK(stored_text(root, texture) == "dds-aa");

    const auto doc = nlohmann::json::parse(read_text(root / "manifest.json"));
    CHECK(doc["store"]["layout"] == "blob");
    CHECK(doc["store"]["blob"] == "assets-0001.blob");
    // "glb-a" padded to the 16-byte boundary, then "dds-aa".
    CHECK(doc["store"]["bytes"] == 22);
}

TEST_CASE("a loose pack lands in the layout the plan describes", "[pack]") {
    TempDir dir;
    auto writer = PackWriter::create(dir / "pack", default_options(StoreLayout::loose));
    REQUIRE(writer.has_value());

    const auto mesh = put(*writer, "meshes/a.nif", AssetKind::mesh, "nif-a", "glb-a");
    put(*writer, "textures/a.dds", AssetKind::texture, "dds-a", "dds-a");
    put(*writer, "scripts/a.pex", AssetKind::script, "pex-a", "pex-a");

    const auto stats = writer->finish(default_manifest());
    REQUIRE(stats.has_value());
    CHECK(stats->converted == 3);
    CHECK(stats->meshes == 1);
    CHECK(stats->textures == 1);
    CHECK(stats->scripts == 1);

    const auto root = dir / "pack";
    CHECK(std::filesystem::exists(root / "manifest.json"));
    CHECK(std::filesystem::exists(root / "vpath.idx"));
    CHECK(std::filesystem::exists(root / "report.json"));

    // assets/<bb>/<64 hex>.<ext>, <bb> being the hash's first byte.
    const auto asset = root / "assets" / mesh.hash.prefix() / (mesh.hash.hex() + ".glb");
    REQUIRE(std::filesystem::exists(asset));
    CHECK(read_text(asset) == "glb-a");
    CHECK(stored_text(root, mesh) == "glb-a");
    CHECK_FALSE(std::filesystem::exists(root / "assets.idx"));
}

TEST_CASE("the extension is .dds, not .ktx2", "[pack]") {
    // DDS, not KTX2.
    CHECK(extension_of(AssetKind::texture) == ".dds");
    CHECK(extension_of(AssetKind::mesh) == ".glb");
    CHECK(extension_of(AssetKind::script) == ".pexfb");
}

TEST_CASE("two paths with the same bytes share one asset and two index lines",
          "[pack]") {
    TempDir dir;
    auto writer = PackWriter::create(dir / "pack", default_options());
    REQUIRE(writer.has_value());

    const auto first = put(*writer, "meshes/a.nif", AssetKind::mesh, "identical", "glb");
    const auto second = put(*writer, "meshes/b.nif", AssetKind::mesh, "identical", "glb");
    CHECK(first.hash == second.hash);
    CHECK_FALSE(first.already_present);
    CHECK(second.already_present);

    const auto stats = writer->finish(default_manifest());
    REQUIRE(stats.has_value());
    CHECK(stats->converted == 1);
    CHECK(stats->deduped == 1);
    CHECK(stats->distinct_assets == 1);
    // Both paths get a line, so "which file won this vpath" stays answerable.
    CHECK(stats->index_entries == 2);

    const auto index = read_text(dir / "pack" / "vpath.idx");
    CHECK(index.find("meshes/a.nif") != std::string::npos);
    CHECK(index.find("meshes/b.nif") != std::string::npos);
}

TEST_CASE("a failed input leaves no line in the index", "[pack]") {
    TempDir dir;
    auto writer = PackWriter::create(dir / "pack", default_options());
    REQUIRE(writer.has_value());

    put(*writer, "meshes/good.nif", AssetKind::mesh, "good", "glb");
    // Reserved but never stored, like a NIF that failed to read.
    const auto lost = writer->reserve("meshes/bad.nif", AssetKind::mesh, bytes_of("bad"),
                                      "Fixture.bsa");
    writer->fail(PackFailure{.vpath = "meshes/bad.nif",
                             .stage = "mesh",
                             .kind = "truncated",
                             .detail = "meshes/bad.nif+0x0: truncated"});

    const auto stats = writer->finish(default_manifest());
    REQUIRE(stats.has_value());
    CHECK(stats->inputs == 2);
    CHECK(stats->converted == 1);
    CHECK(stats->failed == 1);
    CHECK(stats->index_entries == 1);

    const auto index = read_text(dir / "pack" / "vpath.idx");
    CHECK(index.find("meshes/bad.nif") == std::string::npos);
    CHECK(index.find(lost.hash.hex()) == std::string::npos);
}

TEST_CASE("report.json names every failure and warning", "[pack]") {
    TempDir dir;
    auto writer = PackWriter::create(dir / "pack", default_options());
    REQUIRE(writer.has_value());

    put(*writer, "meshes/a.nif", AssetKind::mesh, "nif", "glb");
    writer->fail(PackFailure{
        .vpath = "meshes/bad.nif", .stage = "mesh", .kind = "corrupt", .detail = "no"});
    writer->warn(PackWarning{.vpath = "meshes/a.nif", .detail = "unsupported Havok shape"});
    writer->defer(".hkx");
    writer->defer(".hkx");
    writer->defer(".wav");

    const auto stats = writer->finish(default_manifest());
    REQUIRE(stats.has_value());
    CHECK(stats->deferred == 3);
    CHECK(stats->deferred_kinds == 2);

    const auto report = nlohmann::json::parse(read_text(dir / "pack" / "report.json"));
    CHECK(report["totals"]["failed"] == 1);
    CHECK(report["totals"]["warnings"] == 1);
    REQUIRE(report["failures"].size() == 1);
    CHECK(report["failures"][0]["vpath"] == "meshes/bad.nif");
    CHECK(report["failures"][0]["stage"] == "mesh");
    REQUIRE(report["warnings"].size() == 1);
    CHECK(report["warnings"][0]["detail"] == "unsupported Havok shape");
    CHECK(report["deferred"][".hkx"] == 2);
    CHECK(report["deferred"][".wav"] == 1);
}

TEST_CASE("a warning quoting a malformed name still produces a report", "[pack]") {
    // Warning details contain names from the NIF, and cp1252 bytes are common in
    // mods. `ordered_json::dump()` throws on invalid UTF-8, after every asset has
    // been written, so the old behavior was a full conversion with no report or
    // manifest. Fails if json_text is removed from the warning detail.
    TempDir dir;
    auto writer = PackWriter::create(dir / "pack", default_options());
    REQUIRE(writer.has_value());

    put(*writer, "meshes/a.nif", AssetKind::mesh, "nif", "glb");
    writer->warn(PackWarning{.vpath = std::string("meshes/caf\xE9\x01.nif"),
                             .detail = std::string("node 'caf\xE9' has an undecoded shape")});

    const auto stats = writer->finish(default_manifest());
    REQUIRE(stats.has_value());

    const auto report = nlohmann::json::parse(read_text(dir / "pack" / "report.json"));
    REQUIRE(report["warnings"].size() == 1);
    CHECK(report["warnings"][0]["detail"] == "node 'caf%E9' has an undecoded shape");
    CHECK(report["warnings"][0]["vpath"] == "meshes/caf%E9%01.nif");
}

TEST_CASE("the manifest carries the load order and the source hashes", "[pack]") {
    TempDir dir;
    auto writer = PackWriter::create(dir / "pack", default_options());
    REQUIRE(writer.has_value());
    put(*writer, "meshes/a.nif", AssetKind::mesh, "nif", "glb");

    auto manifest = default_manifest();
    manifest.load_order = {"Skyrim.esm", "Update.esm"};
    manifest.sources.push_back(SourceRecord{.name = "Skyrim.esm",
                                            .kind = "plugin",
                                            .bytes = 42,
                                            .hash = content_hash(bytes_of("esm"), "c", "source")});
    manifest.sources.push_back(
        SourceRecord{.name = "Skyrim - Meshes0.bsa", .kind = "tes4", .bytes = 7, .hash = std::nullopt});
    manifest.world = WorldRecord{.cells = 73725,
                                 .refs = 864815,
                                 .bases = 32814,
                                 .file_bytes = 616,
                                 .hash = content_hash(bytes_of("fb"), "c", "source")};

    REQUIRE(writer->finish(manifest).has_value());
    const auto doc = nlohmann::json::parse(read_text(dir / "pack" / "manifest.json"));
    CHECK(doc["pack_format_version"] == k_pack_format_version);
    CHECK(doc["load_order"] == nlohmann::json({"Skyrim.esm", "Update.esm"}));
    REQUIRE(doc["source_hashes"].size() == 2);
    CHECK(doc["source_hashes"][0].contains("hash"));
    // An unhashed archive has no `hash` key, not a zero value.
    CHECK_FALSE(doc["source_hashes"][1].contains("hash"));
    CHECK(doc["world"]["cells"] == 73725);
    CHECK(doc["world"]["file"] == "world.fb");
    CHECK_FALSE(doc.contains("records"));
}

TEST_CASE("two identical runs produce byte-identical packs", "[pack]") {
    // Inputs are offered in an order a hash table would not keep, so an
    // unsorted index fails here.
    const auto build = [](const std::filesystem::path& root) {
        auto writer = PackWriter::create(root, default_options());
        REQUIRE(writer.has_value());
        put(*writer, "textures/z.dds", AssetKind::texture, "z", "z");
        put(*writer, "meshes/a.nif", AssetKind::mesh, "a", "a");
        put(*writer, "scripts/m.pex", AssetKind::script, "m", "m");
        writer->warn(PackWarning{.vpath = "textures/z.dds", .detail = "second"});
        writer->warn(PackWarning{.vpath = "meshes/a.nif", .detail = "first"});
        REQUIRE(writer->finish(default_manifest()).has_value());
    };

    TempDir dir;
    build(dir / "one");
    build(dir / "two");

    for (const std::string name :
         {"manifest.json", "vpath.idx", "report.json", "assets.idx", "assets-0001.blob"}) {
        CAPTURE(name);
        CHECK(read_text(dir / "one" / name) == read_text(dir / "two" / name));
    }
}

TEST_CASE("nothing in a pack records a wall clock", "[pack]") {
    // No timestamps. Checked on the text itself, so it fails as soon as one is
    // added, not only when the clock ticks between two runs.
    TempDir dir;
    auto writer = PackWriter::create(dir / "pack", default_options());
    REQUIRE(writer.has_value());
    put(*writer, "meshes/a.nif", AssetKind::mesh, "a", "a");
    REQUIRE(writer->finish(default_manifest()).has_value());

    for (const std::string name : {"manifest.json", "report.json"}) {
        const auto text = read_text(dir / "pack" / name);
        CAPTURE(name, text);
        CHECK(text.find("generated") == std::string::npos);
        CHECK(text.find("timestamp") == std::string::npos);
        CHECK(text.find("built_at") == std::string::npos);
    }
}

TEST_CASE("reopening a pack skips what is already there", "[pack]") {
    // Incremental rebuild: the second run sees the asset as present before
    // converting anything.
    const auto layout = GENERATE(StoreLayout::blob, StoreLayout::loose);
    CAPTURE(to_string(layout));
    TempDir dir;
    {
        auto writer = PackWriter::create(dir / "pack", default_options(layout));
        REQUIRE(writer.has_value());
        put(*writer, "meshes/a.nif", AssetKind::mesh, "source", "converted");
        REQUIRE(writer->finish(default_manifest()).has_value());
    }

    auto again = PackWriter::create(dir / "pack", default_options(layout));
    REQUIRE(again.has_value());
    const auto slot = again->reserve("meshes/a.nif", AssetKind::mesh, bytes_of("source"),
                                     "Fixture.bsa");
    CHECK(slot.already_present);
    again->reuse(slot);

    const auto stats = again->finish(default_manifest());
    REQUIRE(stats.has_value());
    CHECK(stats->converted == 0);
    CHECK(stats->deduped == 1);
    CHECK(stats->dedupe_saved_bytes == 6); // "source"
    CHECK(stats->orphaned_assets == 0);
}

TEST_CASE("a changed settings fingerprint does not reuse the old asset", "[pack]") {
    // An option that changes output but not source must change the name, or
    // the second run reuses the first run's bytes.
    TempDir dir;
    {
        auto options = default_options();
        options.mesh_settings = "mesh/1;scale=0.0142875";
        auto writer = PackWriter::create(dir / "pack", options);
        REQUIRE(writer.has_value());
        put(*writer, "meshes/a.nif", AssetKind::mesh, "source", "scaled-for-metres");
        REQUIRE(writer->finish(default_manifest()).has_value());
    }

    auto options = default_options();
    options.mesh_settings = "mesh/1;scale=1";
    auto again = PackWriter::create(dir / "pack", options);
    REQUIRE(again.has_value());
    const auto slot = again->reserve("meshes/a.nif", AssetKind::mesh, bytes_of("source"),
                                     "Fixture.bsa");
    CHECK_FALSE(slot.already_present);
    REQUIRE(again->store(slot, bytes_of("unscaled")).has_value());

    const auto stats = again->finish(default_manifest());
    REQUIRE(stats.has_value());
    CHECK(stats->converted == 1);
    // The first run's asset is still on disk, no longer indexed.
    CHECK(stats->orphaned_assets == 1);
    CHECK(stats->pruned_assets == 0);
}

TEST_CASE("pruning removes exactly the assets nothing names", "[pack]") {
    const auto layout = GENERATE(StoreLayout::blob, StoreLayout::loose);
    CAPTURE(to_string(layout));
    TempDir dir;
    AssetSlot kept;
    {
        auto writer = PackWriter::create(dir / "pack", default_options(layout));
        REQUIRE(writer.has_value());
        kept = put(*writer, "meshes/a.nif", AssetKind::mesh, "keep", "keep");
        put(*writer, "meshes/b.nif", AssetKind::mesh, "drop", "drop");
        REQUIRE(writer->finish(default_manifest()).has_value());
    }

    auto options = default_options(layout);
    options.prune_orphans = true;
    auto again = PackWriter::create(dir / "pack", options);
    REQUIRE(again.has_value());
    const auto slot = again->reserve("meshes/a.nif", AssetKind::mesh, bytes_of("keep"),
                                     "Fixture.bsa");
    REQUIRE(slot.already_present);
    again->reuse(slot);

    const auto stats = again->finish(default_manifest());
    REQUIRE(stats.has_value());
    CHECK(stats->orphaned_assets == 1);
    CHECK(stats->pruned_assets == 1);
    CHECK(stored_text(dir / "pack", kept) == "keep");
    if (layout == StoreLayout::blob) {
        // Compaction wrote the next generation and dropped the old one.
        CHECK(std::filesystem::exists(dir / "pack" / "assets-0002.blob"));
        CHECK_FALSE(std::filesystem::exists(dir / "pack" / "assets-0001.blob"));
        CHECK(std::filesystem::file_size(dir / "pack" / "assets-0002.blob") == 4);
    }
}

TEST_CASE("bytes an interrupted run appended are dropped on reopening", "[pack]") {
    TempDir dir;
    AssetSlot first;
    {
        auto writer = PackWriter::create(dir / "pack", default_options());
        REQUIRE(writer.has_value());
        first = put(*writer, "meshes/a.nif", AssetKind::mesh, "a", "first");
        REQUIRE(writer->finish(default_manifest()).has_value());
    }
    // A run that appended and died before writing the index.
    {
        std::ofstream blob(dir / "pack" / "assets-0001.blob", std::ios::binary | std::ios::app);
        blob << "half-written garbage";
    }
    auto again = PackWriter::create(dir / "pack", default_options());
    REQUIRE(again.has_value());
    CHECK(std::filesystem::file_size(dir / "pack" / "assets-0001.blob") == 5);
    const auto second = put(*again, "meshes/b.nif", AssetKind::mesh, "b", "second");
    REQUIRE(again->finish(default_manifest()).has_value());
    CHECK(stored_text(dir / "pack", first) == "first");
    CHECK(stored_text(dir / "pack", second) == "second");
}

TEST_CASE("a damaged asset index is an error, not a short pack", "[pack]") {
    TempDir dir;
    {
        auto writer = PackWriter::create(dir / "pack", default_options());
        REQUIRE(writer.has_value());
        put(*writer, "meshes/a.nif", AssetKind::mesh, "a", "first");
        REQUIRE(writer->finish(default_manifest()).has_value());
    }
    const auto index_path = dir / "pack" / "assets.idx";
    auto bytes = bytes_of(read_text(index_path));
    REQUIRE(bytes.size() == k_asset_index_header + k_asset_index_entry);

    SECTION("truncated") { bytes.resize(bytes.size() - 1); }
    SECTION("an entry past the blob") { bytes[k_asset_index_header + 32] = std::byte{0x40}; }
    SECTION("a newer version") { bytes[4] = std::byte{9}; }
    SECTION("an unknown kind") { bytes[k_asset_index_header + 48] = std::byte{7}; }

    {
        std::ofstream out(index_path, std::ios::binary | std::ios::trunc);
        for (const std::byte b : bytes) {
            out.put(static_cast<char>(b));
        }
    }
    CHECK_FALSE(AssetReader::open(dir / "pack").has_value());
    CHECK_FALSE(PackWriter::create(dir / "pack", default_options()).has_value());
}
