// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/pack/inputs.hpp"
#include "bethconv/pack/snapshot.hpp"
#include "bethconv/record/merge.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bethconv::cli {
namespace {

int cmd_snapshot(const std::filesystem::path& data_dir, const std::filesystem::path& list_file,
                 const std::vector<std::filesystem::path>& sources, const std::string& language,
                 const std::filesystem::path& out, bool no_strings) {
    auto order = build_order(data_dir, list_file);
    if (!order) {
        std::fprintf(stderr, "error: %s\n", order.error().to_string().c_str());
        return 1;
    }
    std::printf("%zu plugins in the order, %zu problems\n", order->entries().size(),
                order->problems().size());
    for (const auto& problem : order->problems()) {
        std::printf("  %s\n", problem.to_string().c_str());
    }

    bethconv::archive::ArchiveSet set;
    bethconv::record::MergeOptions options;
    options.language = language;
    if (!no_strings) {
        if (sources.empty()) {
            const auto mounted = mount_data_folder(set, data_dir);
            std::printf("mounted %zu sources from the data folder, %zu unique paths\n", mounted,
                        set.unique_paths());
        } else {
            (void)mount_all(set, sources);
        }
        options.strings = bethconv::pack::string_fetch(set);
    }

    const auto started = std::chrono::steady_clock::now();
    const auto world = bethconv::record::MergedWorld::build(*order, options);
    const auto indexed_at =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("\n%s\nindexed in %.1fs\n", world.report().c_str(), indexed_at);

    const auto write_started = std::chrono::steady_clock::now();
    const auto stats = bethconv::pack::write_snapshot(
        world, *order, out,
        bethconv::pack::SnapshotOptions{.converter = "bethconv", .language = language});
    if (!stats) {
        std::fprintf(stderr, "error: %s\n", stats.error().to_string().c_str());
        return 1;
    }
    const auto wrote_at =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - write_started).count();

    std::printf("\nwrote %s in %.1fs\n", out.string().c_str(), wrote_at);
    std::printf("  %llu forms, %llu payload bytes, %llu index bytes, %llu total\n",
                static_cast<unsigned long long>(stats->forms),
                static_cast<unsigned long long>(stats->payload_bytes),
                static_cast<unsigned long long>(stats->index_bytes),
                static_cast<unsigned long long>(stats->file_bytes));
    std::printf("  %llu editor ids, %llu types, %llu parents with children, "
                "%llu worldspaces / %llu grid cells\n",
                static_cast<unsigned long long>(stats->editor_ids),
                static_cast<unsigned long long>(stats->types),
                static_cast<unsigned long long>(stats->children),
                static_cast<unsigned long long>(stats->worlds),
                static_cast<unsigned long long>(stats->grid_cells));
    if (stats->grid_duplicates != 0) {
        std::printf("  %llu exterior cells share a grid square with another -- the grid "
                    "index can only name one of each\n",
                    static_cast<unsigned long long>(stats->grid_duplicates));
    }
    if (stats->unwalkable != 0) {
        std::printf("  %llu records had no readable field list (each costs its "
                    "editor id, not the pack)\n",
                    static_cast<unsigned long long>(stats->unwalkable));
    }

    // The snapshot must match the merge it came from; checked here too, not only
    // in `verify`.
    if (stats->forms != world.stats().forms) {
        std::fprintf(stderr,
                     "error: wrote %llu forms but the merge claims %llu -- MISMATCH\n",
                     static_cast<unsigned long long>(stats->forms),
                     static_cast<unsigned long long>(world.stats().forms));
        return 1;
    }
    return 0;
}

struct SnapshotArgs {
    std::filesystem::path data;
    std::filesystem::path list;
    std::vector<std::filesystem::path> sources;
    std::string language{std::string(bethconv::record::k_default_language)};
    std::filesystem::path out{"records.fb"};
    bool no_strings = false;
};

} // namespace

void register_snapshot(CLI::App& app) {
    auto args = std::make_shared<SnapshotArgs>();
    auto* snapshot =
        app.add_subcommand("snapshot", "Write the merged world to a records.fb snapshot");
    snapshot->add_option("--data", args->data, "The game's Data folder")
        ->required()
        ->check(CLI::ExistingDirectory);
    snapshot->add_option("--list", args->list, "plugins.txt or loadorder.txt")
        ->check(CLI::ExistingFile);
    snapshot->add_option("--source", args->sources,
                         "Where to read strings/ from; default is every archive in --data")
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    snapshot->add_option("--language", args->language, "Which .STRINGS language to resolve")
        ->default_val(std::string(bethconv::record::k_default_language));
    snapshot->add_option("-o,--out", args->out, "Where to write the snapshot")
        ->default_val("records.fb");
    snapshot->add_flag("--no-strings", args->no_strings, "Leave localized names as indices");
    snapshot->callback([args] {
        set_exit_status(cmd_snapshot(args->data, args->list, args->sources, args->language,
                                     args->out, args->no_strings));
    });
}

} // namespace bethconv::cli
