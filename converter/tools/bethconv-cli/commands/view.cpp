// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/pack/pack_view.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace bethconv::cli {
namespace {

/// Materialize a pack as a directory tree glTF-only consumers can open. See
/// pack/pack_view.hpp.
int cmd_view(const std::filesystem::path& pack, const std::filesystem::path& out,
             const std::string& filter, std::size_t limit, const std::filesystem::path& list_file,
             bool copy, bool all, bool allow_slow_target, bool quiet) {
    if (filter.empty() && limit == 0 && list_file.empty() && !all) {
        std::fprintf(stderr, "error: a view of the whole pack writes one file per asset (about "
                             "80,000 and 20 GiB for vanilla SE). Choose with --filter, --from or "
                             "--limit, or pass --all.\n");
        return 2;
    }
    if (!output_target_ok(out, true, allow_slow_target)) {
        return 2;
    }
    bethconv::pack::ViewOptions options;
    options.out = out;
    options.copy_assets = copy;
    options.filter = filter;
    options.limit = limit;
    if (!list_file.empty()) {
        bool ok = true;
        options.only = read_vpath_list(list_file, ok);
        if (!ok) {
            return 1;
        }
    }

    const auto started = std::chrono::steady_clock::now();
    if (!quiet) {
        options.progress = [started](const std::string& phase, std::uint64_t done,
                                     std::uint64_t total) {
            const auto elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::printf("\r%-8s %llu/%llu  %.0fs", phase.c_str(),
                        static_cast<unsigned long long>(done),
                        static_cast<unsigned long long>(total), elapsed);
            std::fflush(stdout);
        };
    }

    auto result = bethconv::pack::materialize_view(pack, options);
    if (!quiet) {
        std::printf("\r%-40s\r", "");
    }
    if (!result) {
        std::fprintf(stderr, "error: %s\n", result.error().to_string().c_str());
        return 1;
    }
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    const auto& stats = result->stats;
    std::printf("wrote %s\n", out.string().c_str());
    std::printf("  from          %llu index entries, %llu considered\n",
                static_cast<unsigned long long>(stats.index_entries),
                static_cast<unsigned long long>(stats.considered));
    std::printf("  files         %llu meshes, %llu textures, %llu scripts\n",
                static_cast<unsigned long long>(stats.meshes),
                static_cast<unsigned long long>(stats.textures),
                static_cast<unsigned long long>(stats.scripts));
    std::printf("                %llu linked, %llu copied, %llu rewritten "
                "(%llu shared a rewrite), %.1f MiB\n",
                static_cast<unsigned long long>(stats.linked),
                static_cast<unsigned long long>(stats.copied),
                static_cast<unsigned long long>(stats.written),
                static_cast<unsigned long long>(stats.mesh_links),
                static_cast<double>(stats.bytes) / (1024.0 * 1024.0));
    if (stats.pulled_in != 0) {
        std::printf("                %llu texture(s) pulled in past the filter, because a "
                    "mesh names them\n",
                    static_cast<unsigned long long>(stats.pulled_in));
    }
    // Always printed, including "0 dangling".
    std::printf("  textures      %llu image reference(s), %llu resolved, %llu dangling\n",
                static_cast<unsigned long long>(stats.image_refs),
                static_cast<unsigned long long>(stats.image_refs_resolved),
                static_cast<unsigned long long>(stats.image_refs_dangling));
    std::printf("in %.1fs\n", elapsed);

    if (stats.failed != 0) {
        std::printf("\n%llu entr(ies) did not make it into the view:\n",
                    static_cast<unsigned long long>(stats.failed));
        std::size_t shown = 0;
        for (const auto& failure : result->failures) {
            if (shown++ >= 10) {
                break;
            }
            std::printf("  %-8s %s: %s\n", failure.stage.c_str(), failure.vpath.c_str(),
                        failure.detail.c_str());
        }
    }
    return stats.failed == 0 ? 0 : 1;
}

struct ViewArgs {
    std::filesystem::path pack;
    std::filesystem::path out;
    std::string filter;
    std::size_t limit = 0;
    std::filesystem::path list;
    bool copy = false;
    bool all = false;
    bool allow_slow_target = false;
    bool quiet = false;
};

} // namespace

void register_view(CLI::App& app) {
    auto args = std::make_shared<ViewArgs>();
    auto* view = app.add_subcommand(
        "view", "Resolve a pack's vpath.idx into a directory a glTF consumer can open");
    view->add_option("pack", args->pack, "The pack directory to resolve")
        ->required()
        ->check(CLI::ExistingDirectory);
    view->add_option("-o,--out", args->out, "Directory to materialize the view in")
        ->default_val("view");
    view->add_option("--filter", args->filter,
                     "Only materialize virtual paths containing this substring");
    view->add_option("--limit", args->limit, "Stop after this many index entries")
        ->default_val(0);
    view->add_option("--from", args->list,
                     "Only materialize the virtual paths in this file (one per line), plus "
                     "the textures their meshes use")
        ->check(CLI::ExistingFile);
    view->add_flag("--copy", args->copy,
                   "Copy asset bytes instead of hard-linking them into a loose pack");
    view->add_flag("--all", args->all, "Materialize the whole pack");
    view->add_flag("--allow-slow-target", args->allow_slow_target, k_allow_slow_help);
    view->add_flag("-q,--quiet", args->quiet, "No progress line");
    view->callback([args] {
        set_exit_status(cmd_view(args->pack, args->out, args->filter, args->limit, args->list,
                                 args->copy, args->all, args->allow_slow_target, args->quiet));
    });
}

} // namespace bethconv::cli
