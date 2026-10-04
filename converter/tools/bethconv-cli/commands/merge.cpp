// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/pack/inputs.hpp"
#include "bethconv/record/form_census.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bethconv::cli {
namespace {

/// Number of paths under `prefix` with more than one provider.
std::size_t shadowed_under(const bethconv::archive::ArchiveSet& set, std::string_view prefix) {
    std::size_t shadowed = 0;
    for (const auto& conflict : set.conflicts()) {
        if (conflict.vpath.starts_with(prefix)) {
            ++shadowed;
        }
    }
    return shadowed;
}

/// Pass two: extract identifying fields from winning records. Only the types
/// handled here produce a name; the rest are counted.
class MergeSampler final : public bethconv::record::MergedRecordSink {
public:
    struct Named {
        std::string type;
        std::string form;
        std::string editor_id;
        std::string name;
        std::string parent;
    };

    MergeSampler(std::size_t per_type, bethconv::record::FormCensus* census)
        : per_type_(per_type), census_(census) {}

    void on_record(const bethconv::record::MergedRecord& merged,
                   const bethconv::record::RecordContext& ctx, bethconv::io::SpanReader& data,
                   const bethconv::record::FormContext& form_ctx) override {
        ++visited_;
        if (merged.deleted) {
            return;
        }

        const auto type = merged.type;
        std::string editor_id;
        std::string name;
        bool described = false;

        using namespace bethconv::record;
        if (type == FourCC{"CELL"}) {
            if (auto cell = parse_cell(data, form_ctx)) {
                editor_id = cell->editor_id;
                name = cell->name.to_string();
                named_ += cell->name.from_table() ? 1U : 0U;
                described = true;
            }
        } else if (type == FourCC{"WRLD"}) {
            if (auto world = parse_worldspace(data, form_ctx)) {
                editor_id = world->editor_id;
                name = world->name.to_string();
                named_ += world->name.from_table() ? 1U : 0U;
                described = true;
            }
        } else if (type == FourCC{"DOOR"}) {
            if (auto door = parse_door(data, form_ctx)) {
                editor_id = door->editor_id;
                name = door->name.to_string();
                named_ += door->name.from_table() ? 1U : 0U;
                described = true;
            }
        } else if (type == FourCC{"LIGH"}) {
            if (auto light = parse_light(data, form_ctx)) {
                editor_id = light->editor_id;
                name = light->name.to_string();
                named_ += light->name.from_table() ? 1U : 0U;
                described = true;
            }
        } else if (type == FourCC{"STAT"}) {
            if (auto stat = parse_static(data, form_ctx)) {
                editor_id = stat->editor_id;
                name = stat->model.path;
                described = true;
            }
        } else if (type == FourCC{"REFR"}) {
            if (auto refr = parse_reference(ctx.header, data, form_ctx)) {
                editor_id = refr->editor_id;
                char buffer[64]{};
                std::snprintf(buffer, sizeof(buffer), "-> %s at %.0f %.0f %.0f",
                              refr->base.to_string().c_str(),
                              static_cast<double>(refr->position.x),
                              static_cast<double>(refr->position.y),
                              static_cast<double>(refr->position.z));
                name = buffer;
                described = true;
            }
        }

        if (described) {
            ++described_;
        }
        if (per_type_ == 0 || samples_[type.value].size() >= per_type_) {
            return;
        }
        samples_[type.value].push_back(
            Named{.type = type.to_string(),
                  .form = merged.form.to_string(),
                  .editor_id = std::move(editor_id),
                  .name = std::move(name),
                  .parent = merged.parent.is_null() ? std::string("-")
                                                    : merged.parent.to_string()});
    }

    [[nodiscard]] bethconv::record::FieldTally* tally() override { return census_; }

    [[nodiscard]] std::size_t visited() const noexcept { return visited_; }
    [[nodiscard]] std::size_t described() const noexcept { return described_; }
    [[nodiscard]] std::size_t named() const noexcept { return named_; }
    [[nodiscard]] const std::map<std::uint32_t, std::vector<Named>>& samples() const noexcept {
        return samples_;
    }

private:
    std::size_t per_type_;
    bethconv::record::FormCensus* census_;
    std::size_t visited_ = 0;
    std::size_t described_ = 0;
    std::size_t named_ = 0;
    std::map<std::uint32_t, std::vector<Named>> samples_;
};

int cmd_merge(const std::filesystem::path& data_dir, const std::filesystem::path& list_file,
              const std::vector<std::filesystem::path>& sources, const std::string& language,
              const std::vector<std::string>& type_names, std::size_t sample,
              const std::vector<std::string>& lookups, bool no_strings, bool no_second_pass) {
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
    if (!no_strings) {
        if (sources.empty()) {
            const auto mounted = mount_data_folder(set, data_dir);
            std::printf("mounted %zu sources from the data folder, %zu unique paths\n", mounted,
                        set.unique_paths());
        } else {
            (void)mount_all(set, sources);
        }
        std::printf("strings/ paths provided by more than one source: %zu\n",
                    shadowed_under(set, "strings/"));
    }

    bethconv::record::MergeOptions options;
    options.language = language;
    if (!no_strings) {
        options.strings = bethconv::pack::string_fetch(set);
    }
    for (const auto& name : type_names) {
        if (name.size() != 4) {
            std::fprintf(stderr, "error: '%s' is not a 4-character record type\n", name.c_str());
            return 2;
        }
        options.types.push_back(bethconv::io::FourCC{
            static_cast<std::uint32_t>(static_cast<unsigned char>(name[0])) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(name[1])) << 8 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(name[2])) << 16 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(name[3])) << 24});
    }

    const auto started = std::chrono::steady_clock::now();
    const auto world = bethconv::record::MergedWorld::build(*order, options);
    const auto merged_at =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    std::printf("\n%s\nmerged in %.1fs\n", world.report().c_str(), merged_at);

    for (const auto& lookup : lookups) {
        const auto value = static_cast<std::uint32_t>(std::stoul(lookup, nullptr, 0));
        const auto* found = world.find(bethconv::record::FormId{value});
        if (found == nullptr) {
            std::printf("\n%s: not in the merged world\n",
                        bethconv::record::FormId{value}.to_string().c_str());
            continue;
        }
        std::printf("\n%s %s\n", found->type.to_string().c_str(),
                    found->form.to_string().c_str());
        std::printf("  won by     %s\n", order->entries()[found->winner].name.c_str());
        std::printf("  owned by   %s%s\n", order->entries()[found->owner].name.c_str(),
                    found->injected ? " (injected: the owner never wrote it)" : "");
        std::printf("  overrides  %u\n", found->overrides);
        std::printf("  parent     %s\n", found->parent.is_null()
                                             ? "-"
                                             : found->parent.to_string().c_str());
        std::printf("  flags      0x%08X%s\n", found->flags, found->deleted ? " DELETED" : "");
    }

    // Few enough to list; the plugins involved are the useful part.
    std::size_t shown_injected = 0;
    for (const auto& record : world.records()) {
        if (!record.injected || shown_injected >= 12) {
            continue;
        }
        if (shown_injected == 0) {
            std::printf("\ninjected records (no plugin that wrote them owns their space):\n");
        }
        std::printf("  %s %s  owner %s, written by %s\n", record.type.to_string().c_str(),
                    record.form.to_string().c_str(), order->entries()[record.owner].name.c_str(),
                    order->entries()[record.winner].name.c_str());
        ++shown_injected;
    }

    if (no_second_pass) {
        return 0;
    }

    // The second pass must return exactly as many records as there are forms
    // (minus deletions); otherwise index and walk disagree about winners.
    bethconv::record::FormCensus census;
    MergeSampler sampler(sample, &census);
    const auto pass_started = std::chrono::steady_clock::now();
    world.for_each_record(sampler);
    const auto pass_at =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - pass_started).count();

    const bool reconciled = sampler.visited() == world.stats().forms;
    std::printf("\nsecond pass: %zu winning records visited, index claims %llu forms -- %s\n",
                sampler.visited(),
                static_cast<unsigned long long>(world.stats().forms),
                reconciled ? "reconciled" : "MISMATCH");
    std::printf("%zu described by a field definition, %zu names came from a string table, "
                "in %.1fs\n",
                sampler.described(), sampler.named(), pass_at);

    for (const auto& [type, rows] : sampler.samples()) {
        std::printf("\n%s\n", bethconv::io::FourCC{type}.to_string().c_str());
        for (const auto& row : rows) {
            std::printf("  %-10s parent %-10s %-34s %s\n", row.form.c_str(), row.parent.c_str(),
                        row.editor_id.c_str(), row.name.c_str());
        }
    }

    // Not census.report(): the census only acts as a FieldTally here, so its
    // seen/parsed columns are zero. Print the unhandled fields only.
    std::printf("\nfields with no definition, across the winning records:\n");
    for (const auto& [type, stats] : census.types()) {
        if (stats.unhandled.empty() && stats.leftover.empty()) {
            continue;
        }
        std::printf("  %s\n", bethconv::io::FourCC{type}.to_string().c_str());
        std::vector<std::pair<std::uint32_t, std::uint64_t>> rows(stats.unhandled.begin(),
                                                                  stats.unhandled.end());
        std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.second > b.second; });
        const auto shown = std::min<std::size_t>(rows.size(), 12);
        for (std::size_t i = 0; i < shown; ++i) {
            std::printf("    %s %llu\n", bethconv::io::FourCC{rows[i].first}.to_string().c_str(),
                        static_cast<unsigned long long>(rows[i].second));
        }
        if (rows.size() > shown) {
            std::printf("    ... and %zu more field types\n", rows.size() - shown);
        }
        for (const auto& [field, count] : stats.leftover) {
            std::printf("    LEFTOVER %s %llu\n", bethconv::io::FourCC{field}.to_string().c_str(),
                        static_cast<unsigned long long>(count));
        }
    }

    return reconciled ? 0 : 1;
}

struct MergeArgs {
    std::filesystem::path data;
    std::filesystem::path list;
    std::vector<std::filesystem::path> sources;
    std::string language{std::string(bethconv::record::k_default_language)};
    std::vector<std::string> types;
    std::size_t sample = 0;
    std::vector<std::string> lookups;
    bool no_strings = false;
    bool no_second_pass = false;
};

} // namespace

void register_merge(CLI::App& app) {
    auto args = std::make_shared<MergeArgs>();
    auto* merge = app.add_subcommand("merge", "Collapse a load order into one flat world");
    merge->add_option("--data", args->data, "The game's Data folder")
        ->required()
        ->check(CLI::ExistingDirectory);
    merge->add_option("--list", args->list, "plugins.txt or loadorder.txt")
        ->check(CLI::ExistingFile);
    merge->add_option("--source", args->sources,
                      "Where to read strings/ from; default is every archive in --data")
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    merge->add_option("--language", args->language, "Which .STRINGS language to resolve")
        ->default_val(std::string(bethconv::record::k_default_language));
    merge->add_option("--types", args->types, "Only merge these record types, e.g. CELL REFR");
    merge->add_option("--sample", args->sample, "Show this many winning records per type")
        ->default_val(0);
    merge->add_option("--find", args->lookups, "Print the provenance of these global FormIDs");
    merge->add_flag("--no-strings", args->no_strings, "Leave localized names as indices");
    merge->add_flag("--no-second-pass", args->no_second_pass,
                    "Index only; skip re-reading the winning records");
    merge->callback([args] {
        set_exit_status(cmd_merge(args->data, args->list, args->sources, args->language,
                                  args->types, args->sample, args->lookups, args->no_strings,
                                  args->no_second_pass));
    });
}

} // namespace bethconv::cli
