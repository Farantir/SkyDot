// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/histogram.hpp"
#include "bethconv/record/plugin.hpp"

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

int cmd_records(const std::vector<std::filesystem::path>& paths, bool stats,
                std::size_t top_fields, const std::vector<std::string>& size_types) {
    bethconv::record::Histogram histogram;
    for (const auto& type : size_types) {
        if (type.size() != 4) {
            std::fprintf(stderr, "error: --field-sizes takes 4-character record types, "
                                 "got \"%s\"\n", type.c_str());
            return 2;
        }
        histogram.track_field_sizes(bethconv::io::FourCC{
            static_cast<std::uint32_t>(static_cast<unsigned char>(type[0])) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(type[1])) << 8 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(type[2])) << 16 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(type[3])) << 24});
    }
    bethconv::record::ScanStats totals;
    const auto started = std::chrono::steady_clock::now();
    int failures = 0;

    for (const auto& path : paths) {
        auto plugin = bethconv::record::Plugin::open(path);
        if (!plugin) {
            std::fprintf(stderr, "error: %s: %s\n", path.filename().string().c_str(),
                         plugin.error().to_string().c_str());
            ++failures;
            continue;
        }

        const auto& header = plugin->header();
        std::printf("%-24s  v%.2f  %-6s %-6s %-11s  masters=%zu  records=%d\n",
                    plugin->name().c_str(), static_cast<double>(header.version),
                    header.is_master() ? "ESM" : "ESP",
                    header.is_light() ? "ESL" : "",
                    header.is_localized() ? "localized" : "",
                    header.masters.size(), header.record_count);
        for (const auto& master : header.masters) {
            std::printf("      master: %s\n", master.name.c_str());
        }

        const auto scanned = plugin->scan(histogram);
        totals.records += scanned.records;
        totals.groups += scanned.groups;
        totals.compressed_records += scanned.compressed_records;
        totals.deleted_records += scanned.deleted_records;
        totals.bytes_scanned += scanned.bytes_scanned;
        totals.bytes_inflated += scanned.bytes_inflated;
        totals.errors += scanned.errors;
    }

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    std::printf("\nscanned %llu records in %llu groups (%llu compressed, %llu deleted) "
                "in %.2fs\n",
                static_cast<unsigned long long>(totals.records),
                static_cast<unsigned long long>(totals.groups),
                static_cast<unsigned long long>(totals.compressed_records),
                static_cast<unsigned long long>(totals.deleted_records), elapsed);
    std::printf("payload %.1f MiB (+%.1f MiB inflated), %llu structural errors\n",
                static_cast<double>(totals.bytes_scanned) / (1024.0 * 1024.0),
                static_cast<double>(totals.bytes_inflated) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(totals.errors));

    if (stats) {
        std::printf("\n%s", histogram.report(top_fields).c_str());
    }
    if (!size_types.empty()) {
        std::printf("\nfield sizes:\n%s", histogram.size_report().c_str());
    }
    return failures == 0 ? 0 : 1;
}

struct RecordsArgs {
    std::vector<std::filesystem::path> plugins;
    bool stats = false;
    std::size_t top_fields = 0;
    std::vector<std::string> size_types;
};

} // namespace

void register_records(CLI::App& app) {
    auto args = std::make_shared<RecordsArgs>();
    auto* records = app.add_subcommand("records", "Parse plugins and report their contents");
    records->add_option("plugins", args->plugins, "ESM/ESP/ESL files, in load order")
        ->required()
        ->check(CLI::ExistingFile);
    records->add_flag("--stats", args->stats, "Print a per-record-type histogram");
    records->add_option("--top-fields", args->top_fields,
                        "List this many most-common field types per record type")
        ->default_val(0);
    records->add_option("--field-sizes", args->size_types,
                        "Census the on-disk size of every field of these record types");
    records->callback([args] {
        set_exit_status(cmd_records(args->plugins, args->stats, args->top_fields,
                                    args->size_types));
    });
}

} // namespace bethconv::cli
