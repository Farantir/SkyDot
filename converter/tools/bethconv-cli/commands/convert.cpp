// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"
#include "front_end.hpp"

#include "bethconv/io/json_text.hpp"
#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/pack/convert.hpp"
#include "bethconv/pack/inputs.hpp"
#include "bethconv/texture/bc_encode.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bethconv::cli {
namespace {

/// Everything `convert` takes from the command line.
struct ConvertArgs {
    std::filesystem::path data_dir;
    std::filesystem::path list_file;
    std::vector<std::filesystem::path> sources;
    std::filesystem::path mo2;
    std::string mo2_profile;
    std::filesystem::path out;
    std::string language;
    std::string filter;
    std::size_t limit = 0;
    bool no_records = false;
    bool no_meshes = false;
    bool no_textures = false;
    bool no_scripts = false;
    bool no_lod = false;
    bool no_animations = false;
    bool no_mip_fix = false;
    bool no_collision = false;
    bool no_skinning = false;
    bool keep_z_up = false;
    float unit_scale = bethconv::mesh::k_default_unit_scale;
    std::uint32_t max_texture_size = 0;
    bethconv::texture::Encoding encoding = bethconv::texture::Encoding::keep;
    bool hash_archives = false;
    bool prune = false;
    bethconv::pack::StoreLayout layout = bethconv::pack::StoreLayout::blob;
    bool allow_slow_target = false;
    bool quiet = false;
    /// Progress and the result as JSON lines on stdout (docs/cli-json.md);
    /// the text goes to stderr.
    bool json = false;
};

/// One command, one pack. The work is in `pack/convert.cpp`; this mounts,
/// builds the load order and prints.
int cmd_convert(const ConvertArgs& args) {
    using bethconv::cli::emit;
    using nlohmann::ordered_json;
    using bethconv::io::path_text;
    // With --json, stdout carries only JSON lines.
    FILE* text = args.json ? stderr : stdout;
    const auto started = std::chrono::steady_clock::now();
    const auto seconds = [&started] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    const auto fail = [&](int code, const std::string& message) {
        std::fprintf(stderr, "error: %s\n", message.c_str());
        if (args.json) {
            emit(ordered_json{{"event", "error"},
                              {"json_version", bethconv::cli::k_json_version},
                              {"message", message},
                              {"exit", code}});
        }
        return code;
    };

    const auto verdict = bethconv::cli::check_target(
        args.out, args.layout == bethconv::pack::StoreLayout::loose, args.allow_slow_target);
    if (verdict.level == bethconv::cli::TargetLevel::refuse) {
        return fail(2, verdict.reason);
    }
    if (verdict.level == bethconv::cli::TargetLevel::warn) {
        std::fprintf(stderr, "warning: %s\n", verdict.reason.c_str());
    }

    // ---- what to mount and the load order ---------------------------------
    std::function<void(std::size_t, std::size_t)> mounted;
    if (args.json) {
        mounted = [&seconds](std::size_t done, std::size_t total) {
            emit(ordered_json{{"event", "progress"},
                              {"phase", "mount"},
                              {"done", done},
                              {"total", total},
                              {"elapsed", seconds()}});
        };
    }
    auto prepared = bethconv::pack::prepare_inputs(
        bethconv::pack::InputSpec{.data_dir = args.data_dir,
                                  .list_file = args.list_file,
                                  .mo2 = args.mo2,
                                  .mo2_profile = args.mo2_profile,
                                  .sources = args.sources},
        mounted);
    if (!prepared) {
        return fail(1, prepared.error().to_string());
    }
    auto& input = prepared->input;
    const auto& order = prepared->order;
    const auto& set = prepared->set;

    if (const auto& profile = prepared->profile) {
        std::fprintf(text, "profile %s: %zu mods enabled, %zu disabled, %zu missing\n",
                     profile->name.c_str(), profile->mods.size(), profile->disabled,
                     profile->missing.size());
        for (const auto& name : profile->missing) {
            std::fprintf(text, "  missing mod folder: %s\n", name.c_str());
        }
    }
    std::fprintf(text, "%zu plugins in the order, %zu problems\n", order.entries().size(),
                 order.problems().size());
    for (const auto& problem : order.problems()) {
        std::fprintf(text, "  %s\n", problem.to_string().c_str());
    }
    for (const auto& failure : prepared->mount_failures) {
        std::fprintf(stderr, "warning: skipping %s\n", failure.c_str());
    }
    if (const auto& plan = prepared->plan) {
        std::fprintf(text, "mounted %zu archives and %zu folders\n", plan->archives.size(),
                     plan->loose.size());
    }
    if (!prepared->unloaded_archives.empty()) {
        std::fprintf(text, "%zu mod archives not mounted: no loaded plugin is named like them\n",
                     prepared->unloaded_archives.size());
    }
    std::fprintf(text, "%zu unique virtual paths\n", set.unique_paths());

    if (args.json) {
        auto problems = ordered_json::array();
        for (const auto& problem : order.problems()) {
            problems.push_back(problem.to_string());
        }
        auto failures = ordered_json::array();
        for (const auto& failure : prepared->mount_failures) {
            failures.push_back(failure);
        }
        auto unloaded = ordered_json::array();
        for (const auto& path : prepared->unloaded_archives) {
            unloaded.push_back(path_text(path));
        }
        emit(ordered_json{{"event", "start"},
                          {"json_version", bethconv::cli::k_json_version},
                          {"out", path_text(args.out)},
                          {"target_warning", verdict.reason},
                          {"input", ordered_json{{"kind", input.kind},
                                                 {"edition", input.edition},
                                                 {"data", input.data},
                                                 {"plugin_list", input.plugin_list},
                                                 {"mo2_instance", input.mo2_instance},
                                                 {"mo2_profile", bethconv::io::json_text(input.mo2_profile)},
                                                 {"mods", input.mods}}},
                          {"plugins", order.entries().size()},
                          {"load_order_problems", std::move(problems)},
                          {"sources", set.sources().size()},
                          {"mount_failures", std::move(failures)},
                          {"unloaded_archives", std::move(unloaded)},
                          {"unique_paths", set.unique_paths()}});
    }

    bethconv::pack::ConvertOptions options;
    options.out = args.out;
    options.converter = std::string("bethconv ") + BETHCONV_VERSION;
    options.language = args.language;
    options.input = std::move(input);
    options.write_records = !args.no_records;
    options.convert_meshes = !args.no_meshes;
    options.convert_textures = !args.no_textures;
    options.convert_scripts = !args.no_scripts;
    options.convert_lod = !args.no_lod;
    options.convert_animations = !args.no_animations;
    options.fix_mip_tail = !args.no_mip_fix;
    options.max_texture_size = args.max_texture_size;
    options.texture_encoding = args.encoding;
    options.mesh_read.read_collision = !args.no_collision;
    options.mesh_read.read_skinning = !args.no_skinning;
    options.mesh_write.convert_to_y_up = !args.keep_z_up;
    options.mesh_write.unit_scale = args.unit_scale;
    options.filter = args.filter;
    options.limit = args.limit;
    options.hash_archives = args.hash_archives;
    options.prune_orphans = args.prune;
    options.layout = args.layout;

    if (args.json) {
        // A bar needs more than one step per 2,000 files (~1.5 s).
        options.progress_interval = 250;
        options.progress = [&seconds](const std::string& phase, std::uint64_t done,
                                      std::uint64_t total) {
            emit(ordered_json{{"event", "progress"},
                              {"phase", phase},
                              {"done", done},
                              {"total", total},
                              {"elapsed", seconds()}});
        };
    } else if (!args.quiet) {
        options.progress = [&seconds](const std::string& phase, std::uint64_t done,
                                      std::uint64_t total) {
            std::printf("\r%-8s %llu/%llu  %.0fs", phase.c_str(),
                        static_cast<unsigned long long>(done),
                        static_cast<unsigned long long>(total), seconds());
            std::fflush(stdout);
        };
    }

    auto result = bethconv::pack::convert(set, order, options);
    if (!args.quiet && !args.json) {
        std::printf("\r%-40s\r", "");
    }
    if (!result) {
        return fail(1, result.error().to_string());
    }
    const auto elapsed = seconds();

    const auto& stats = result->pack;
    std::fprintf(text, "\nwrote %s\n", args.out.string().c_str());
    if (result->snapshot) {
        std::fprintf(text, "  records.fb    %llu forms, %.1f MiB\n",
                     static_cast<unsigned long long>(result->snapshot->forms),
                     static_cast<double>(result->snapshot->file_bytes) / (1024.0 * 1024.0));
    }
    if (result->world) {
        const auto& w = *result->world;
        std::fprintf(text, "  world.fb      %llu cells, %llu refs, %llu worldspaces, %llu with terrain "
                     "(%llu layers), %llu land textures, %llu water types, %llu climates, "
                     "%llu weathers, %.1f MiB\n",
                     static_cast<unsigned long long>(w.cells),
                     static_cast<unsigned long long>(w.refs),
                     static_cast<unsigned long long>(w.worlds),
                     static_cast<unsigned long long>(w.terrains),
                     static_cast<unsigned long long>(w.terrain_layers),
                     static_cast<unsigned long long>(w.land_textures),
                     static_cast<unsigned long long>(w.waters),
                     static_cast<unsigned long long>(w.climates),
                     static_cast<unsigned long long>(w.weathers),
                     static_cast<double>(w.file_bytes) / (1024.0 * 1024.0));
        std::fprintf(text, "                %llu doors, %llu scripts, %llu locks, %llu linked refs, "
                     "%llu activate parents, %llu primitives\n",
                     static_cast<unsigned long long>(w.doors),
                     static_cast<unsigned long long>(w.scripts),
                     static_cast<unsigned long long>(w.locks),
                     static_cast<unsigned long long>(w.links),
                     static_cast<unsigned long long>(w.activate_parents),
                     static_cast<unsigned long long>(w.primitives));
        std::fprintf(text, "                %llu quests (%llu aliases, %llu stage fragments), "
                     "%llu globals, %llu placed actors\n",
                     static_cast<unsigned long long>(w.quests),
                     static_cast<unsigned long long>(w.quest_aliases),
                     static_cast<unsigned long long>(w.quest_fragments),
                     static_cast<unsigned long long>(w.globals),
                     static_cast<unsigned long long>(w.actors));
        std::fprintf(text, "                %llu precipitation types, %llu regions with weather\n",
                     static_cast<unsigned long long>(w.precipitations),
                     static_cast<unsigned long long>(w.regions));
        std::fprintf(text, "                %llu navmeshes (%llu triangles, %llu orphaned)\n",
                     static_cast<unsigned long long>(w.navmeshes),
                     static_cast<unsigned long long>(w.nav_triangles),
                     static_cast<unsigned long long>(w.orphan_navmeshes));
        std::fprintf(text, "                %llu actors of %llu NPCs, %llu races, %llu armors (%llu addons), "
                           "%llu outfits, %llu leveled lists, %llu packages\n",
                     static_cast<unsigned long long>(w.actors), static_cast<unsigned long long>(w.npcs),
                     static_cast<unsigned long long>(w.races), static_cast<unsigned long long>(w.armors),
                     static_cast<unsigned long long>(w.armor_addons), static_cast<unsigned long long>(w.outfits),
                     static_cast<unsigned long long>(w.leveled_lists),
                     static_cast<unsigned long long>(w.packages));
        std::fprintf(text, "                %llu unresolved, %llu parse errors, %llu script errors\n",
                     static_cast<unsigned long long>(w.unresolved),
                     static_cast<unsigned long long>(w.parse_errors),
                     static_cast<unsigned long long>(w.script_errors));
    }
    std::fprintf(text, "  assets        %llu written, %llu deduped, %llu distinct "
                 "(%llu meshes, %llu textures, %llu scripts, %llu LOD, %llu animations)\n",
                 static_cast<unsigned long long>(stats.converted),
                 static_cast<unsigned long long>(stats.deduped),
                 static_cast<unsigned long long>(stats.distinct_assets),
                 static_cast<unsigned long long>(stats.meshes),
                 static_cast<unsigned long long>(stats.textures),
                 static_cast<unsigned long long>(stats.scripts),
                 static_cast<unsigned long long>(stats.lod),
                 static_cast<unsigned long long>(stats.animations));
    std::fprintf(text, "                %.1f MiB written, %.1f MiB not re-converted\n",
                 static_cast<double>(stats.asset_bytes) / (1024.0 * 1024.0),
                 static_cast<double>(stats.dedupe_saved_bytes) / (1024.0 * 1024.0));
    if (args.encoding != bethconv::texture::Encoding::keep) {
        std::fprintf(text, "  textures      %llu uncompressed ones encoded (%s), %llu left "
                     "uncompressed (listed in report.json)\n",
                     static_cast<unsigned long long>(result->textures_encoded),
                     std::string(bethconv::texture::to_string(args.encoding)).c_str(),
                     static_cast<unsigned long long>(result->textures_not_encoded));
    }
    if (args.max_texture_size != 0) {
        std::fprintf(text, "  textures      %llu limited to %u px (%.1f MiB saved), %llu kept larger "
                     "(no smaller level stored; listed in report.json)\n",
                     static_cast<unsigned long long>(result->textures_shrunk),
                     args.max_texture_size,
                     static_cast<double>(result->texture_bytes_saved) / (1024.0 * 1024.0),
                     static_cast<unsigned long long>(result->textures_kept_large));
    }
    std::fprintf(text, "  store         %s, %.1f MiB\n", std::string(to_string(args.layout)).c_str(),
                 static_cast<double>(stats.store_bytes) / (1024.0 * 1024.0));
    std::fprintf(text, "  vpath.idx     %llu entries, %.1f MiB\n",
                 static_cast<unsigned long long>(stats.index_entries),
                 static_cast<double>(stats.index_bytes) / (1024.0 * 1024.0));
    std::fprintf(text, "  report.json   %llu failed, %llu warnings\n",
                 static_cast<unsigned long long>(stats.failed),
                 static_cast<unsigned long long>(stats.warnings));
    std::fprintf(text, "  deferred      %llu inputs this pass does not convert, across "
                 "%llu extensions\n",
                 static_cast<unsigned long long>(stats.deferred),
                 static_cast<unsigned long long>(stats.deferred_kinds));
    if (stats.orphaned_assets != 0) {
        std::fprintf(text, "  %llu asset(s) on disk this run's index does not name%s\n",
                     static_cast<unsigned long long>(stats.orphaned_assets),
                     args.prune ? " -- pruned" : " -- pass --prune to remove them");
    }
    std::fprintf(text, "in %.1fs\n", elapsed);

    // Show a few failures on the terminal; the full list is in report.json.
    if (stats.failed != 0) {
        std::fprintf(text, "\nfirst %zu of %llu failures -- report.json has every one:\n",
                     result->first_failures.size(),
                     static_cast<unsigned long long>(stats.failed));
        for (const auto& failure : result->first_failures) {
            std::fprintf(text, "  %-8s %s\n", failure.stage.c_str(), failure.detail.c_str());
        }
    }
    const int exit_code = stats.failed == 0 ? 0 : 1;
    if (args.json) {
        auto failures = ordered_json::array();
        for (const auto& failure : result->first_failures) {
            failures.push_back(ordered_json{{"vpath", bethconv::io::json_text(failure.vpath)},
                                            {"stage", failure.stage},
                                            {"detail", bethconv::io::json_text(failure.detail)}});
        }
        emit(ordered_json{
            {"event", "done"},
            {"json_version", bethconv::cli::k_json_version},
            {"exit", exit_code},
            {"elapsed", elapsed},
            {"out", path_text(args.out)},
            {"forms", result->snapshot ? result->snapshot->forms : 0},
            {"cells", result->world ? result->world->cells : 0},
            {"assets", ordered_json{{"written", stats.converted},
                                    {"deduped", stats.deduped},
                                    {"distinct", stats.distinct_assets},
                                    {"meshes", stats.meshes},
                                    {"textures", stats.textures},
                                    {"scripts", stats.scripts},
                                    {"lod", stats.lod},
                                    {"animations", stats.animations},
                                    {"bytes_written", stats.asset_bytes},
                                    {"store_bytes", stats.store_bytes}}},
            {"textures", ordered_json{{"max_size", args.max_texture_size},
                                      {"shrunk", result->textures_shrunk},
                                      {"kept_large", result->textures_kept_large},
                                      {"uncompressed", bethconv::texture::to_string(args.encoding)},
                                      {"encoded", result->textures_encoded},
                                      {"not_encoded", result->textures_not_encoded},
                                      {"bytes_saved", result->texture_bytes_saved}}},
            {"failed", stats.failed},
            {"warnings", stats.warnings},
            {"orphaned_assets", stats.orphaned_assets},
            {"pruned", args.prune},
            {"first_failures", std::move(failures)}});
    }
    return exit_code;
}

/// What the command line binds to; `convert_args` turns it into ConvertArgs.
struct ConvertCli {
    std::filesystem::path data;
    std::filesystem::path list;
    std::vector<std::filesystem::path> sources;
    std::filesystem::path out{"pack"};
    std::string language{std::string(bethconv::record::k_default_language)};
    std::string filter;
    std::size_t limit = 0;
    bool no_records = false;
    bool no_meshes = false;
    bool no_textures = false;
    bool no_scripts = false;
    bool no_lod = false;
    bool no_animations = false;
    bool no_mip_fix = false;
    bool no_collision = false;
    bool no_skinning = false;
    bool keep_z_up = false;
    float unit_scale = bethconv::mesh::k_default_unit_scale;
    bool hash_archives = false;
    bool prune = false;
    std::string store = "blob";
    bool allow_slow_target = false;
    bool quiet = false;
    bool json = false;
    std::uint32_t max_texture = 0;
    std::string encode = "keep";
    std::filesystem::path mo2;
    std::string profile;
};

} // namespace

void register_convert(CLI::App& app) {
    auto args = std::make_shared<ConvertCli>();
    auto* convert = app.add_subcommand("convert", "Convert an install into a pack");
    convert->add_option("--data", args->data, "The game's Data folder")
        ->required()
        ->check(CLI::ExistingDirectory);
    convert->add_option("--list", args->list, "plugins.txt or loadorder.txt")
        ->check(CLI::ExistingFile);
    convert->add_option("--source", args->sources,
                        "What to mount; default is every archive in --data plus loose files")
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    convert->add_option("-o,--out", args->out, "Pack directory to write")
        ->default_val("pack");
    convert->add_option("--language", args->language, "Which .STRINGS language to resolve")
        ->default_val(std::string(bethconv::record::k_default_language));
    convert->add_option("--filter", args->filter,
                        "Only convert virtual paths containing this substring");
    convert->add_option("--limit", args->limit, "Stop after this many inputs")
        ->default_val(0);
    convert->add_flag("--no-records", args->no_records, "Skip the merge and records.fb");
    convert->add_flag("--no-meshes", args->no_meshes, "Skip the NIF pass");
    convert->add_flag("--no-textures", args->no_textures, "Skip the DDS pass");
    convert->add_flag("--no-scripts", args->no_scripts, "Skip the PEX pass");
    convert->add_flag("--no-lod", args->no_lod, "Skip terrain, object and tree LOD");
    convert->add_flag("--no-animations", args->no_animations, "Skip Havok files (.hkx)");
    convert->add_flag("--no-mip-fix", args->no_mip_fix,
                      "Leave short DDS mip chains alone (control run)");
    convert->add_flag("--no-collision", args->no_collision, "Do not read Havok shapes");
    convert->add_flag("--no-skinning", args->no_skinning, "Do not read skin data");
    convert->add_flag("--keep-z-up", args->keep_z_up, "Leave meshes in NIF space");
    convert->add_option("--unit-scale", args->unit_scale, "Metres per game unit")
        ->default_val(bethconv::mesh::k_default_unit_scale);
    convert->add_flag("--hash-sources", args->hash_archives,
                      "Hash every mounted archive into the manifest, not only the plugins");
    convert->add_flag("--prune", args->prune,
                      "Remove assets this run's index does not name (compacts a blob)");
    convert->add_option("--store", args->store,
                        "blob: one index and one blob file (default); loose: one file per asset")
        ->check(CLI::IsMember({"blob", "loose"}));
    convert->add_flag("--allow-slow-target", args->allow_slow_target, k_allow_slow_help);
    convert->add_flag("-q,--quiet", args->quiet, "No progress line");
    convert->add_option("--mo2", args->mo2,
                        "A Mod Organizer 2 instance folder: mount its profile's mods over --data")
        ->check(CLI::ExistingDirectory)
        ->excludes("--source");
    convert->add_option("--profile", args->profile,
                        "With --mo2: the profile (default: the one MO2 has selected)")
        ->needs("--mo2");
    convert->add_option("--max-texture-size", args->max_texture,
                        "Largest texture side in pixels; larger textures lose their top mip "
                        "levels (0: full size)")
        ->check(CLI::NonNegativeNumber);
    convert->add_option("--encode-uncompressed", args->encode,
                        "Uncompressed textures: keep, bc7 (4x smaller), or compact (BC1, 8x "
                        "smaller, when opaque and not a normal map; else BC7)")
        ->check(CLI::IsMember({"keep", "bc7", "compact"}));
    convert->add_flag("--json", args->json,
                      "Progress and result as JSON lines on stdout; text goes to stderr");
    convert->callback([args] {
        const auto encoding = args->encode == "bc7"       ? bethconv::texture::Encoding::bc7
                              : args->encode == "compact" ? bethconv::texture::Encoding::compact
                                                          : bethconv::texture::Encoding::keep;
        const auto layout = *bethconv::pack::layout_from_string(args->store);
        set_exit_status(cmd_convert(ConvertArgs{.data_dir = args->data,
                                                .list_file = args->list,
                                                .sources = args->sources,
                                                .mo2 = args->mo2,
                                                .mo2_profile = args->profile,
                                                .out = args->out,
                                                .language = args->language,
                                                .filter = args->filter,
                                                .limit = args->limit,
                                                .no_records = args->no_records,
                                                .no_meshes = args->no_meshes,
                                                .no_textures = args->no_textures,
                                                .no_scripts = args->no_scripts,
                                                .no_lod = args->no_lod,
                                                .no_animations = args->no_animations,
                                                .no_mip_fix = args->no_mip_fix,
                                                .no_collision = args->no_collision,
                                                .no_skinning = args->no_skinning,
                                                .keep_z_up = args->keep_z_up,
                                                .unit_scale = args->unit_scale,
                                                .max_texture_size = args->max_texture,
                                                .encoding = encoding,
                                                .hash_archives = args->hash_archives,
                                                .prune = args->prune,
                                                .layout = layout,
                                                .allow_slow_target = args->allow_slow_target,
                                                .quiet = args->quiet,
                                                .json = args->json}));
    });
}

} // namespace bethconv::cli
