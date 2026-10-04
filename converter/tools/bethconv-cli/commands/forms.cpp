// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/pack/inputs.hpp"
#include "bethconv/record/form_census.hpp"
#include "bethconv/record/plugin.hpp"
#include "bethconv/record/strings.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bethconv::cli {
namespace {

int cmd_forms(const std::vector<std::filesystem::path>& paths,
              const std::vector<std::filesystem::path>& sources, const std::string& language) {
    bethconv::archive::ArchiveSet set;
    const bool have_sources = !sources.empty();
    if (have_sources) {
        (void)mount_all(set, sources);
    }

    bethconv::record::FormCensus census;
    const auto started = std::chrono::steady_clock::now();
    int failures = 0;
    std::size_t with_strings = 0;
    std::size_t table_entries = 0;
    std::size_t repaired = 0;

    for (const auto& path : paths) {
        auto plugin = bethconv::record::Plugin::open(path);
        if (!plugin) {
            std::fprintf(stderr, "error: %s: %s\n", path.filename().string().c_str(),
                         plugin.error().to_string().c_str());
            ++failures;
            continue;
        }
        // Localization and string tables are per plugin, so set them for each.
        census.set_localized(plugin->header().is_localized());

        bethconv::record::StringSource strings;
        if (have_sources && plugin->header().is_localized()) {
            std::vector<bethconv::io::ParseError> problems;
            strings = bethconv::record::load_string_source(
                bethconv::pack::string_fetch(set), plugin->name(), language, &problems);
            for (const auto& problem : problems) {
                std::fprintf(stderr, "warning: %s\n", problem.to_string().c_str());
            }
            if (!strings.empty()) {
                ++with_strings;
                table_entries += strings.size();
                repaired += strings.repaired();
            }
        }
        census.set_strings(strings.empty() ? nullptr : &strings);
        (void)plugin->scan(census);
        // Detach the tables before `strings` goes out of scope.
        census.set_strings(nullptr);
    }

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    if (have_sources) {
        std::printf("%zu plugin(s) had %s tables: %zu entries, %zu repaired\n\n", with_strings,
                    language.c_str(), table_entries, repaired);
    }
    std::printf("%s\nin %.2fs\n", census.report().c_str(), elapsed);
    return failures == 0 ? 0 : 1;
}

struct FormsArgs {
    std::vector<std::filesystem::path> plugins;
    std::vector<std::filesystem::path> sources;
    std::string language{std::string(bethconv::record::k_default_language)};
};

} // namespace

void register_forms(CLI::App& app) {
    auto args = std::make_shared<FormsArgs>();
    auto* forms = app.add_subcommand(
        "forms", "Parse the record types that have field definitions and report coverage");
    forms->add_option("plugins", args->plugins, "ESM/ESP/ESL files")
        ->required()
        ->check(CLI::ExistingFile);
    forms->add_option("--source", args->sources,
                      "BSA/BA2 or loose Data dir holding strings/; repeat, in load order")
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    forms->add_option("--language", args->language, "Which .STRINGS language to resolve against")
        ->default_val(std::string(bethconv::record::k_default_language));
    forms->callback([args] {
        set_exit_status(cmd_forms(args->plugins, args->sources, args->language));
    });
}

} // namespace bethconv::cli
