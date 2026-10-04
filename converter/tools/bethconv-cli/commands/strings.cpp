// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/pack/inputs.hpp"
#include "bethconv/record/strings.hpp"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bethconv::cli {
namespace {

int cmd_strings(const std::vector<std::filesystem::path>& sources,
                const std::vector<std::string>& plugins, const std::string& language,
                std::size_t list_count, const std::vector<std::string>& lookups) {
    bethconv::archive::ArchiveSet set;
    (void)mount_all(set, sources);

    std::size_t missing = 0;
    std::size_t repaired = 0;
    for (const auto& name : plugins) {
        std::vector<bethconv::io::ParseError> problems;
        const auto source = bethconv::record::load_string_source(
            bethconv::pack::string_fetch(set), name, language, &problems);
        for (const auto& problem : problems) {
            std::fprintf(stderr, "error: %s\n", problem.to_string().c_str());
        }

        std::printf("\n%s (%s)\n", name.c_str(), language.c_str());
        if (source.empty() && problems.empty()) {
            std::printf("  no tables in this set; looked for %s\n",
                        bethconv::record::string_table_path(name, language,
                                                            bethconv::record::StringKind::plain)
                            .c_str());
            ++missing;
            continue;
        }

        std::printf("  %-10s %10s %10s %12s %10s\n", "TABLE", "ENTRIES", "DISTINCT", "TEXT BYTES",
                    "REPAIRED");
        for (const auto kind : bethconv::record::k_string_kinds) {
            const auto& table = source.table(kind);
            if (table.empty()) {
                continue;
            }
            const auto& stats = table.stats();
            std::printf("  %-10s %10llu %10llu %12llu %10llu\n",
                        std::string(bethconv::record::to_string(kind)).c_str(),
                        static_cast<unsigned long long>(stats.entries),
                        static_cast<unsigned long long>(stats.distinct_strings),
                        static_cast<unsigned long long>(stats.text_bytes),
                        static_cast<unsigned long long>(stats.repaired));
            repaired += stats.repaired;
        }

        for (const auto& lookup : lookups) {
            // Accept hex (0x1A2B) and decimal; ids appear both ways.
            const auto id = static_cast<std::uint32_t>(std::stoul(lookup, nullptr, 0));
            bool found = false;
            for (const auto kind : bethconv::record::k_string_kinds) {
                if (const auto* text = source.find(id, kind)) {
                    std::printf("  #%08X [%s] %s\n", id,
                                std::string(bethconv::record::to_string(kind)).c_str(),
                                text->c_str());
                    found = true;
                }
            }
            if (!found) {
                std::printf("  #%08X not in any table\n", id);
            }
        }

        if (list_count != 0) {
            // Ids are sparse and unsorted, so probe a range instead of "first N".
            std::printf("  first %zu ids that resolve:\n", list_count);
            std::size_t shown = 0;
            for (std::uint32_t id = 1; id != 0 && shown < list_count; ++id) {
                for (const auto kind : bethconv::record::k_string_kinds) {
                    const auto* text = source.find(id, kind);
                    if (text == nullptr) {
                        continue;
                    }
                    std::string preview = *text;
                    if (preview.size() > 70) {
                        preview.resize(70);
                        preview += "...";
                    }
                    std::printf("    #%08X [%-9s] %s\n", id,
                                std::string(bethconv::record::to_string(kind)).c_str(),
                                preview.c_str());
                    ++shown;
                    break;
                }
            }
        }
    }

    if (repaired != 0) {
        std::printf("\n%zu entries were not valid UTF-8 and were repaired\n", repaired);
    }
    return missing == plugins.size() ? 1 : 0;
}

struct StringsArgs {
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> plugins;
    std::string language{std::string(bethconv::record::k_default_language)};
    std::size_t list = 0;
    std::vector<std::string> lookups;
};

} // namespace

void register_strings(CLI::App& app) {
    auto args = std::make_shared<StringsArgs>();
    auto* strings = app.add_subcommand("strings", "Read a plugin's .STRINGS tables");
    strings->add_option("--source", args->sources,
                        "BSA/BA2 or loose Data dir; repeat, in load order")
        ->required()
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    strings->add_option("plugin", args->plugins, "Plugin filenames, e.g. Skyrim.esm")
        ->required();
    strings->add_option("--language", args->language, "Language to load")
        ->default_val(std::string(bethconv::record::k_default_language));
    strings->add_option("--list", args->list, "Show this many resolving ids")->default_val(0);
    strings->add_option("--id", args->lookups, "Look up these string indices");
    strings->callback([args] {
        set_exit_status(cmd_strings(args->sources, args->plugins, args->language, args->list,
                                    args->lookups));
    });
}

} // namespace bethconv::cli
