// SPDX-License-Identifier: GPL-3.0-or-later
//
// Regenerates `corpus-expectations.json` from the configured installs. Output
// on stdout, diagnostics on stderr:
//
//     bethconv-corpus-snapshot > tests/corpus/corpus-expectations.json
//
// The existing file is also the input (installs, plugins, archives, known-bad
// entries). Only measured values are replaced; unconfigured installs keep
// their numbers instead of being emptied.
#include "corpus_probe.hpp"

#include "../support/temp_dir.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace bethconv;
using nlohmann::json;

namespace {

void update_plugins(const std::filesystem::path& dir, json& install) {
    for (auto& expected : install.at("plugins")) {
        const auto file = expected.at("file").get<std::string>();
        const auto path = dir / file;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            std::fprintf(stderr, "  %-32s absent, keeping recorded values\n", file.c_str());
            continue;
        }
        const auto facts = corpus::probe_plugin(path);
        if (!facts) {
            std::fprintf(stderr, "  %-32s WILL NOT OPEN -- not updated\n", file.c_str());
            continue;
        }
        expected["file_bytes"] = facts->file_bytes;
        expected["header_version"] = corpus::format_version(facts->header_version);
        expected["hedr_record_count"] = facts->hedr_record_count;
        expected["records"] = facts->records;
        expected["groups"] = facts->groups;
        expected["compressed_records"] = facts->compressed_records;
        expected["record_types"] = facts->record_types;
        expected["masters"] = facts->masters;
        expected["is_master"] = facts->is_master;
        expected["is_light"] = facts->is_light;
        expected["is_localized"] = facts->is_localized;
        expected["type_histogram_hash"] = facts->type_histogram_hash;
        expected["field_coverage_bp"] = facts->field_coverage_bp;
        std::fprintf(stderr, "  %-32s %llu bytes, %llu records, %llu groups, %llu errors\n",
                     file.c_str(), static_cast<unsigned long long>(facts->file_bytes),
                     static_cast<unsigned long long>(facts->records),
                     static_cast<unsigned long long>(facts->groups),
                     static_cast<unsigned long long>(facts->errors));
        if (facts->errors != 0) {
            std::fprintf(stderr, "    ^ NOT ZERO. Recording this is recording a bug.\n");
        }
    }
}

/// Textures are read from the set the archive pass already mounted.
void update_textures(json& install, const archive::ArchiveSet& set) {
    if (!install.contains("textures")) {
        return;
    }
    for (auto& expected : install.at("textures")) {
        const auto vpath = expected.at("vpath").get<std::string>();
        const auto facts = corpus::probe_texture(set, vpath);
        if (!facts) {
            std::fprintf(stderr, "  %-46s absent, keeping recorded values\n", vpath.c_str());
            continue;
        }
        expected["format"] = facts->format;
        expected["kind"] = facts->kind;
        expected["width"] = facts->width;
        expected["height"] = facts->height;
        expected["faces"] = facts->faces;
        expected["stored_levels"] = facts->stored_levels;
        expected["full_chain_levels"] = facts->full_chain_levels;
        expected["declared_bytes"] = facts->declared_bytes;
        expected["outcome"] = facts->outcome;
        expected["output_bytes"] = facts->output_bytes;
        expected["output_hash"] = facts->output_hash;
        std::fprintf(stderr, "  %-46s %ux%u %s %s %u/%u levels -> %s\n", vpath.c_str(),
                     facts->width, facts->height, facts->format.c_str(), facts->kind.c_str(),
                     facts->stored_levels, facts->full_chain_levels, facts->outcome.c_str());
    }
}

/// Meshes use the same mount.
void update_meshes(json& install, const archive::ArchiveSet& set) {
    if (!install.contains("meshes")) {
        return;
    }
    for (auto& expected : install.at("meshes")) {
        const auto vpath = expected.at("vpath").get<std::string>();
        const auto facts = corpus::probe_mesh(set, vpath);
        if (!facts) {
            std::fprintf(stderr, "  %-56s absent, keeping recorded values\n", vpath.c_str());
            continue;
        }
        expected["flavor"] = facts->flavor;
        expected["nif_version"] = facts->nif_version;
        expected["nif_stream"] = facts->nif_stream;
        expected["nodes"] = facts->nodes;
        expected["roots"] = facts->roots;
        expected["primitives"] = facts->primitives;
        expected["materials"] = facts->materials;
        expected["skins"] = facts->skins;
        expected["collision"] = facts->collision;
        expected["joints"] = facts->joints;
        expected["vertices"] = facts->vertices;
        expected["triangles"] = facts->triangles;
        expected["warnings"] = facts->warnings;
        expected["with_normals"] = facts->with_normals;
        expected["with_tangents"] = facts->with_tangents;
        expected["with_uvs"] = facts->with_uvs;
        expected["with_colors"] = facts->with_colors;
        expected["with_joints"] = facts->with_joints;
        expected["glb_bytes"] = facts->glb_bytes;
        expected["glb_hash"] = facts->glb_hash;
        expected["images"] = facts->images;
        expected["escaped_uris"] = facts->escaped_uris;
        if (facts->glb_control_bytes != 0 || !facts->glb_json_parses) {
            std::fprintf(stderr,
                         "    ^ %llu control byte(s) in the JSON chunk, %s. Recording this "
                         "is recording a file no consumer will open.\n",
                         static_cast<unsigned long long>(facts->glb_control_bytes),
                         facts->glb_json_parses ? "parses" : "DOES NOT PARSE");
        }
        std::fprintf(stderr,
                     "  %-56s %s %llu prims, %llu verts, %llu tris, %llu skins, %llu coll -> "
                     "%llu B\n",
                     vpath.c_str(), facts->flavor.c_str(),
                     static_cast<unsigned long long>(facts->primitives),
                     static_cast<unsigned long long>(facts->vertices),
                     static_cast<unsigned long long>(facts->triangles),
                     static_cast<unsigned long long>(facts->skins),
                     static_cast<unsigned long long>(facts->collision),
                     static_cast<unsigned long long>(facts->glb_bytes));
    }
}

/// Every script in the mounted archives.
void update_scripts(json& install, const archive::ArchiveSet& set) {
    if (!install.contains("scripts")) {
        return;
    }
    const auto facts = corpus::probe_scripts(set);
    auto& expected = install.at("scripts");
    expected["scripts"] = facts.scripts;
    expected["objects"] = facts.objects;
    expected["functions"] = facts.functions;
    expected["natives"] = facts.natives;
    expected["instructions"] = facts.instructions;
    expected["asset_hash"] = facts.asset_hash;
    std::fprintf(stderr,
                 "  scripts: %llu decoded, %llu failed, %llu functions (%llu native), %llu "
                 "instructions\n",
                 static_cast<unsigned long long>(facts.scripts),
                 static_cast<unsigned long long>(facts.failed),
                 static_cast<unsigned long long>(facts.functions),
                 static_cast<unsigned long long>(facts.natives),
                 static_cast<unsigned long long>(facts.instructions));
    if (facts.failed != 0) {
        std::fprintf(stderr, "    ^ NOT ZERO on a vanilla install -- recording this is recording a bug.\n");
    }
}

/// Every Havok file in the mounted archives.
void update_animations(json& install, const archive::ArchiveSet& set) {
    if (!install.contains("animations")) {
        return;
    }
    const auto facts = corpus::probe_animations(set);
    auto& expected = install.at("animations");
    expected["files"] = facts.files;
    expected["tagfiles"] = facts.tagfiles;
    expected["skeletons"] = facts.skeletons;
    expected["clips"] = facts.clips;
    expected["frames"] = facts.frames;
    expected["annotations"] = facts.annotations;
    expected["asset_hash"] = facts.asset_hash;
    std::fprintf(stderr, "  animations: %llu files, %llu failed, %llu skeletons, %llu clips, %llu frames\n",
                 static_cast<unsigned long long>(facts.files), static_cast<unsigned long long>(facts.failed),
                 static_cast<unsigned long long>(facts.skeletons), static_cast<unsigned long long>(facts.clips),
                 static_cast<unsigned long long>(facts.frames));
    if (facts.failed != 0) {
        std::fprintf(stderr, "    ^ NOT ZERO on a vanilla install -- recording this is recording a bug.\n");
    }
}

/// Field census over the same mount and the plugin pin's list (totals only mean
/// something over the whole set).
void update_forms(const std::filesystem::path& dir, json& install,
                  const archive::ArchiveSet& set) {
    if (!install.contains("forms")) {
        return;
    }

    std::vector<std::string> plugins;
    for (const auto& expected : install.at("plugins")) {
        const auto file = expected.at("file").get<std::string>();
        std::error_code ec;
        if (!std::filesystem::exists(dir / file, ec)) {
            std::fprintf(stderr, "  forms: %s is absent -- census not updated\n", file.c_str());
            return;
        }
        plugins.push_back(file);
    }

    const auto facts = corpus::probe_forms(dir, set, plugins);
    if (!facts) {
        std::fprintf(stderr, "  forms: nothing to walk -- not updated\n");
        return;
    }

    auto& expected = install.at("forms");
    expected["localized"] = facts->localized;
    expected["parsed"] = facts->parsed;
    expected["types_defined"] = facts->types_defined;
    expected["types_seen"] = facts->types_seen;
    expected["string_tables"] = facts->string_tables;
    expected["table_entries"] = facts->table_entries;
    expected["resolved_strings"] = facts->resolved_strings;
    expected["unresolved_strings"] = facts->unresolved_strings;
    expected["unhandled"] = facts->unhandled;
    expected["unhandled_fields"] = facts->unhandled_fields;
    expected["unhandled_hash"] = facts->unhandled_hash;
    expected["type_counts_hash"] = facts->type_counts_hash;

    std::fprintf(stderr,
                 "  forms: %llu plugin(s), %llu parsed, %llu failed, %llu of %llu defined "
                 "types seen\n",
                 static_cast<unsigned long long>(facts->plugins),
                 static_cast<unsigned long long>(facts->parsed),
                 static_cast<unsigned long long>(facts->failed),
                 static_cast<unsigned long long>(facts->types_seen),
                 static_cast<unsigned long long>(facts->types_defined));
    std::fprintf(stderr,
                 "         %llu unhandled field occurrence(s) across %llu (type, field) "
                 "pair(s) -- the work list, not a bug\n",
                 static_cast<unsigned long long>(facts->unhandled),
                 static_cast<unsigned long long>(facts->unhandled_fields));
    std::fprintf(stderr,
                 "         strings: %llu table(s), %llu entries, %llu resolved, %llu "
                 "unresolved\n",
                 static_cast<unsigned long long>(facts->string_tables),
                 static_cast<unsigned long long>(facts->table_entries),
                 static_cast<unsigned long long>(facts->resolved_strings),
                 static_cast<unsigned long long>(facts->unresolved_strings));
    // `unresolved_strings` may legitimately be nonzero; these three may not.
    if (facts->failed != 0 || facts->leftover != 0 || facts->unreadable != 0) {
        std::fprintf(stderr,
                     "    ^ %llu failed, %llu leftover byte-run(s) across %llu field(s), "
                     "%llu unreadable. NOT ZERO on a vanilla install -- recording this is "
                     "recording a bug.\n",
                     static_cast<unsigned long long>(facts->failed),
                     static_cast<unsigned long long>(facts->leftover),
                     static_cast<unsigned long long>(facts->leftover_fields),
                     static_cast<unsigned long long>(facts->unreadable));
    }
    if (facts->localized != 0 && facts->string_tables == 0) {
        std::fprintf(stderr, "    ^ %llu localized plugin(s) and not one string table: the "
                             "archive holding them is not mounted, so every string index "
                             "went unasked rather than unresolved.\n",
                     static_cast<unsigned long long>(facts->localized));
    }
}

/// The merge uses the same mount; it needs every archive to resolve names (SE
/// has 23).
void update_merge(const std::filesystem::path& dir, json& install,
                  const archive::ArchiveSet& set) {
    if (!install.contains("merge")) {
        return;
    }
    auto& expected = install.at("merge");

    std::vector<std::string> pinned;
    for (const auto& form : expected.at("forms")) {
        pinned.push_back(form.at("form").get<std::string>());
    }

    const bethconv::test::TempDir scratch;
    const auto facts = corpus::probe_merge(dir, set, pinned, scratch.path());
    if (!facts) {
        std::fprintf(stderr, "  merge: the load order will not build -- not updated\n");
        return;
    }

    expected["plugins"] = facts->plugins;
    expected["visited"] = facts->visited;
    expected["forms"] = json::array();
    expected["form_count"] = facts->forms;
    expected["collapsed"] = facts->collapsed;
    expected["deleted"] = facts->deleted;
    expected["injected"] = facts->injected;
    expected["unresolved"] = facts->unresolved;
    expected["problems"] = facts->problems;
    expected["string_tables"] = facts->string_tables;
    expected["type_counts_hash"] = facts->type_counts_hash;
    for (const auto& [name, pin] : {std::pair{"snapshot", &facts->snapshot},
                                    std::pair{"world", &facts->world}}) {
        if (pin->empty()) {
            std::fprintf(stderr, "  merge: %s was not written or does not reopen\n", name);
            continue;
        }
        json out = json::object();
        for (const auto& [key, value] : *pin) {
            out[key] = value;
        }
        expected[name] = std::move(out);
    }

    for (const auto& form : facts->forms_pinned) {
        expected["forms"].push_back(json{{"form", form.form},
                                         {"type", form.type},
                                         {"parent", form.parent},
                                         {"winner", form.winner},
                                         {"owner", form.owner},
                                         {"overrides", form.overrides},
                                         {"flags", form.flags},
                                         {"deleted", form.deleted},
                                         {"injected", form.injected},
                                         {"present", form.present},
                                         {"payload_bytes", form.payload_bytes},
                                         {"payload_hash", form.payload_hash},
                                         {"editor_id_hash", form.editor_id_hash},
                                         {"name_hash", form.name_hash},
                                         {"name_from_table", form.name_from_table}});
    }

    std::fprintf(stderr,
                 "  merge: %llu plugins, %llu records -> %llu forms, %llu collapsed, "
                 "%llu deleted, %llu injected\n",
                 static_cast<unsigned long long>(facts->plugins),
                 static_cast<unsigned long long>(facts->visited),
                 static_cast<unsigned long long>(facts->forms),
                 static_cast<unsigned long long>(facts->collapsed),
                 static_cast<unsigned long long>(facts->deleted),
                 static_cast<unsigned long long>(facts->injected));
    std::fprintf(stderr, "         second pass forwarded %llu, index claims %llu -- %s\n",
                 static_cast<unsigned long long>(facts->forwarded),
                 static_cast<unsigned long long>(facts->forms),
                 facts->forwarded == facts->forms ? "reconciled" : "MISMATCH");
    // `unresolved` and `problems` are 1 on vanilla (GMST 0x0123C00E); these two
    // must be 0.
    if (facts->errors != 0 || facts->unparented != 0) {
        std::fprintf(stderr,
                     "    ^ %llu errors, %llu unparented. NOT ZERO on a vanilla folder "
                     "-- recording this is recording a bug.\n",
                     static_cast<unsigned long long>(facts->errors),
                     static_cast<unsigned long long>(facts->unparented));
    }
    std::fprintf(stderr, "         %llu unresolved, %llu problems, %llu string tables\n",
                 static_cast<unsigned long long>(facts->unresolved),
                 static_cast<unsigned long long>(facts->problems),
                 static_cast<unsigned long long>(facts->string_tables));

    // Suggest one candidate per merge class, so nobody has to hunt for e.g. a
    // deleted record by hand.
    if (facts->forms_pinned.empty()) {
        std::fprintf(stderr, "    no forms pinned yet. Candidates in this install:\n");
        for (const auto& suggestion : facts->suggestions) {
            std::fprintf(stderr, "      %s\n", suggestion.c_str());
        }
    }
    for (const auto& form : facts->forms_pinned) {
        if (!form.present) {
            std::fprintf(stderr, "    pinned form is not in this merged world: %s\n",
                         form.form.c_str());
        }
    }
}

/// The convert pin uses the same mount. Two runs per install, since no single
/// filter covers both cases (see tests/corpus/README.md).
void update_convert(const std::filesystem::path& dir, json& install,
                    const bethconv::archive::ArchiveSet& set) {
    if (!install.contains("convert")) {
        return;
    }
    for (auto& expected : install.at("convert")) {
        const auto name = expected.at("name").get<std::string>();

        // Fresh directory, or the run would be all dedupe.
        const bethconv::test::TempDir out;
        const auto facts =
            corpus::probe_convert(dir, set, out.path() / "pack",
                                  expected.at("filter").get<std::string>(),
                                  expected.at("limit").get<std::size_t>());
        if (!facts) {
            std::fprintf(stderr, "  convert %s: the load order will not build -- not updated\n",
                         name.c_str());
            continue;
        }

        expected["unique_paths"] = facts->unique_paths;
        expected["considered"] = facts->considered;
        expected["inputs"] = facts->inputs;
        expected["converted"] = facts->converted;
        expected["deduped"] = facts->deduped;
        expected["deferred"] = facts->deferred;
        expected["deferred_kinds"] = facts->deferred_kinds;
        expected["meshes"] = facts->meshes;
        expected["textures"] = facts->textures;
        expected["scripts"] = facts->scripts;
        expected["lod"] = facts->lod;
        expected["distinct_assets"] = facts->distinct_assets;
        expected["index_entries"] = facts->index_entries;
        expected["warnings"] = facts->warnings;
        expected["source_bytes"] = facts->source_bytes;
        expected["asset_bytes"] = facts->asset_bytes;
        expected["dedupe_saved_bytes"] = facts->dedupe_saved_bytes;
        expected["glb_escaped_uris"] = facts->glb_escaped_uris;
        expected["manifest_hash"] = facts->manifest_hash;
        expected["index_hash"] = facts->index_hash;
        expected["report_hash"] = facts->report_hash;

        std::fprintf(stderr,
                     "  convert %s ('%s'): %llu considered -> %llu assets (%llu meshes, %llu "
                     "textures, %llu scripts), %llu deferred across %llu extensions\n",
                     name.c_str(), facts->filter.c_str(),
                     static_cast<unsigned long long>(facts->considered),
                     static_cast<unsigned long long>(facts->converted),
                     static_cast<unsigned long long>(facts->meshes),
                     static_cast<unsigned long long>(facts->textures),
                     static_cast<unsigned long long>(facts->scripts),
                     static_cast<unsigned long long>(facts->deferred),
                     static_cast<unsigned long long>(facts->deferred_kinds));
        std::fprintf(stderr,
                     "        %llu GLB(s) checked: %llu control byte(s), %llu unparseable, "
                     "%llu bad container, %llu escaped image URI(s)\n",
                     static_cast<unsigned long long>(facts->glb_checked),
                     static_cast<unsigned long long>(facts->glb_control_bytes),
                     static_cast<unsigned long long>(facts->glb_unparseable),
                     static_cast<unsigned long long>(facts->glb_bad_container),
                     static_cast<unsigned long long>(facts->glb_escaped_uris));
        // These must be zero; otherwise the pack's meshes would not open.
        if (facts->failed != 0 || facts->glb_control_bytes != 0 ||
            facts->glb_unparseable != 0 || facts->glb_bad_container != 0) {
            std::fprintf(stderr,
                         "    ^ %llu failed. NOT ZERO on a vanilla install -- recording this "
                         "is recording a bug.\n",
                         static_cast<unsigned long long>(facts->failed));
        }
        if (facts->glb_checked == 0) {
            std::fprintf(stderr, "    ^ no GLB was examined, so the two chunk assertions "
                                 "checked nothing. Widen the filter.\n");
        }

        if (!expected.contains("view")) {
            continue;
        }
        auto& want_view = expected.at("view");
        // View of the pack just built.
        const bethconv::test::TempDir viewed;
        const auto view = corpus::probe_view(out.path() / "pack", viewed.path() / "view",
                                             want_view.at("filter").get<std::string>(),
                                             want_view.at("limit").get<std::size_t>());
        if (!view) {
            std::fprintf(stderr, "  view %s: the pack will not open -- not updated\n",
                         name.c_str());
            continue;
        }
        want_view["index_entries"] = view->index_entries;
        want_view["considered"] = view->considered;
        want_view["meshes"] = view->meshes;
        want_view["textures"] = view->textures;
        want_view["scripts"] = view->scripts;
        want_view["lod"] = view->lod;
        want_view["linked"] = view->linked;
        want_view["copied"] = view->copied;
        want_view["written"] = view->written;
        want_view["mesh_links"] = view->mesh_links;
        want_view["pulled_in"] = view->pulled_in;
        want_view["image_refs"] = view->image_refs;
        want_view["image_refs_resolved"] = view->image_refs_resolved;
        want_view["image_refs_dangling"] = view->image_refs_dangling;
        want_view["bytes"] = view->bytes;

        std::fprintf(stderr,
                     "  view %s ('%s'): %llu of %llu entries -> %llu written, %llu mesh "
                     "links, %llu linked, %llu copied, %llu pulled in\n",
                     name.c_str(), view->filter.c_str(),
                     static_cast<unsigned long long>(view->considered),
                     static_cast<unsigned long long>(view->index_entries),
                     static_cast<unsigned long long>(view->written),
                     static_cast<unsigned long long>(view->mesh_links),
                     static_cast<unsigned long long>(view->linked),
                     static_cast<unsigned long long>(view->copied),
                     static_cast<unsigned long long>(view->pulled_in));
        std::fprintf(stderr,
                     "        %llu image refs: %llu resolved, %llu dangling -- mostly "
                     "textures this filter did not select, not missing files\n",
                     static_cast<unsigned long long>(view->image_refs),
                     static_cast<unsigned long long>(view->image_refs_resolved),
                     static_cast<unsigned long long>(view->image_refs_dangling));
        if (view->failed != 0 || view->glb_control_bytes != 0 || view->glb_unparseable != 0 ||
            view->glb_bad_container != 0) {
            std::fprintf(stderr,
                         "    ^ %llu failed, %llu control byte(s), %llu unparseable. A "
                         "vanilla pack materializes whole -- recording this is recording a "
                         "bug.\n",
                         static_cast<unsigned long long>(view->failed),
                         static_cast<unsigned long long>(view->glb_control_bytes),
                         static_cast<unsigned long long>(view->glb_unparseable));
        }
    }
}

void update_load_order(const std::filesystem::path& dir, json& install) {
    if (!install.contains("load_order")) {
        return;
    }
    const auto facts = corpus::probe_load_order(dir);
    json plugins = json::array();
    for (const auto& plugin : facts.plugins) {
        plugins.push_back(json{{"name", plugin.name},
                               {"index", plugin.index},
                               {"flags", plugin.flags},
                               {"masters", plugin.masters}});
    }
    install["load_order"]["plugins"] = std::move(plugins);
    install["load_order"]["normal_used"] = facts.normal_used;
    install["load_order"]["light_used"] = facts.light_used;
    std::fprintf(stderr, "  load order: %zu plugins, %llu normal, %llu light, %llu problems\n",
                 facts.plugins.size(), static_cast<unsigned long long>(facts.normal_used),
                 static_cast<unsigned long long>(facts.light_used),
                 static_cast<unsigned long long>(facts.problems));
    if (facts.problems != 0) {
        std::fprintf(stderr, "    ^ NOT ZERO on a vanilla folder. Recording this is "
                             "recording a bug.\n");
    }
}

void update_archives(const std::filesystem::path& dir, json& install) {
    std::vector<std::string> names;
    for (const auto& expected : install.at("archives")) {
        names.push_back(expected.at("file").get<std::string>());
    }

    archive::ArchiveSet set;
    const auto scan = corpus::probe_archives(dir, names, set);

    for (const auto& facts : scan.archives) {
        for (auto& expected : install.at("archives")) {
            if (expected.at("file").get<std::string>() != facts.file) {
                continue;
            }
            expected["kind"] = facts.kind;
            expected["version"] = facts.version;
            expected["files"] = facts.files;
        }
    }
    for (const auto& name : scan.missing) {
        std::fprintf(stderr, "  %-32s absent, keeping recorded values\n", name.c_str());
    }

    if (scan.missing.empty()) {
        install["unique_paths"] = scan.unique_paths;
        install["shadowed"] = scan.shadowed;
    }
    std::fprintf(stderr, "  %zu archives -> %llu unique paths, %llu contested\n",
                 scan.archives.size(), static_cast<unsigned long long>(scan.unique_paths),
                 static_cast<unsigned long long>(scan.shadowed));

    // A recorded-broken path that now reads fine means the reader changed or
    // this is a different install.
    std::vector<std::string> known_bad;
    for (const auto& vpath : install.at("expected_read_failures")) {
        known_bad.push_back(vpath.get<std::string>());
    }
    if (!known_bad.empty()) {
        const auto check = corpus::check_reads(set, known_bad);
        for (const auto& vpath : check.unresolved) {
            std::fprintf(stderr, "  known-bad entry is not in this install: %s\n",
                         vpath.c_str());
        }
        for (const auto& vpath : check.succeeded) {
            std::fprintf(stderr,
                         "  known-bad entry now reads successfully: %s\n"
                         "    investigate before committing -- do NOT just delete it\n",
                         vpath.c_str());
        }
    }

    update_textures(install, set);
    update_meshes(install, set);
    update_forms(dir, install, set);
    update_scripts(install, set);
    update_animations(install, set);
    update_merge(dir, install, set);
    update_convert(dir, install, set);
}

} // namespace

int main() {
    json expectations;
    {
        std::ifstream in(BETHCONV_CORPUS_EXPECTATIONS);
        if (!in.good()) {
            std::fprintf(stderr, "cannot read %s\n", BETHCONV_CORPUS_EXPECTATIONS);
            return 2;
        }
        expectations = json::parse(in);
    }

    for (auto& install : expectations.at("installs")) {
        const auto id = install.at("id").get<std::string>();
        const auto var = install.at("env").get<std::string>();
        const auto value = corpus::env(var.c_str());

        std::error_code ec;
        if (!value || !std::filesystem::is_directory(*value, ec)) {
            std::fprintf(stderr, "%s: %s unset or not a directory, left as recorded\n",
                         id.c_str(), var.c_str());
            continue;
        }

        std::fprintf(stderr, "%s: %s\n", id.c_str(), value->c_str());
        update_plugins(*value, install);
        update_load_order(*value, install);
        update_archives(*value, install);
    }

    std::cout << expectations.dump(2) << '\n';
    return 0;
}
