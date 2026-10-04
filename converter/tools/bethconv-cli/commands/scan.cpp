// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bethconv::cli {
namespace {

int cmd_scan(const std::vector<std::filesystem::path>& paths, bool list_conflicts,
             std::size_t max_listed, const std::string& list_filter,
             std::size_t list_count, bool read_all) {
    bethconv::archive::ArchiveSet set;
    const int failures = mount_all(set, paths);

    std::printf("%-62s %-9s %10s %9s\n", "SOURCE", "KIND", "FILES", "VERSION");
    std::printf("%-62s %-9s %10s %9s\n", "------", "----", "-----", "-------");
    std::size_t total = 0;
    for (const auto& source : set.sources()) {
        std::printf("%-62s %-9s %10zu %9u\n", source.name.c_str(),
                    std::string(to_string(source.kind)).c_str(), source.file_count,
                    source.version);
        total += source.file_count;
    }

    if (list_count > 0) {
        std::printf("\npaths%s%s:\n", list_filter.empty() ? "" : " matching ",
                    list_filter.c_str());
        std::size_t shown = 0;
        set.for_each([&](const bethconv::archive::Resolution& resolution) {
            if (shown >= list_count) {
                return;
            }
            if (!list_filter.empty() &&
                resolution.vpath.find(list_filter) == std::string::npos) {
                return;
            }
            std::printf("  %-70s %s\n", resolution.vpath.c_str(),
                        set.sources()[resolution.winner].name.c_str());
            ++shown;
        });
    }

    if (read_all) {
        // Read and decompress every winning entry: an integrity check and, with
        // sanitizers, a stress test on real archives.
        std::size_t ok = 0;
        std::size_t failed = 0;
        std::uint64_t bytes = 0;
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> failure_samples;
        set.for_each([&](const bethconv::archive::Resolution& resolution) {
            auto data = set.read(resolution.vpath);
            if (data) {
                ++ok;
                bytes += data->size();
            } else {
                ++failed;
                if (failure_samples.size() < 20) {
                    failure_samples.push_back(data.error().to_string());
                }
            }
        });
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::printf("\nread-all: %zu ok, %zu failed, %.1f MiB in %.1fs (%.1f MiB/s)\n",
                    ok, failed, static_cast<double>(bytes) / (1024.0 * 1024.0), elapsed,
                    static_cast<double>(bytes) / (1024.0 * 1024.0) / elapsed);
        for (const auto& failure : failure_samples) {
            std::printf("  FAIL %s\n", failure.c_str());
        }
    }

    const auto conflicts = set.conflicts();
    std::printf("\n%zu entries across %zu sources -> %zu unique paths, %zu shadowed\n",
                total, set.sources().size(), set.unique_paths(), conflicts.size());

    if (list_conflicts && !conflicts.empty()) {
        std::printf("\nconflicts (winner first):\n");
        std::size_t shown = 0;
        for (const auto& conflict : conflicts) {
            if (shown++ >= max_listed) {
                std::printf("  ... and %zu more\n", conflicts.size() - max_listed);
                break;
            }
            std::printf("  %s\n      <- %s\n", conflict.vpath.c_str(),
                        set.sources()[conflict.winner].name.c_str());
            for (const auto shadowed : conflict.shadowed) {
                std::printf("      xx %s\n", set.sources()[shadowed].name.c_str());
            }
        }
    }
    return failures == 0 ? 0 : 1;
}

struct ScanArgs {
    std::vector<std::filesystem::path> paths;
    bool list_conflicts = false;
    std::size_t max_listed = 20;
    std::string list_filter;
    std::size_t list_count = 0;
    bool read_all = false;
};

} // namespace

void register_scan(CLI::App& app) {
    auto args = std::make_shared<ScanArgs>();
    auto* scan = app.add_subcommand("scan", "Mount archives/directories and report contents");
    scan->add_option("sources", args->paths,
                     "BSA/BA2 files and loose directories, in load order")
        ->required()
        ->check(CLI::ExistingPath);
    scan->add_flag("--conflicts", args->list_conflicts,
                   "List paths provided by more than one source");
    scan->add_option("--max-listed", args->max_listed, "Cap on listed conflicts")->default_val(20);
    scan->add_option("--list", args->list_count, "List this many virtual paths")->default_val(0);
    scan->add_option("--filter", args->list_filter, "Only list paths containing this substring");
    scan->add_flag("--read-all", args->read_all,
                   "Read and decompress every entry; reports failures");
    scan->callback([args] {
        set_exit_status(cmd_scan(args->paths, args->list_conflicts, args->max_listed,
                                 args->list_filter, args->list_count, args->read_all));
    });
}

} // namespace bethconv::cli
