// SPDX-License-Identifier: GPL-3.0-or-later
//
// Corpus harness: runs the converter over real installs and compares with
// `corpus-expectations.json`. Each install is optional (environment variable)
// and skipped when unset, as in CI.
//
// After an intended behavior change, regenerate and read the diff:
//
//     bethconv-corpus-snapshot > tests/corpus/corpus-expectations.json
#include "corpus_probe.hpp"

#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace bethconv;
using nlohmann::json;

namespace {

/// Which installs were visited. Skipping passes, so a green run means nothing
/// without this list (the other hint was a 0.2 s runtime instead of 65 s).
class Visits {
public:
    static Visits& instance() {
        static Visits visits;
        return visits;
    }

    void visited(const std::string& id) { visited_.insert(id); }
    void skipped(const std::string& id) { skipped_.insert(id); }

    [[nodiscard]] const std::set<std::string>& visited() const { return visited_; }
    [[nodiscard]] const std::set<std::string>& skipped() const { return skipped_; }

private:
    std::set<std::string> visited_;
    std::set<std::string> skipped_;
};

/// BETHCONV_CORPUS_REQUIRE makes "visited nothing" a failure. Off by default so
/// CI and partial setups pass; set it when an install is expected.
[[nodiscard]] bool require_an_install() {
    return corpus::env("BETHCONV_CORPUS_REQUIRE").has_value();
}

const json& expectations() {
    static const json cached = [] {
        std::ifstream in(BETHCONV_CORPUS_EXPECTATIONS);
        REQUIRE(in.good());
        return json::parse(in);
    }();
    return cached;
}

/// The install's data directory, or nullopt when the harness should skip.
std::optional<std::filesystem::path> data_dir(const json& install) {
    const auto id = install.at("id").get<std::string>();
    const auto value = corpus::env(install.at("env").get<std::string>().c_str());
    if (!value) {
        Visits::instance().skipped(id);
        return std::nullopt;
    }
    std::filesystem::path path(*value);
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        Visits::instance().skipped(id);
        return std::nullopt;
    }
    Visits::instance().visited(id);
    return path;
}

std::string describe(const json& install) {
    return install.at("id").get<std::string>() + " (" +
           install.at("env").get<std::string>() + ")";
}

} // namespace

TEST_CASE("plugins parse to the same shape they did last time", "[corpus]") {
    for (const auto& install : expectations().at("installs")) {
        const auto dir = data_dir(install);
        if (!dir) {
            WARN("skipping " << describe(install) << ": not set or not a directory");
            continue;
        }

        for (const auto& expected : install.at("plugins")) {
            const auto file = expected.at("file").get<std::string>();
            INFO(describe(install) << " :: " << file);

            const auto path = *dir / file;
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                WARN("absent from this install: " << file);
                continue;
            }

            const auto facts = corpus::probe_plugin(path);
            REQUIRE(facts.has_value());

            // Checked first: only a data change moves it. A mismatch means the
            // game was patched (as SE was on 2026-08-28) and the expectations
            // need regenerating, not the reader fixing.
            const auto recorded_bytes = expected.at("file_bytes").get<std::uint64_t>();
            if (facts->file_bytes != recorded_bytes) {
                WARN(file << " is " << facts->file_bytes << " bytes on disk, expected "
                          << recorded_bytes
                          << ": this install has changed since the expectations were "
                             "generated. Failures below are the data moving, not the reader "
                             "-- see tests/corpus/README.md.");
            }
            CHECK(facts->file_bytes == recorded_bytes);

            CHECK(corpus::format_version(facts->header_version) ==
                  expected.at("header_version").get<std::string>());

            // Structural counts: first to move on a GRUP walk regression.
            CHECK(facts->records == expected.at("records").get<std::uint64_t>());
            CHECK(facts->groups == expected.at("groups").get<std::uint64_t>());
            CHECK(facts->compressed_records ==
                  expected.at("compressed_records").get<std::uint64_t>());
            CHECK(facts->record_types == expected.at("record_types").get<std::uint64_t>());

            // Vanilla must parse without structural errors.
            CHECK(facts->errors == 0);

            // HEDR equals records + groups (esm4-plugins.md). Catches skipped or
            // repeated groups even if the counts were regenerated with the bug.
            CHECK(static_cast<std::uint64_t>(facts->hedr_record_count) ==
                  facts->records + facts->groups);

            // Header flags, from files where flag and extension matter.
            CHECK(facts->is_master == expected.at("is_master").get<bool>());
            CHECK(facts->is_light == expected.at("is_light").get<bool>());
            CHECK(facts->is_localized == expected.at("is_localized").get<bool>());
            CHECK(facts->masters == expected.at("masters").get<std::uint64_t>());

            // One number for all per-type counts.
            CHECK(facts->type_histogram_hash ==
                  expected.at("type_histogram_hash").get<std::uint64_t>());

            // Coverage may only rise; new definitions must not fail the test.
            CHECK(facts->field_coverage_bp >=
                  expected.at("field_coverage_bp").get<std::uint64_t>());
        }
    }
}

TEST_CASE("archives mount to the same virtual filesystem", "[corpus]") {
    for (const auto& install : expectations().at("installs")) {
        const auto dir = data_dir(install);
        if (!dir) {
            WARN("skipping " << describe(install) << ": not set or not a directory");
            continue;
        }
        INFO(describe(install));

        std::vector<std::string> names;
        for (const auto& expected : install.at("archives")) {
            names.push_back(expected.at("file").get<std::string>());
        }

        archive::ArchiveSet set;
        const auto scan = corpus::probe_archives(*dir, names, set);

        if (!scan.missing.empty()) {
            WARN("absent from this install: " << scan.missing.size() << " archive(s)");
        }

        for (const auto& facts : scan.archives) {
            INFO("archive: " << facts.file);
            const auto expected = std::ranges::find_if(
                install.at("archives"), [&](const json& e) {
                    return e.at("file").get<std::string>() == facts.file;
                });
            REQUIRE(expected != install.at("archives").end());

            CHECK(facts.kind == expected->at("kind").get<std::string>());
            CHECK(facts.version == expected->at("version").get<std::uint32_t>());
            CHECK(facts.files == expected->at("files").get<std::uint64_t>());
        }

        // Only meaningful if every archive mounted.
        if (scan.missing.empty()) {
            CHECK(scan.unique_paths == install.at("unique_paths").get<std::uint64_t>());
            CHECK(scan.shadowed == install.at("shadowed").get<std::uint64_t>());
        }

        // Entries corrupt in the shipped archive, pinned both ways: a new
        // failure is our regression, a new success means this is not the
        // recorded vanilla install.
        std::vector<std::string> known_bad;
        for (const auto& vpath : install.at("expected_read_failures")) {
            known_bad.push_back(vpath.get<std::string>());
        }
        if (!known_bad.empty() && scan.missing.empty()) {
            const auto check = corpus::check_reads(set, known_bad);
            // Absent means a typo or the wrong game; succeeded means no longer
            // broken. Neither may pass silently.
            CHECK(check.unresolved.empty());
            CHECK(check.succeeded.empty());
            CHECK(check.failed == known_bad);
        }
    }
}

TEST_CASE("textures keep their shape, and the mip fixup keeps its bytes",
          "[corpus]") {
    for (const auto& install : expectations().at("installs")) {
        const auto dir = data_dir(install);
        if (!dir) {
            WARN("skipping " << describe(install) << ": not set or not a directory");
            continue;
        }
        if (!install.contains("textures") || install.at("textures").empty()) {
            continue;
        }
        INFO(describe(install));

        std::vector<std::string> names;
        for (const auto& expected : install.at("archives")) {
            names.push_back(expected.at("file").get<std::string>());
        }
        archive::ArchiveSet set;
        const auto scan = corpus::probe_archives(*dir, names, set);
        if (!scan.missing.empty()) {
            WARN("absent from this install: " << scan.missing.size() << " archive(s)");
        }

        for (const auto& expected : install.at("textures")) {
            const auto vpath = expected.at("vpath").get<std::string>();
            INFO("texture: " << vpath);

            const auto facts = corpus::probe_texture(set, vpath);
            if (!facts) {
                WARN("absent from this install: " << vpath);
                continue;
            }

            // Header arithmetic on real surfaces.
            CHECK(facts->format == expected.at("format").get<std::string>());
            CHECK(facts->kind == expected.at("kind").get<std::string>());
            CHECK(facts->width == expected.at("width").get<std::uint32_t>());
            CHECK(facts->height == expected.at("height").get<std::uint32_t>());
            CHECK(facts->faces == expected.at("faces").get<std::uint32_t>());
            CHECK(facts->stored_levels == expected.at("stored_levels").get<std::uint32_t>());
            CHECK(facts->full_chain_levels ==
                  expected.at("full_chain_levels").get<std::uint32_t>());
            CHECK(facts->declared_bytes == expected.at("declared_bytes").get<std::uint64_t>());

            // The output; the hash catches a single changed byte.
            CHECK(facts->outcome == expected.at("outcome").get<std::string>());
            CHECK(facts->output_bytes == expected.at("output_bytes").get<std::uint64_t>());
            CHECK(facts->output_hash == expected.at("output_hash").get<std::uint64_t>());
        }
    }
}

TEST_CASE("meshes convert to the same geometry, and the same bytes", "[corpus]") {
    for (const auto& install : expectations().at("installs")) {
        const auto dir = data_dir(install);
        if (!dir) {
            WARN("skipping " << describe(install) << ": not set or not a directory");
            continue;
        }
        if (!install.contains("meshes") || install.at("meshes").empty()) {
            continue;
        }
        INFO(describe(install));

        std::vector<std::string> names;
        for (const auto& expected : install.at("archives")) {
            names.push_back(expected.at("file").get<std::string>());
        }
        archive::ArchiveSet set;
        const auto scan = corpus::probe_archives(*dir, names, set);
        if (!scan.missing.empty()) {
            WARN("absent from this install: " << scan.missing.size() << " archive(s)");
        }

        for (const auto& expected : install.at("meshes")) {
            const auto vpath = expected.at("vpath").get<std::string>();
            INFO("mesh: " << vpath);

            const auto facts = corpus::probe_mesh(set, vpath);
            if (!facts) {
                WARN("absent from this install: " << vpath);
                continue;
            }

            // Reader path; includes the LE-format file in SE's archives.
            CHECK(facts->flavor == expected.at("flavor").get<std::string>());
            CHECK(facts->nif_version == expected.at("nif_version").get<std::string>());
            CHECK(facts->nif_stream == expected.at("nif_stream").get<std::uint32_t>());

            // IR counts: first to move if shapes, joints or collision drop.
            CHECK(facts->nodes == expected.at("nodes").get<std::uint64_t>());
            CHECK(facts->roots == expected.at("roots").get<std::uint64_t>());
            CHECK(facts->primitives == expected.at("primitives").get<std::uint64_t>());
            CHECK(facts->materials == expected.at("materials").get<std::uint64_t>());
            CHECK(facts->skins == expected.at("skins").get<std::uint64_t>());
            CHECK(facts->collision == expected.at("collision").get<std::uint64_t>());
            CHECK(facts->joints == expected.at("joints").get<std::uint64_t>());
            CHECK(facts->vertices == expected.at("vertices").get<std::uint64_t>());
            CHECK(facts->triangles == expected.at("triangles").get<std::uint64_t>());

            // Attribute presence, so a dropped TANGENT or COLOR_0 shows.
            CHECK(facts->with_normals == expected.at("with_normals").get<std::uint64_t>());
            CHECK(facts->with_tangents == expected.at("with_tangents").get<std::uint64_t>());
            CHECK(facts->with_uvs == expected.at("with_uvs").get<std::uint64_t>());
            CHECK(facts->with_colors == expected.at("with_colors").get<std::uint64_t>());
            CHECK(facts->with_joints == expected.at("with_joints").get<std::uint64_t>());

            // Pinned both ways: a new warning is a regression; a missing one
            // means a Havok shape now decodes.
            CHECK(facts->warnings == expected.at("warnings").get<std::uint64_t>());

            // Writer output; the hash catches any changed byte (accessor,
            // transform, URI, extras).
            CHECK(facts->glb_bytes == expected.at("glb_bytes").get<std::uint64_t>());
            CHECK(facts->glb_hash == expected.at("glb_hash").get<std::uint64_t>());

            // Never recorded: no control bytes, strict JSON must parse. The
            // FaceGen head and greybeardstatic.nif in these lists make these
            // checks meaningful.
            CHECK(facts->glb_control_bytes == 0);
            CHECK(facts->glb_json_parses);

            // Escaping, named explicitly rather than only via `glb_hash`.
            CHECK(facts->images == expected.at("images").get<std::uint64_t>());
            CHECK(facts->escaped_uris == expected.at("escaped_uris").get<std::uint64_t>());
        }
    }
}

TEST_CASE("a real install converts into the same pack, and the pack still opens",
          "[corpus]") {
    for (const auto& install : expectations().at("installs")) {
        const auto dir = data_dir(install);
        if (!dir) {
            WARN("skipping " << describe(install) << ": not set or not a directory");
            continue;
        }
        if (!install.contains("convert") || install.at("convert").empty()) {
            continue;
        }
        INFO(describe(install));

        std::vector<std::string> names;
        for (const auto& expected : install.at("archives")) {
            names.push_back(expected.at("file").get<std::string>());
        }
        archive::ArchiveSet set;
        const auto scan = corpus::probe_archives(*dir, names, set);
        if (!scan.missing.empty()) {
            // Everything below depends on what is mounted.
            WARN("absent from this install: " << scan.missing.size()
                                              << " archive(s); skipping the convert pin");
            continue;
        }

        // Two runs, since no single filter covers both: `critters` has every
        // kind (meshes, textures, scripts, a deferred `.hkx`, two control-byte
        // slots); the second selects meshes with spaces in texture slots and
        // their textures. Before it, `0186824` changed emitted bytes and this
        // harness stayed green.
        for (const auto& expected : install.at("convert")) {
            const auto name = expected.at("name").get<std::string>();
            INFO("convert run: " << name);

            // Fresh directory each run; reusing one would make everything
            // `deduped`.
            const bethconv::test::TempDir out;
            const auto facts = corpus::probe_convert(
                *dir, set, out.path() / "pack", expected.at("filter").get<std::string>(),
                expected.at("limit").get<std::size_t>());
            REQUIRE(facts.has_value());

            // The GLB checks, plus a nonzero count so they cannot pass vacuously.
            // A run of LOD data only has no GLB to examine.
            if (facts->meshes > 0) {
                CHECK(facts->glb_checked > 0);
            }
            CHECK(facts->glb_control_bytes == 0);
            CHECK(facts->glb_unparseable == 0);
            CHECK(facts->glb_bad_container == 0);

            // No failures, and no asset missing from a fresh pack's index.
            CHECK(facts->failed == 0);
            CHECK(facts->orphaned_assets == 0);

            // Install facts, checked first: if only these move, the corpus
            // changed.
            CHECK(facts->unique_paths == expected.at("unique_paths").get<std::uint64_t>());
            CHECK(facts->considered == expected.at("considered").get<std::uint64_t>());

            // What was converted, of which kind, and what was skipped.
            CHECK(facts->inputs == expected.at("inputs").get<std::uint64_t>());
            CHECK(facts->converted == expected.at("converted").get<std::uint64_t>());
            CHECK(facts->deduped == expected.at("deduped").get<std::uint64_t>());
            CHECK(facts->deferred == expected.at("deferred").get<std::uint64_t>());
            CHECK(facts->deferred_kinds == expected.at("deferred_kinds").get<std::uint64_t>());
            CHECK(facts->meshes == expected.at("meshes").get<std::uint64_t>());
            CHECK(facts->textures == expected.at("textures").get<std::uint64_t>());
            CHECK(facts->scripts == expected.at("scripts").get<std::uint64_t>());
            CHECK(facts->lod == expected.at("lod").get<std::uint64_t>());
            CHECK(facts->distinct_assets == expected.at("distinct_assets").get<std::uint64_t>());
            CHECK(facts->index_entries == expected.at("index_entries").get<std::uint64_t>());

            // Pinned both ways, as for meshes.
            CHECK(facts->warnings == expected.at("warnings").get<std::uint64_t>());

            // Bytes in and out.
            CHECK(facts->source_bytes == expected.at("source_bytes").get<std::uint64_t>());
            CHECK(facts->asset_bytes == expected.at("asset_bytes").get<std::uint64_t>());
            CHECK(facts->dedupe_saved_bytes ==
                  expected.at("dedupe_saved_bytes").get<std::uint64_t>());
            CHECK(facts->glb_escaped_uris ==
                  expected.at("glb_escaped_uris").get<std::uint64_t>());

            // The bookkeeping files. `index_hash` catches any asset hash change
            // (converter or fingerprint).
            CHECK(facts->manifest_hash == expected.at("manifest_hash").get<std::uint64_t>());
            CHECK(facts->index_hash == expected.at("index_hash").get<std::uint64_t>());
            CHECK(facts->report_hash == expected.at("report_hash").get<std::uint64_t>());

            // View of the pack just built, rather than converting again.
            if (!expected.contains("view")) {
                continue;
            }
            const auto& want_view = expected.at("view");
            const bethconv::test::TempDir viewed;
            const auto view = corpus::probe_view(out.path() / "pack", viewed.path() / "view",
                                                 want_view.at("filter").get<std::string>(),
                                                 want_view.at("limit").get<std::size_t>());
            REQUIRE(view.has_value());

            // No failures, and the GLB checks again: the view re-serializes each
            // mesh and is the last writer before a consumer.
            CHECK(view->failed == 0);
            if (view->meshes > 0) {
                CHECK(view->glb_checked > 0);
            }
            CHECK(view->glb_control_bytes == 0);
            CHECK(view->glb_unparseable == 0);
            CHECK(view->glb_bad_container == 0);

            // How many image URIs `vpath.idx` resolves: escaping and unescaping
            // must agree. On a filtered pack most dangling references are
            // textures the filter excluded (not the 572 of a full SE pack).
            CHECK(view->image_refs == want_view.at("image_refs").get<std::uint64_t>());
            CHECK(view->image_refs_resolved ==
                  want_view.at("image_refs_resolved").get<std::uint64_t>());
            CHECK(view->image_refs_dangling ==
                  want_view.at("image_refs_dangling").get<std::uint64_t>());

            // Entries and what became of them. `pulled_in` shows textures added
            // for selected meshes.
            CHECK(view->index_entries == want_view.at("index_entries").get<std::uint64_t>());
            CHECK(view->considered == want_view.at("considered").get<std::uint64_t>());
            CHECK(view->meshes == want_view.at("meshes").get<std::uint64_t>());
            CHECK(view->textures == want_view.at("textures").get<std::uint64_t>());
            CHECK(view->scripts == want_view.at("scripts").get<std::uint64_t>());
            CHECK(view->lod == want_view.at("lod").get<std::uint64_t>());
            CHECK(view->pulled_in == want_view.at("pulled_in").get<std::uint64_t>());

            // `written` + `mesh_links` is every mesh; identical meshes at the
            // same depth are linked.
            CHECK(view->written == want_view.at("written").get<std::uint64_t>());
            CHECK(view->mesh_links == want_view.at("mesh_links").get<std::uint64_t>());
            CHECK(view->linked == want_view.at("linked").get<std::uint64_t>());
            CHECK(view->copied == want_view.at("copied").get<std::uint64_t>());
            CHECK(view->bytes == want_view.at("bytes").get<std::uint64_t>());
        }
    }
}

TEST_CASE("the load order places every plugin where it did last time", "[corpus]") {
    for (const auto& install : expectations().at("installs")) {
        const auto dir = data_dir(install);
        if (!dir) {
            WARN("skipping " << describe(install) << ": not set or not a directory");
            continue;
        }
        if (!install.contains("load_order")) {
            continue;
        }
        INFO(describe(install));

        const auto facts = corpus::probe_load_order(*dir);
        const auto& expected = install.at("load_order");

            // Vanilla has no missing masters or ordering violations.
        CHECK(facts.problems == 0);
        CHECK(facts.normal_used == expected.at("normal_used").get<std::uint64_t>());
        CHECK(facts.light_used == expected.at("light_used").get<std::uint64_t>());

        const auto& listed = expected.at("plugins");
        REQUIRE(facts.plugins.size() == listed.size());
        for (std::size_t i = 0; i < facts.plugins.size(); ++i) {
            INFO("position " << i);
            // Position, index and flags: catches placement errors. Cannot catch
            // flag-from-filename bugs (vanilla flag and extension always agree);
            // tests/unit/test_load_order.cpp covers those.
            CHECK(facts.plugins[i].name == listed[i].at("name").get<std::string>());
            CHECK(facts.plugins[i].index == listed[i].at("index").get<std::string>());
            CHECK(facts.plugins[i].flags == listed[i].at("flags").get<std::string>());
            CHECK(facts.plugins[i].masters == listed[i].at("masters").get<std::uint64_t>());
        }
    }
}

TEST_CASE("every field definition still reads every record it claims", "[corpus]") {
    for (const auto& install : expectations().at("installs")) {
        const auto dir = data_dir(install);
        if (!dir) {
            WARN("skipping " << describe(install) << ": not set or not a directory");
            continue;
        }
        if (!install.contains("forms")) {
            continue;
        }
        INFO(describe(install));

        // The plugin pin's list; totals only make sense over the whole set, so
        // skip if a file is missing.
        std::vector<std::string> plugins;
        bool complete = true;
        for (const auto& expected : install.at("plugins")) {
            const auto file = expected.at("file").get<std::string>();
            std::error_code ec;
            if (!std::filesystem::exists(*dir / file, ec)) {
                WARN("absent from this install: " << file
                                                  << " -- the field-definition census "
                                                     "covers all of them or none");
                complete = false;
                break;
            }
            plugins.push_back(file);
        }
        if (!complete) {
            continue;
        }

        // Every archive, as for the merge: SE's tables are in
        // `Skyrim - Interface.bsa`. Without them the census would report 0
        // unresolved having asked nothing.
        std::vector<std::string> names;
        for (const auto& expected : install.at("archives")) {
            names.push_back(expected.at("file").get<std::string>());
        }
        archive::ArchiveSet set;
        const auto scan = corpus::probe_archives(*dir, names, set);
        if (!scan.missing.empty()) {
            WARN("absent from this install: " << scan.missing.size() << " archive(s)");
        }

        const auto facts = corpus::probe_forms(*dir, set, plugins);
        REQUIRE(facts.has_value());
        const auto& expected = install.at("forms");

        // Must be zero. `leftover` is the main point: a too-short definition
        // moves no other number.
        CHECK(facts->unreadable == 0);
        CHECK(facts->failed == 0);
        CHECK(facts->leftover == 0);
        CHECK(facts->leftover_fields == 0);

        // Install facts; read these first when things go red.
        CHECK(facts->plugins == plugins.size());
        CHECK(facts->localized == expected.at("localized").get<std::uint64_t>());
        CHECK(facts->parsed == expected.at("parsed").get<std::uint64_t>());

        // `types_defined` is `defined_types()`; `types_seen` shows which were
        // actually exercised.
        CHECK(facts->types_defined == expected.at("types_defined").get<std::uint64_t>());
        CHECK(facts->types_seen == expected.at("types_seen").get<std::uint64_t>());

        // `string_tables` distinguishes "0 unresolved" from "nothing asked".
        CHECK(facts->string_tables == expected.at("string_tables").get<std::uint64_t>());
        CHECK(facts->table_entries == expected.at("table_entries").get<std::uint64_t>());
        CHECK(facts->resolved_strings == expected.at("resolved_strings").get<std::uint64_t>());

        // Recorded, not zero: depends on which archive supplies each table
        // (docs/format-notes/localized-strings.md).
        CHECK(facts->unresolved_strings ==
              expected.at("unresolved_strings").get<std::uint64_t>());

        // Undecoded fields, pinned by count and by content hash.
        CHECK(facts->unhandled == expected.at("unhandled").get<std::uint64_t>());
        CHECK(facts->unhandled_fields == expected.at("unhandled_fields").get<std::uint64_t>());
        CHECK(facts->unhandled_hash == expected.at("unhandled_hash").get<std::uint64_t>());

        // Seen/parsed/failed per defined type (records a definition ran on, not
        // records the walk saw).
        CHECK(facts->type_counts_hash == expected.at("type_counts_hash").get<std::uint64_t>());
    }
}

TEST_CASE("every script decodes completely, to the same assets", "[corpus]") {
    for (const auto& install : expectations().at("installs")) {
        const auto dir = data_dir(install);
        if (!dir || !install.contains("scripts")) {
            continue;
        }
        INFO(describe(install));
        std::vector<std::string> names;
        for (const auto& expected : install.at("archives")) {
            names.push_back(expected.at("file").get<std::string>());
        }
        archive::ArchiveSet set;
        const auto scan = corpus::probe_archives(*dir, names, set);
        if (!scan.missing.empty()) {
            WARN("absent from this install: " << scan.missing.size() << " archive(s)");
            continue;
        }
        const auto facts = corpus::probe_scripts(set);
        const auto& expected = install.at("scripts");
        CHECK(facts.failed == 0);
        CHECK(facts.scripts == expected.at("scripts").get<std::uint64_t>());
        CHECK(facts.objects == expected.at("objects").get<std::uint64_t>());
        CHECK(facts.functions == expected.at("functions").get<std::uint64_t>());
        CHECK(facts.natives == expected.at("natives").get<std::uint64_t>());
        CHECK(facts.instructions == expected.at("instructions").get<std::uint64_t>());
        CHECK(facts.asset_hash == expected.at("asset_hash").get<std::uint64_t>());
    }
}

TEST_CASE("every animation file decodes, to the same assets", "[corpus]") {
    for (const auto& install : expectations().at("installs")) {
        const auto dir = data_dir(install);
        if (!dir || !install.contains("animations")) {
            continue;
        }
        INFO(describe(install));
        std::vector<std::string> names;
        for (const auto& expected : install.at("archives")) {
            names.push_back(expected.at("file").get<std::string>());
        }
        archive::ArchiveSet set;
        const auto scan = corpus::probe_archives(*dir, names, set);
        if (!scan.missing.empty()) {
            WARN("absent from this install: " << scan.missing.size() << " archive(s)");
            continue;
        }
        const auto facts = corpus::probe_animations(set);
        const auto& expected = install.at("animations");
        CHECK(facts.failed == 0);
        CHECK(facts.files == expected.at("files").get<std::uint64_t>());
        CHECK(facts.tagfiles == expected.at("tagfiles").get<std::uint64_t>());
        CHECK(facts.skeletons == expected.at("skeletons").get<std::uint64_t>());
        CHECK(facts.clips == expected.at("clips").get<std::uint64_t>());
        CHECK(facts.frames == expected.at("frames").get<std::uint64_t>());
        CHECK(facts.annotations == expected.at("annotations").get<std::uint64_t>());
        CHECK(facts.data_failed == 0);
        CHECK(facts.characters == expected.at("characters").get<std::uint64_t>());
        CHECK(facts.clip_generators == expected.at("clip_generators").get<std::uint64_t>());
        CHECK(facts.data_files == expected.at("data_files").get<std::uint64_t>());
        CHECK(facts.data_clips == expected.at("data_clips").get<std::uint64_t>());
        CHECK(facts.data_motions == expected.at("data_motions").get<std::uint64_t>());
        CHECK(facts.asset_hash == expected.at("asset_hash").get<std::uint64_t>());
    }
}

TEST_CASE("the load order collapses into the same flat world", "[corpus]") {
    for (const auto& install : expectations().at("installs")) {
        const auto dir = data_dir(install);
        if (!dir) {
            WARN("skipping " << describe(install) << ": not set or not a directory");
            continue;
        }
        if (!install.contains("merge")) {
            continue;
        }
        INFO(describe(install));

        // Every archive, in the expectations' order: on VR two archives ship
        // different copies of 105 `strings/` paths. See `probe_merge`.
        std::vector<std::string> names;
        for (const auto& expected : install.at("archives")) {
            names.push_back(expected.at("file").get<std::string>());
        }
        archive::ArchiveSet set;
        const auto scan = corpus::probe_archives(*dir, names, set);
        if (!scan.missing.empty()) {
            WARN("absent from this install: " << scan.missing.size() << " archive(s)");
        }

        const auto& expected = install.at("merge");
        std::vector<std::string> pinned;
        for (const auto& form : expected.at("forms")) {
            pinned.push_back(form.at("form").get<std::string>());
        }

        const bethconv::test::TempDir scratch;
        const auto facts = corpus::probe_merge(*dir, set, pinned, scratch.path());
        REQUIRE(facts.has_value());

        // Must be zero on vanilla.
        CHECK(facts->errors == 0);
        CHECK(facts->unparented == 0);
        CHECK(facts->unreadable == 0);

        // Recorded because of vanilla Skyrim.esm's GMST 0x0123C00E (index 1, no
        // masters): exactly one per install, so a second one fails
        // (docs/format-notes/merge.md).
        CHECK(facts->unresolved == expected.at("unresolved").get<std::uint64_t>());
        CHECK(facts->problems == expected.at("problems").get<std::uint64_t>());

        // Totals of the collapse.
        CHECK(facts->plugins == expected.at("plugins").get<std::uint64_t>());
        CHECK(facts->visited == expected.at("visited").get<std::uint64_t>());
        CHECK(facts->forms == expected.at("form_count").get<std::uint64_t>());
        CHECK(facts->collapsed == expected.at("collapsed").get<std::uint64_t>());
        CHECK(facts->deleted == expected.at("deleted").get<std::uint64_t>());
        CHECK(facts->injected == expected.at("injected").get<std::uint64_t>());
        CHECK(facts->string_tables == expected.at("string_tables").get<std::uint64_t>());

        // Integrity check: the second pass forwards exactly as many records as
        // there are forms. A wrong winner choice breaks this.
        CHECK(facts->forwarded == facts->forms);

        // Per-type totals after merging (moves when the collapse changes).
        CHECK(facts->type_counts_hash ==
              expected.at("type_counts_hash").get<std::uint64_t>());

        REQUIRE(facts->forms_pinned.size() == expected.at("forms").size());
        for (std::size_t i = 0; i < facts->forms_pinned.size(); ++i) {
            const auto& form = facts->forms_pinned[i];
            const auto& want = expected.at("forms")[i];
            INFO("form: " << form.form << " (" << want.at("type").get<std::string>() << ")");

            // Pinned both ways: indexed but not forwarded is a bug; missing
            // entirely is a typo or a regression.
            CHECK(form.present == want.at("present").get<bool>());

            // Provenance: winner, owner, number of collapsed versions.
            CHECK(form.type == want.at("type").get<std::string>());
            CHECK(form.parent == want.at("parent").get<std::string>());
            CHECK(form.winner == want.at("winner").get<std::string>());
            CHECK(form.owner == want.at("owner").get<std::string>());
            CHECK(form.overrides == want.at("overrides").get<std::uint32_t>());
            CHECK(form.flags == want.at("flags").get<std::uint32_t>());
            CHECK(form.deleted == want.at("deleted").get<bool>());
            CHECK(form.injected == want.at("injected").get<bool>());

            // Payload: moves when a different plugin wins.
            CHECK(form.payload_bytes == want.at("payload_bytes").get<std::uint64_t>());
            CHECK(form.payload_hash == want.at("payload_hash").get<std::uint64_t>());

            // Strings, hashed. `name_from_table` checks the name came from the
            // winning plugin's own table.
            CHECK(form.editor_id_hash == want.at("editor_id_hash").get<std::uint64_t>());
            CHECK(form.name_hash == want.at("name_hash").get<std::uint64_t>());
            CHECK(form.name_from_table == want.at("name_from_table").get<bool>());
        }

        // records.fb and world.fb from this world: written, reopened, and the
        // same counts and bytes as recorded.
        for (const auto& [name, pin] : {std::pair{"snapshot", &facts->snapshot},
                                        std::pair{"world", &facts->world}}) {
            INFO(name);
            REQUIRE_FALSE(pin->empty());
            const auto& want = expected.at(name);
            REQUIRE(want.size() == pin->size());
            for (const auto& [key, value] : *pin) {
                INFO(key);
                CHECK(value == want.at(key).get<std::uint64_t>());
            }
        }
    }
}

// Runs last (Catch2 uses declaration order by default) and reports which
// installs were visited.
TEST_CASE("say which installs this run actually visited", "[corpus][summary]") {
    const auto& visited = Visits::instance().visited();
    const auto& skipped = Visits::instance().skipped();

    std::cout << "\ncorpus harness: visited " << visited.size() << " install(s)";
    for (const auto& id : visited) {
        std::cout << " " << id;
    }
    if (!skipped.empty()) {
        std::cout << "; skipped";
        for (const auto& id : skipped) {
            std::cout << " " << id;
        }
    }
    std::cout << std::endl;

    if (visited.empty()) {
        WARN("this run tested nothing: every install was skipped. Point "
             "SKYRIM_DATA_LE / _SE / _VR at a Data folder, or set "
             "BETHCONV_CORPUS_REQUIRE to make this a failure.");
    }
    if (require_an_install()) {
        // With REQUIRE set, passing by doing nothing is impossible.
        REQUIRE_FALSE(visited.empty());
    }
}
