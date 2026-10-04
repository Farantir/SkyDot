// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/plugin.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bethconv::cli {
namespace {

/// Resolve every record FormID in a plugin against the load order: the high
/// byte must index that plugin's own master list.
class FormIdCheck final : public bethconv::record::RecordSink {
public:
    FormIdCheck(const bethconv::record::LoadOrder& order, std::size_t plugin)
        : order_(order), plugin_(plugin) {}

    void on_record(const bethconv::record::RecordContext& ctx, bethconv::io::SpanReader&) override {
        ++records;
        const auto resolved = order_.resolve(plugin_, ctx.header.form_id);
        if (!resolved) {
            ++unresolved;
            if (examples.size() < 4) {
                examples.push_back(resolved.error().to_string());
            }
            return;
        }
        const auto owner = order_.owner_of(plugin_, ctx.header.form_id);
        if (owner && *owner == plugin_) {
            ++own;
        } else {
            ++overrides;
        }
    }

    std::size_t records = 0;
    std::size_t own = 0;
    std::size_t overrides = 0;
    std::size_t unresolved = 0;
    std::vector<std::string> examples;

private:
    const bethconv::record::LoadOrder& order_;
    std::size_t plugin_;
};

/// Resolve a load order. The list order is kept; masters are not moved first
/// (see docs/format-notes/load-order.md).
int cmd_loadorder(const std::filesystem::path& data_dir, const std::filesystem::path& list_file,
                  bool include_inactive, bool no_implicit, bool verify, bool verbose) {
    bethconv::record::LoadOrderOptions options;
    options.active_only = !include_inactive;
    options.add_implicit_masters = !no_implicit;

    std::optional<bethconv::record::LoadOrder> order;
    if (list_file.empty()) {
        auto built = bethconv::record::LoadOrder::from_directory(data_dir, options);
        if (!built) {
            std::fprintf(stderr, "error: %s\n", built.error().to_string().c_str());
            return 1;
        }
        std::printf("no list given: ordering %s by last-modified time, which is what the "
                    "game does with no plugins.txt and is only as good as the timestamps\n",
                    data_dir.string().c_str());
        order = std::move(*built);
    } else {
        auto list = bethconv::record::read_plugin_list(list_file);
        if (!list) {
            std::fprintf(stderr, "error: %s\n", list.error().to_string().c_str());
            return 1;
        }
        std::printf("%s: %zu entries, %s\n", list_file.filename().string().c_str(),
                    list->plugins.size(),
                    list->marks_active ? "with activation marks" : "no activation marks");
        order = bethconv::record::LoadOrder::build(data_dir, *list, options);
    }

    std::printf("\n%-8s %-5s %-7s %-7s %s\n", "INDEX", "FLAGS", "VERSION", "MASTERS", "PLUGIN");
    std::printf("%-8s %-5s %-7s %-7s %s\n", "-----", "-----", "-------", "-------", "------");
    for (const auto& entry : order->entries()) {
        char index[16]{};
        if (entry.is_light) {
            std::snprintf(index, sizeof(index), "FE:%03X", entry.index);
        } else {
            std::snprintf(index, sizeof(index), "%02X", entry.index);
        }
        const std::string flags = std::string(entry.is_master ? "M" : "-") +
                                  (entry.is_light ? "L" : "-") +
                                  (entry.is_localized ? "S" : "-") +
                                  (entry.active ? "" : " off");
        std::printf("%-8s %-5s %-7.2f %-7zu %s\n", index, flags.c_str(),
                    static_cast<double>(entry.header_version), entry.masters.size(),
                    entry.name.c_str());
    }

    std::printf("\n%zu plugins: %zu normal (of %u), %zu light (of %u)\n",
                order->entries().size(), order->normal_count(),
                bethconv::record::k_max_normal_index + 1, order->light_count(),
                bethconv::record::k_max_light_index + 1);

    if (!order->problems().empty()) {
        std::printf("\n%zu problems:\n", order->problems().size());
        for (const auto& problem : order->problems()) {
            std::printf("  %s\n", problem.to_string().c_str());
        }
    }

    if (!verify) {
        return order->problems().empty() ? 0 : 1;
    }

    // Remap every record every plugin defines or overrides.
    std::printf("\nverifying every record FormID against the order:\n");
    std::size_t total = 0;
    std::size_t own = 0;
    std::size_t overrides = 0;
    std::size_t unresolved = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < order->entries().size(); ++i) {
        const auto& entry = order->entries()[i];
        auto plugin = bethconv::record::Plugin::open(entry.path);
        if (!plugin) {
            std::fprintf(stderr, "error: %s: %s\n", entry.name.c_str(),
                         plugin.error().to_string().c_str());
            continue;
        }
        FormIdCheck check(*order, i);
        const auto stats = plugin->scan(check);
        total += check.records;
        own += check.own;
        overrides += check.overrides;
        unresolved += check.unresolved;
        if (verbose || check.unresolved != 0) {
            std::printf("  %-52s %7zu records, %6zu own, %6zu overrides, %zu unresolved\n",
                        entry.name.c_str(), check.records, check.own, check.overrides,
                        check.unresolved);
            for (const auto& example : check.examples) {
                std::printf("      %s\n", example.c_str());
            }
        }
        if (stats.errors != 0) {
            std::fprintf(stderr, "warning: %s: %llu structural errors\n", entry.name.c_str(),
                         static_cast<unsigned long long>(stats.errors));
        }
    }
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("\n%zu records: %zu own, %zu overrides, %zu unresolved, in %.1fs\n", total, own,
                overrides, unresolved, elapsed);
    return unresolved == 0 && order->problems().empty() ? 0 : 1;
}

struct LoadOrderArgs {
    std::filesystem::path data;
    std::filesystem::path list;
    bool include_inactive = false;
    bool no_implicit = false;
    bool verify = false;
    bool verbose = false;
};

} // namespace

void register_loadorder(CLI::App& app) {
    auto args = std::make_shared<LoadOrderArgs>();
    auto* loadorder =
        app.add_subcommand("loadorder", "Resolve a plugin list into an indexed load order");
    loadorder->add_option("--data", args->data, "The game's Data folder")
        ->required()
        ->check(CLI::ExistingDirectory);
    loadorder->add_option("--list", args->list, "plugins.txt or loadorder.txt")
        ->check(CLI::ExistingFile);
    loadorder->add_flag("--include-inactive", args->include_inactive,
                        "Keep plugins a plugins.txt leaves unmarked");
    loadorder->add_flag("--no-implicit", args->no_implicit,
                        "Do not prepend Skyrim.esm and the DLC when the list omits them");
    loadorder->add_flag("--verify", args->verify,
                        "Walk every plugin and remap every record FormID");
    loadorder->add_flag("-v,--verbose", args->verbose, "One line per plugin while verifying");
    loadorder->callback([args] {
        set_exit_status(cmd_loadorder(args->data, args->list, args->include_inactive,
                                      args->no_implicit, args->verify, args->verbose));
    });
}

} // namespace bethconv::cli
