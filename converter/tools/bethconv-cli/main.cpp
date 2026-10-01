// SPDX-License-Identifier: GPL-3.0-or-later
//
// bethconv CLI:
//   probe     map a file and report what it looks like
//   records   parse plugins, print a type/field histogram
//   forms     run the field definitions over plugins
//   scan      list archives/plugins and their conflicts
//   loadorder resolve plugins.txt into an indexed load order
//   strings   read a plugin's .STRINGS tables
//   merge     collapse a load order into one flat world
//   snapshot  write the flat world to records.fb
//   extract   pull virtual paths out of the archive set
//   mesh      convert NIFs to glTF, or report what is in them
//   texture   pass DDS through, completing the mip chain
//   verify    re-read records.fb and check what it claims
//   convert   produce a pack
//   view      materialize a pack as a directory tree
//   cell      inspect cells in a pack's world.fb
//   detect    find installs (front_end.cpp, as are the next three)
//   mo2       read a Mod Organizer 2 instance
//   target    check a folder for a pack
//   info      summarize a pack
#include "front_end.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/archive/vpath.hpp"
#include "bethconv/install/mo2.hpp"
#include "bethconv/install/mount_plan.hpp"
#include "bethconv/io/json_text.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/output_target.hpp"
#include "bethconv/io/span_stream.hpp"
#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/mesh/nif_reader.hpp"
#include "bethconv/pack/convert.hpp"
#include "bethconv/pack/pack_view.hpp"
#include "bethconv/pack/snapshot.hpp"
#include "bethconv/pack/vpath_index.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/form_census.hpp"
#include "bethconv/record/histogram.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"
#include "bethconv/record/plugin.hpp"
#include "bethconv/record/strings.hpp"
#include "bethconv/script/pex.hpp"
#include "bethconv/texture/dds.hpp"
#include "bethconv/texture/mip_tail.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

int cmd_probe(const std::vector<std::filesystem::path>& paths) {
    int failures = 0;
    for (const auto& path : paths) {
        auto mapped = bethconv::io::MappedFile::open(path);
        if (!mapped) {
            std::fprintf(stderr, "error: %s\n", mapped.error().to_string().c_str());
            ++failures;
            continue;
        }
        auto reader = mapped->reader();
        const auto tag = reader.tag();
        std::printf("%-44s %12zu bytes  tag=%s\n", path.filename().string().c_str(),
                    mapped->size(), tag ? tag->to_string().c_str() : "<empty>");
    }
    return failures == 0 ? 0 : 1;
}

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

/// Mount every archive and loose dir given, in order; later mounts win ties.
int mount_all(bethconv::archive::ArchiveSet& set,
              const std::vector<std::filesystem::path>& paths) {
    int failures = 0;
    int priority = 0;
    for (const auto& path : paths) {
        const auto count = std::filesystem::is_directory(path)
                               ? set.mount_loose(path, priority)
                               : set.mount_archive(path, priority);
        if (!count) {
            std::fprintf(stderr, "warning: skipping %s: %s\n",
                         path.filename().string().c_str(),
                         count.error().to_string().c_str());
            ++failures;
        }
        ++priority;
    }
    return failures;
}

/// Adapt an archive set to strings.hpp's fetch callback, so the record layer
/// does not depend on the archive layer.
bethconv::record::StringFetch fetch_from(const bethconv::archive::ArchiveSet& set) {
    return [&set](std::string_view vpath) -> std::optional<std::vector<std::byte>> {
        auto bytes = set.read(vpath);
        if (!bytes) {
            return std::nullopt;
        }
        return std::move(*bytes);
    };
}

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
                fetch_from(set), plugin->name(), language, &problems);
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

int cmd_strings(const std::vector<std::filesystem::path>& sources,
                const std::vector<std::string>& plugins, const std::string& language,
                std::size_t list_count, const std::vector<std::string>& lookups) {
    bethconv::archive::ArchiveSet set;
    (void)mount_all(set, sources);

    std::size_t missing = 0;
    std::size_t repaired = 0;
    for (const auto& name : plugins) {
        std::vector<bethconv::io::ParseError> problems;
        const auto source =
            bethconv::record::load_string_source(fetch_from(set), name, language, &problems);
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

/// Mount a Data folder: every archive (alphabetically, not in load order), then
/// loose files, which win as in the game. The command reports how many
/// `strings/` paths have more than one provider rather than assuming none.
std::size_t mount_data_folder(bethconv::archive::ArchiveSet& set,
                              const std::filesystem::path& data_dir) {
    const auto plan = bethconv::install::plan_data_folder(data_dir);
    for (const auto& failure : bethconv::install::mount(set, plan)) {
        std::fprintf(stderr, "warning: skipping %s\n", failure.c_str());
    }
    return plan.archives.size() + plan.loose.size();
}

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

/// Build a load order as `merge` does, so `snapshot` and `merge` always agree.
bethconv::io::ParseResult<bethconv::record::LoadOrder> build_order(
    const std::filesystem::path& data_dir, const std::filesystem::path& list_file) {
    if (list_file.empty()) {
        return bethconv::record::LoadOrder::from_directory(data_dir);
    }
    auto list = bethconv::record::read_plugin_list(list_file);
    if (!list) {
        return std::unexpected(std::move(list).error());
    }
    return bethconv::record::LoadOrder::build(data_dir, *list);
}

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
        options.strings = fetch_from(set);
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

/// Check a records.fb, cheapest first: the form array, the indices against the
/// forms, and N spot-checked cells. `--against` also re-runs the merge and
/// compares every form.
int cmd_verify(const std::filesystem::path& path, std::size_t cells,
               const std::filesystem::path& against, const std::filesystem::path& against_list,
               const std::vector<std::string>& lookups, bool deep, bool verbose) {
    using namespace bethconv;

    auto snapshot = pack::Snapshot::open(path);
    if (!snapshot) {
        std::fprintf(stderr, "error: %s\n", snapshot.error().to_string().c_str());
        return 1;
    }

    // `--find`: what the written snapshot says about a form (cf. `merge --find`).
    for (const auto& lookup : lookups) {
        const auto id = record::FormId{static_cast<std::uint32_t>(std::stoul(lookup, nullptr, 0))};
        const auto form = snapshot->find(id);
        if (!form) {
            std::printf("\n%s: not in this snapshot\n", id.to_string().c_str());
            continue;
        }
        const auto plugins_by_index = snapshot->plugins();
        const auto name = [&](std::uint16_t i) {
            return i < plugins_by_index.size() ? plugins_by_index[i] : std::string_view("?");
        };
        std::printf("\n%s %s\n", form->type.to_string().c_str(), form->id.to_string().c_str());
        std::printf("  editor id  %.*s\n",
                    static_cast<int>(snapshot->editor_id_of(id).size()),
                    snapshot->editor_id_of(id).data());
        std::printf("  won by     %.*s\n", static_cast<int>(name(form->winner).size()),
                    name(form->winner).data());
        std::printf("  owned by   %.*s%s\n", static_cast<int>(name(form->owner).size()),
                    name(form->owner).data(), form->injected ? " (injected)" : "");
        std::printf("  overrides  %u\n", form->overrides);
        std::printf("  parent     %s\n",
                    form->parent.is_null() ? "-" : form->parent.to_string().c_str());
        std::printf("  flags      0x%08X%s\n", form->flags, form->deleted ? " DELETED" : "");
        std::printf("  payload    %zu bytes\n", form->payload.size());
        std::printf("  children   %zu\n", snapshot->children_of(id).size());
        if (form->type == io::FourCC{"CELL"}) {
            io::SpanReader body(form->payload, path.string());
            if (const auto cell = record::parse_cell(body, record::FormContext{})) {
                if (cell->grid) {
                    std::printf("  grid       (%d, %d)\n", cell->grid->x, cell->grid->y);
                } else {
                    std::printf("  grid       interior\n");
                }
            }
        }
    }

    std::printf("%s\n", path.string().c_str());
    std::printf("  pack format v%u, written by %.*s, names in %.*s\n", snapshot->format_version(),
                static_cast<int>(snapshot->converter().size()), snapshot->converter().data(),
                static_cast<int>(snapshot->language().size()), snapshot->language().data());
    const auto plugins = snapshot->plugins();
    std::printf("  %zu forms from %zu plugins\n", snapshot->size(), plugins.size());

    const auto merge = snapshot->merge_stats();
    std::printf("  the merge it came from: %llu visited, %llu collapsed, %llu deleted, "
                "%llu injected, %llu unresolved\n",
                static_cast<unsigned long long>(merge.visited),
                static_cast<unsigned long long>(merge.collapsed),
                static_cast<unsigned long long>(merge.deleted),
                static_cast<unsigned long long>(merge.injected),
                static_cast<unsigned long long>(merge.unresolved));

    int failures = 0;

    if (deep) {
        const bool intact = snapshot->blob_intact();
        std::printf("\npayload blob hash: %s\n", intact ? "matches the header" : "MISMATCH");
        failures += intact ? 0 : 1;
    }

    // Layer 1: the form array is sorted and unique and every form is found by
    // id. `find` relies on the order.
    std::printf("\nform array\n");
    std::uint32_t previous = 0;
    std::size_t unsorted = 0;
    std::size_t unfindable = 0;
    std::size_t no_payload = 0;
    for (std::size_t i = 0; i < snapshot->size(); ++i) {
        const auto form = snapshot->at(i);
        if (!form) {
            ++no_payload;
            continue;
        }
        if (i != 0 && form->id.value <= previous) {
            ++unsorted;
        }
        previous = form->id.value;
        if (!snapshot->find(form->id)) {
            ++unfindable;
        }
    }
    std::printf("  %zu out of order, %zu not findable by id, %zu with an unreadable payload\n",
                unsorted, unfindable, no_payload);
    failures += (unsorted != 0) + (unfindable != 0) + (no_payload != 0);

    // Layer 2: the indices agree with the form array.
    std::printf("\nindices\n");
    std::size_t type_mismatch = 0;
    std::size_t typed = 0;
    for (const auto type : record::defined_types()) {
        const auto listed = snapshot->of_type(type);
        typed += listed.size();
        for (const auto id : listed) {
            const auto form = snapshot->find(record::FormId{id});
            if (!form || form->type != type) {
                ++type_mismatch;
            }
        }
    }
    std::printf("  type index: %zu forms across the defined types, %zu disagree with the "
                "form array\n",
                typed, type_mismatch);
    failures += type_mismatch != 0;

    std::size_t parent_mismatch = 0;
    std::size_t checked_children = 0;
    for (std::size_t i = 0; i < snapshot->size(); i += 997) {
        const auto form = snapshot->at(i);
        if (!form || form->parent.is_null()) {
            continue;
        }
        const auto siblings = snapshot->children_of(form->parent);
        ++checked_children;
        if (std::ranges::find(siblings, form->id.value) == siblings.end()) {
            ++parent_mismatch;
        }
    }
    std::printf("  child index: %zu sampled forms with a parent, %zu missing from their "
                "parent's list\n",
                checked_children, parent_mismatch);
    failures += parent_mismatch != 0;

    // Layer 3: cell contents.
    std::printf("\ncell contents (%zu spot checks)\n", cells);
    const auto cell_forms = snapshot->of_type(io::FourCC{"CELL"});
    std::size_t checked = 0;
    std::size_t empty_cells = 0;
    std::size_t bad_refr = 0;
    std::uint64_t total_children = 0;
    if (!cell_forms.empty() && cells != 0) {
        // Spread across the array; the first N cells all come from one plugin
        // and mostly one worldspace.
        const std::size_t stride = std::max<std::size_t>(1, cell_forms.size() / cells);
        for (std::size_t i = 0; i < cell_forms.size() && checked < cells; i += stride) {
            const auto cell = snapshot->find(record::FormId{cell_forms[i]});
            if (!cell) {
                continue;
            }
            ++checked;
            const auto children = snapshot->children_of(cell->id);
            total_children += children.size();
            if (children.empty()) {
                ++empty_cells;
            }

            // Parse the contents: the payload must still be a CELL and its
            // children REFRs.
            io::SpanReader body(cell->payload, path.string());
            const record::FormContext ctx{};
            const auto parsed = record::parse_cell(body, ctx);
            std::size_t refrs = 0;
            for (const auto child : children) {
                const auto record = snapshot->find(record::FormId{child});
                if (!record || record->type != io::FourCC{"REFR"}) {
                    continue;
                }
                io::SpanReader refr_body(record->payload, path.string());
                record::RecordHeader header{};
                header.flags = record->flags;
                if (record::parse_reference(header, refr_body, ctx)) {
                    ++refrs;
                } else {
                    ++bad_refr;
                }
            }
            if (verbose) {
                std::printf("  %s %-28s %zu children, %zu REFR parsed%s\n",
                            cell->id.to_string().c_str(),
                            parsed ? parsed->editor_id.c_str() : "<unparsed>", children.size(),
                            refrs, parsed ? "" : "  [CELL DID NOT PARSE]");
            }
            failures += parsed ? 0 : 1;
        }
    }
    std::printf("  %zu cells checked, %llu children, %zu empty, %zu REFR would not parse\n",
                checked, static_cast<unsigned long long>(total_children), empty_cells, bad_refr);
    failures += bad_refr != 0;

    // The grid index: each entry must name a CELL in that worldspace.
    std::printf("\nworldspace grid\n");
    std::size_t grid_checked = 0;
    std::size_t grid_wrong = 0;
    std::size_t persistent_cells = 0;
    for (const auto world : snapshot->worlds()) {
        for (const auto id : snapshot->children_of(world)) {
            const auto form = snapshot->find(record::FormId{id});
            if (!form || form->type != io::FourCC{"CELL"}) {
                continue;
            }
            // Persistent cells (XCLC 0,0) are not in the grid index; see
            // src/pack/snapshot_writer.cpp.
            if (record::has_flag(form->flags, record::RecordFlag::persistent)) {
                ++persistent_cells;
                continue;
            }
            io::SpanReader body(form->payload, path.string());
            const auto parsed = record::parse_cell(body, record::FormContext{});
            if (!parsed || !parsed->grid) {
                continue;
            }
            ++grid_checked;
            const auto found = snapshot->cell_at(world, parsed->grid->x, parsed->grid->y);
            if (!found || *found != form->id) {
                ++grid_wrong;
                // Name the conflict: a different cell means two records claim
                // the square (a data fact); none means the index dropped a cell
                // (a bug).
                const auto other = found ? snapshot->editor_id_of(*found) : std::string_view{};
                std::printf("  %s %-24s at (%d, %d) in world %s -> %s %.*s\n",
                            form->id.to_string().c_str(),
                            parsed->editor_id.empty() ? "<no EDID>" : parsed->editor_id.c_str(),
                            parsed->grid->x, parsed->grid->y, world.to_string().c_str(),
                            found ? found->to_string().c_str() : "nothing",
                            static_cast<int>(other.size()), other.data());
            }
            if (grid_checked >= 2000) {
                break;
            }
        }
        if (grid_checked >= 2000) {
            break;
        }
    }
    std::printf("  %zu worldspaces, %zu exterior cells looked up by grid, %zu did not come "
                "back (%zu persistent cells skipped)\n",
                snapshot->worlds().size(), grid_checked, grid_wrong, persistent_cells);
    failures += grid_wrong != 0;

    // Layer 4: re-run the merge and compare every form.
    if (!against.empty()) {
        std::printf("\nround-trip against %s\n", against.string().c_str());
        auto order = build_order(against, against_list);
        if (!order) {
            std::fprintf(stderr, "error: %s\n", order.error().to_string().c_str());
            return 1;
        }
        archive::ArchiveSet set;
        (void)mount_data_folder(set, against);
        record::MergeOptions options;
        options.language = std::string(snapshot->language());
        options.strings = fetch_from(set);
        const auto world = record::MergedWorld::build(*order, options);

        std::size_t missing = 0;
        std::size_t differs = 0;
        for (const auto& merged : world.records()) {
            const auto stored = snapshot->find(merged.form);
            if (!stored) {
                ++missing;
                continue;
            }
            if (stored->type != merged.type || stored->parent != merged.parent ||
                stored->flags != merged.flags || stored->overrides != merged.overrides ||
                stored->deleted != merged.deleted || stored->injected != merged.injected) {
                ++differs;
            }
        }
        std::printf("  the merge has %llu forms, the snapshot %zu\n",
                    static_cast<unsigned long long>(world.stats().forms), snapshot->size());
        std::printf("  %zu absent from the snapshot, %zu with different provenance\n", missing,
                    differs);
        failures += (missing != 0) + (differs != 0) +
                    (world.stats().forms != snapshot->size() ? 1 : 0);
    }

    std::printf("\n%s\n", failures == 0 ? "verify: clean" : "verify: FAILED");
    return failures == 0 ? 0 : 1;
}

int cmd_merge(const std::filesystem::path& data_dir, const std::filesystem::path& list_file,
              const std::vector<std::filesystem::path>& sources, const std::string& language,
              const std::vector<std::string>& type_names, std::size_t sample,
              const std::vector<std::string>& lookups, bool no_strings, bool no_second_pass) {
    bethconv::record::LoadOrderOptions lo_options;
    std::optional<bethconv::record::LoadOrder> order;
    if (list_file.empty()) {
        auto built = bethconv::record::LoadOrder::from_directory(data_dir, lo_options);
        if (!built) {
            std::fprintf(stderr, "error: %s\n", built.error().to_string().c_str());
            return 1;
        }
        order = std::move(*built);
    } else {
        auto list = bethconv::record::read_plugin_list(list_file);
        if (!list) {
            std::fprintf(stderr, "error: %s\n", list.error().to_string().c_str());
            return 1;
        }
        order = bethconv::record::LoadOrder::build(data_dir, *list, lo_options);
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
        options.strings = fetch_from(set);
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

/// Read one virtual path per line, ignoring blanks and `#` comments. Lets many
/// files share one mount (mounting SE's texture archives takes ~0.8 s).
std::vector<std::string> read_vpath_list(const std::filesystem::path& path, bool& ok) {
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", path.string().c_str());
        ok = false;
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        out.push_back(line);
    }
    ok = true;
    return out;
}

bool write_bytes(const std::filesystem::path& out, std::span<const std::byte> data) {
    std::error_code ec;
    if (out.has_parent_path()) {
        std::filesystem::create_directories(out.parent_path(), ec);
    }
    std::string error;
    if (!bethconv::io::write_file(out, data, error)) {
        std::fprintf(stderr, "error: cannot write %s: %s\n", out.string().c_str(), error.c_str());
        return false;
    }
    return true;
}

int cmd_extract(const std::vector<std::filesystem::path>& paths,
                std::vector<std::string> vpaths, const std::filesystem::path& list_file,
                const std::filesystem::path& out, bool quiet) {
    if (!list_file.empty()) {
        bool ok = false;
        auto listed = read_vpath_list(list_file, ok);
        if (!ok) {
            return 1;
        }
        vpaths.insert(vpaths.end(), std::make_move_iterator(listed.begin()),
                      std::make_move_iterator(listed.end()));
    }
    if (vpaths.empty()) {
        std::fprintf(stderr, "error: no virtual paths given\n");
        return 1;
    }

    bethconv::archive::ArchiveSet set;
    mount_all(set, paths);

    // One path: -o is the file. Several: -o is a root mirroring the virtual
    // tree (what glTF texture URIs resolve against).
    const bool out_is_dir = vpaths.size() > 1;

    std::size_t ok_count = 0;
    std::size_t missing = 0;
    std::size_t bytes = 0;
    for (const std::string& vpath : vpaths) {
        auto data = set.read(vpath);
        if (!data) {
            std::fprintf(stderr, "error: %s\n", data.error().to_string().c_str());
            ++missing;
            continue;
        }
        ++ok_count;
        bytes += data->size();

        const auto resolution = set.resolve(vpath);
        if (!quiet) {
            std::printf("%s: %zu bytes from %s\n", resolution->vpath.c_str(), data->size(),
                        set.sources()[resolution->winner].name.c_str());
        }

        if (out.empty()) {
            continue;
        }
        if (out_is_dir && !bethconv::archive::is_safe_relative(resolution->vpath)) {
            std::fprintf(stderr, "error: %s: not a safe relative path, not written\n",
                         resolution->vpath.c_str());
            ++missing;
            continue;
        }
        const std::filesystem::path target =
            out_is_dir ? out / std::filesystem::path(resolution->vpath) : out;
        if (!write_bytes(target, *data)) {
            return 1;
        }
        if (!quiet) {
            std::printf("wrote %s\n", target.string().c_str());
        }
    }

    if (vpaths.size() > 1) {
        std::printf("%zu extracted, %zu missing, %.1f MiB\n", ok_count, missing,
                    static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return missing == 0 ? 0 : 1;
}


/// Bounds of `points` times `scale` (game units), before any node transform.
void print_bounds(const std::vector<bethconv::mesh::Vec3>& points, float scale) {
    if (points.empty()) {
        return;
    }
    std::array<float, 3> lo{points[0].x, points[0].y, points[0].z};
    std::array<float, 3> hi = lo;
    for (const auto& v : points) {
        const std::array<float, 3> p{v.x, v.y, v.z};
        for (std::size_t k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    std::printf("      bounds (%.1f %.1f %.1f) to (%.1f %.1f %.1f)\n",
                static_cast<double>(lo[0] * scale), static_cast<double>(lo[1] * scale),
                static_cast<double>(lo[2] * scale), static_cast<double>(hi[0] * scale),
                static_cast<double>(hi[1] * scale), static_cast<double>(hi[2] * scale));
}

/// Result of one NIF, for the summary and sweep totals.
struct MeshTally {
    std::size_t files = 0;
    std::size_t failed = 0;
    std::size_t primitives = 0;
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    std::size_t skinned = 0;
    std::size_t collision_shapes = 0;
    std::size_t warnings = 0;
    std::size_t clips = 0;
    std::size_t channels = 0;
    std::size_t particle_systems = 0;
    std::size_t glb_bytes = 0;
    std::size_t le = 0;
    std::size_t se = 0;
};

/// Sources may be archives, loose dirs or plain .nif files, so a single
/// extracted file can be converted without a mount.
int cmd_mesh(const std::vector<std::filesystem::path>& sources,
             const std::vector<std::string>& vpaths, const std::string& filter,
             const std::filesystem::path& out_dir, bool inspect, std::size_t limit,
             bool no_collision, bool no_skinning, bool keep_z_up, float unit_scale,
             bool verbose) {
    bethconv::mesh::ReadOptions read_options;
    read_options.read_collision = !no_collision;
    read_options.read_skinning = !no_skinning;

    bethconv::mesh::WriteOptions write_options;
    write_options.convert_to_y_up = !keep_z_up;
    write_options.unit_scale = unit_scale;

    bethconv::archive::ArchiveSet set;
    std::vector<std::filesystem::path> mounts;
    std::vector<std::filesystem::path> loose_nifs;
    for (const auto& source : sources) {
        if (!std::filesystem::is_directory(source) && source.extension() == ".nif") {
            loose_nifs.push_back(source);
        } else {
            mounts.push_back(source);
        }
    }
    mount_all(set, mounts);

    // Explicit vpaths, or every .nif in the mount, filtered.
    std::vector<std::string> work = vpaths;
    if (work.empty() && !mounts.empty()) {
        set.for_each([&](const bethconv::archive::Resolution& entry) {
            if (limit != 0 && work.size() >= limit) {
                return;
            }
            if (!entry.vpath.ends_with(".nif")) {
                return;
            }
            if (!filter.empty() && entry.vpath.find(filter) == std::string::npos) {
                return;
            }
            work.push_back(entry.vpath);
        });
        // for_each walks a hash index; sort so truncated sweeps are
        // reproducible.
        std::ranges::sort(work);
    }

    MeshTally tally;
    const auto started = std::chrono::steady_clock::now();

    auto handle = [&](std::string_view origin, std::span<const std::byte> bytes,
                      const std::filesystem::path& out_name) {
        ++tally.files;
        auto model = bethconv::mesh::read_nif(bytes, origin, read_options);
        if (!model) {
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", model.error().to_string().c_str());
            return;
        }

        std::size_t vertices = 0;
        std::size_t triangles = 0;
        for (const auto& prim : model->primitives) {
            vertices += prim.positions.size();
            triangles += prim.indices.size() / 3;
            if (prim.skin.has_value()) {
                ++tally.skinned;
            }
        }
        tally.primitives += model->primitives.size();
        tally.vertices += vertices;
        tally.triangles += triangles;
        tally.collision_shapes += model->collision.size();
        tally.warnings += model->warnings.size();
        tally.clips += model->animations.size();
        for (const auto& clip : model->animations) {
            tally.channels += clip.channels.size();
        }
        tally.particle_systems += model->particles.size();
        switch (bethconv::mesh::flavor_of(model->nif_stream)) {
        case bethconv::mesh::NifFlavor::le: ++tally.le; break;
        case bethconv::mesh::NifFlavor::se: ++tally.se; break;
        case bethconv::mesh::NifFlavor::unknown: break;
        }

        if (inspect || verbose) {
            std::printf("%s\n", std::string(origin).c_str());
            std::printf("  %s  stream=%u (%s)  nodes=%zu shapes=%zu materials=%zu "
                        "skins=%zu collision=%zu\n",
                        model->nif_version.c_str(), model->nif_stream,
                        std::string(to_string(bethconv::mesh::flavor_of(model->nif_stream)))
                            .c_str(),
                        model->nodes.size(), model->primitives.size(),
                        model->materials.size(), model->skins.size(),
                        model->collision.size());
            for (const auto& prim : model->primitives) {
                const auto& mat = model->materials[prim.material];
                std::printf("    %-40s v=%-6zu tri=%-6zu %s%s%s%s mat=%u %s\n",
                            prim.name.c_str(), prim.positions.size(),
                            prim.indices.size() / 3,
                            prim.normals.empty() ? "-" : "N",
                            prim.tangents.empty() ? "-" : "T",
                            prim.uvs.empty() ? "-" : "U",
                            prim.colors.empty() ? "-" : "C", mat.bs_shader_type,
                            mat.textures[0].c_str());
                if (verbose) {
                    print_bounds(prim.positions, 1.0f);
                }
            }
            for (const auto& shape : model->collision) {
                std::printf("    collision %-18s verts=%zu tris=%zu\n",
                            shape.block_name.c_str(), shape.vertices.size(),
                            shape.indices.size() / 3);
                if (verbose) {
                    print_bounds(shape.vertices, bethconv::mesh::k_havok_scale);
                }
            }
            for (const auto& clip : model->animations) {
                std::printf("    clip '%s'%s %.2f-%.2fs x%.2f, %zu channels\n",
                            clip.name.c_str(), clip.autoplay ? " autoplay" : "",
                            static_cast<double>(clip.start), static_cast<double>(clip.stop),
                            static_cast<double>(clip.frequency), clip.channels.size());
                if (verbose) {
                    for (const auto& ch : clip.channels) {
                        std::printf("      %-28s %-26s %zu keys\n",
                                    model->nodes[ch.node].name.c_str(), ch.property.c_str(),
                                    ch.times.size());
                    }
                }
            }
            for (const auto& ps : model->particles) {
                std::printf("    particles '%s' max=%u emitters=%zu%s%s\n",
                            model->nodes[ps.node].name.c_str(), ps.max_particles,
                            ps.emitters.size(), ps.world_space ? " world" : " local",
                            ps.strip ? " strip" : "");
                for (const auto& em : ps.emitters) {
                    std::printf("      emitter rate=%.1f/s life=%.2f±%.2f speed=%.1f "
                                "radius=%.1f\n",
                                static_cast<double>(em.birth_rate),
                                static_cast<double>(em.life_span),
                                static_cast<double>(em.life_span_variation),
                                static_cast<double>(em.speed), static_cast<double>(em.radius));
                }
            }
        }
        for (const auto& warning : model->warnings) {
            std::fprintf(stderr, "warning: %s: %s\n", std::string(origin).c_str(),
                         warning.c_str());
        }

        if (inspect || out_dir.empty()) {
            return;
        }
        const auto written =
            bethconv::mesh::write_glb_file(*model, out_dir / out_name, write_options);
        if (!written) {
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", written.error().to_string().c_str());
            return;
        }
        tally.glb_bytes += *written;
    };

    for (const auto& path : loose_nifs) {
        auto mapped = bethconv::io::MappedFile::open(path);
        if (!mapped) {
            ++tally.files;
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", mapped.error().to_string().c_str());
            continue;
        }
        handle(path.string(), mapped->bytes(),
               std::filesystem::path(path).filename().replace_extension(".glb"));
    }

    for (const auto& vpath : work) {
        auto bytes = set.read(vpath);
        if (!bytes) {
            ++tally.files;
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", bytes.error().to_string().c_str());
            continue;
        }
        const std::string target = bethconv::archive::normalize_vpath(vpath);
        if (!out_dir.empty() && !inspect && !bethconv::archive::is_safe_relative(target)) {
            ++tally.files;
            ++tally.failed;
            std::fprintf(stderr, "error: %s: not a safe relative path, not written\n",
                         vpath.c_str());
            continue;
        }
        std::filesystem::path out_name(target);
        out_name.replace_extension(".glb");
        handle(vpath, *bytes, out_name);
    }

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("\n%zu files (%zu LE, %zu SE), %zu failed, %zu shapes, %zu vertices, "
                "%zu triangles, %zu skinned, %zu collision shapes, %zu clips, %zu channels, "
                "%zu particle systems, %zu warnings",
                tally.files, tally.le, tally.se, tally.failed, tally.primitives,
                tally.vertices, tally.triangles, tally.skinned, tally.collision_shapes,
                tally.clips, tally.channels, tally.particle_systems, tally.warnings);
    if (tally.glb_bytes != 0) {
        std::printf(", %.1f MiB written",
                    static_cast<double>(tally.glb_bytes) / (1024.0 * 1024.0));
    }
    std::printf(" in %.1fs\n", elapsed);
    return tally.failed == 0 ? 0 : 1;
}

/// Texture sweep totals. Formats are counted by name, which doubles as a
/// census of an install.
struct TextureTally {
    std::size_t files = 0;
    std::size_t failed = 0;
    std::size_t completed = 0;
    std::size_t already_complete = 0;
    std::size_t single_level = 0;
    std::size_t unsupported = 0;
    std::size_t short_chains = 0;
    std::size_t cubemaps = 0;
    std::size_t volumes = 0;
    std::size_t dx10 = 0;
    std::size_t written = 0;
    std::size_t bytes_read = 0;
    std::size_t bytes_written = 0;
    std::size_t bytes_added = 0;
    std::size_t bytes_dropped = 0;
    std::map<std::string, std::size_t> formats;
};

/// Desktop textures are passed through, with short mip chains completed for
/// Godot. Reads headers, completes chains, copies bytes; never decodes.
/// Decode every `.pex` in the sources (or the named ones) and count what they
/// contain; `dump` prints each one's disassembly instead.
int cmd_script(const std::vector<std::filesystem::path>& sources, std::vector<std::string> vpaths,
               const std::string& filter, bool dump) {
    bethconv::archive::ArchiveSet set;
    mount_all(set, sources);
    if (vpaths.empty()) {
        set.for_each([&](const bethconv::archive::Resolution& entry) {
            if (entry.vpath.ends_with(".pex") &&
                (filter.empty() || entry.vpath.find(filter) != std::string::npos)) {
                vpaths.push_back(entry.vpath);
            }
        });
        std::ranges::sort(vpaths);
    }
    std::size_t scripts = 0;
    std::size_t failed = 0;
    std::size_t objects = 0;
    std::size_t functions = 0;
    std::size_t natives = 0;
    std::size_t instructions = 0;
    std::size_t with_lines = 0;
    std::size_t debug = 0;
    std::array<std::size_t, bethconv::script::k_pex_op_count> ops{};
    const auto started = std::chrono::steady_clock::now();
    for (const auto& vpath : vpaths) {
        auto bytes = set.read(vpath);
        if (!bytes) {
            ++failed;
            std::fprintf(stderr, "error: %s\n", bytes.error().to_string().c_str());
            continue;
        }
        auto script = bethconv::script::read_pex_script(*bytes, vpath);
        ++scripts;
        if (!script) {
            ++failed;
            std::fprintf(stderr, "error: %s\n", script.error().to_string().c_str());
            continue;
        }
        if (dump) {
            std::printf("; %s\n%s\n", vpath.c_str(), bethconv::script::disassemble(*script).c_str());
            continue;
        }
        debug += script->has_debug_info ? 1U : 0U;
        const auto count = [&](const bethconv::script::PexFunction& f) {
            ++functions;
            natives += f.is_native() ? 1U : 0U;
            instructions += f.opcodes.size();
            with_lines += f.lines.empty() ? 0U : 1U;
            for (const auto op : f.opcodes) {
                ++ops[static_cast<std::size_t>(op)];
            }
        };
        for (const auto& o : script->objects) {
            ++objects;
            for (const auto& p : o.properties) {
                if (p.getter) {
                    count(*p.getter);
                }
                if (p.setter) {
                    count(*p.setter);
                }
            }
            for (const auto& state : o.states) {
                for (const auto& f : state.functions) {
                    count(f);
                }
            }
        }
    }
    if (dump) {
        return failed == 0 ? 0 : 1;
    }
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("%zu scripts, %zu failed, %zu with debug info | %zu objects, %zu functions "
                "(%zu native, %zu with line numbers), %zu instructions | %.1f s\n",
                scripts, failed, debug, objects, functions, natives, with_lines, instructions,
                elapsed);
    std::printf("opcodes:");
    for (std::size_t i = 0; i < ops.size(); ++i) {
        std::printf(" %s %zu", std::string(bethconv::script::op_info(
                                               static_cast<bethconv::script::PexOp>(i)).name)
                                   .c_str(),
                    ops[i]);
    }
    std::printf("\n");
    return failed == 0 ? 0 : 1;
}

int cmd_texture(const std::vector<std::filesystem::path>& sources,
                std::vector<std::string> vpaths, const std::filesystem::path& list_file,
                const std::string& filter, const std::filesystem::path& out_dir, bool inspect,
                bool no_fix, std::size_t limit, bool verbose, bool quiet) {
    // A list file lets thousands of paths share one mount (nine minutes one
    // process at a time vs. 4.3 s in one).
    if (!list_file.empty()) {
        bool ok = false;
        auto listed = read_vpath_list(list_file, ok);
        if (!ok) {
            return 1;
        }
        vpaths.insert(vpaths.end(), std::make_move_iterator(listed.begin()),
                      std::make_move_iterator(listed.end()));
    }

    bethconv::archive::ArchiveSet set;
    std::vector<std::filesystem::path> mounts;
    std::vector<std::filesystem::path> loose_textures;
    for (const auto& source : sources) {
        if (!std::filesystem::is_directory(source) && source.extension() == ".dds") {
            loose_textures.push_back(source);
        } else {
            mounts.push_back(source);
        }
    }
    mount_all(set, mounts);

    std::vector<std::string> work = vpaths;
    if (work.empty() && !mounts.empty()) {
        set.for_each([&](const bethconv::archive::Resolution& entry) {
            if (limit != 0 && work.size() >= limit) {
                return;
            }
            if (!entry.vpath.ends_with(".dds")) {
                return;
            }
            if (!filter.empty() && entry.vpath.find(filter) == std::string::npos) {
                return;
            }
            work.push_back(entry.vpath);
        });
        // Sorted, as for meshes, so truncated runs are reproducible.
        std::ranges::sort(work);
    }

    TextureTally tally;
    const auto started = std::chrono::steady_clock::now();

    auto handle = [&](std::string_view origin, std::span<const std::byte> bytes,
                      const std::filesystem::path& out_name) {
        ++tally.files;
        tally.bytes_read += bytes.size();

        auto info = bethconv::texture::parse_dds(bytes, origin);
        if (!info) {
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", info.error().to_string().c_str());
            return;
        }
        ++tally.formats[info->layout.name];
        if (info->short_chain()) {
            ++tally.short_chains;
        }
        switch (info->kind) {
        case bethconv::texture::SurfaceKind::cubemap: ++tally.cubemaps; break;
        case bethconv::texture::SurfaceKind::volume: ++tally.volumes; break;
        case bethconv::texture::SurfaceKind::texture_2d: break;
        }
        if (info->dx10) {
            ++tally.dx10;
        }

        // With --no-fix, `fix` stays default, so the control run reports no
        // chain work.
        bethconv::texture::TailFix fix;
        if (!no_fix) {
            auto completed = bethconv::texture::complete_mip_tail(bytes, *info, origin);
            if (!completed) {
                ++tally.failed;
                std::fprintf(stderr, "error: %s\n", completed.error().to_string().c_str());
                return;
            }
            fix = std::move(*completed);
            switch (fix.outcome) {
            case bethconv::texture::TailOutcome::completed: ++tally.completed; break;
            case bethconv::texture::TailOutcome::already_complete:
                ++tally.already_complete;
                break;
            case bethconv::texture::TailOutcome::single_level: ++tally.single_level; break;
            case bethconv::texture::TailOutcome::unsupported: ++tally.unsupported; break;
            }
        }
        tally.bytes_added += fix.added_bytes;
        tally.bytes_dropped += fix.dropped_bytes;

        if ((inspect && !quiet) || verbose) {
            std::printf("%-58s %5ux%-5u %-8s %-7s %2u/%-2u levels  %s", std::string(origin).c_str(),
                        info->width, info->height, info->layout.name.c_str(),
                        std::string(to_string(info->kind)).c_str(), info->stored_levels,
                        info->full_chain_levels,
                        std::string(to_string(fix.outcome)).c_str());
            if (fix.outcome == bethconv::texture::TailOutcome::completed) {
                std::printf(" +%zu bytes", fix.added_bytes);
            }
            if (fix.dropped_bytes != 0) {
                std::printf(" (dropped %zu trailing bytes)", fix.dropped_bytes);
            }
            std::printf("\n");
        }

        if (inspect || out_dir.empty()) {
            return;
        }
        const bool rebuilt = fix.outcome == bethconv::texture::TailOutcome::completed;
        const std::span<const std::byte> payload = rebuilt ? std::span<const std::byte>(fix.data)
                                                           : bytes;
        std::string error;
        if (!bethconv::io::write_file(out_dir / out_name, payload, error)) {
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return;
        }
        ++tally.written;
        tally.bytes_written += payload.size();
    };

    for (const auto& path : loose_textures) {
        auto mapped = bethconv::io::MappedFile::open(path);
        if (!mapped) {
            ++tally.files;
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", mapped.error().to_string().c_str());
            continue;
        }
        handle(path.string(), mapped->bytes(), std::filesystem::path(path).filename());
    }

    for (const auto& vpath : work) {
        auto bytes = set.read(vpath);
        if (!bytes) {
            ++tally.files;
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", bytes.error().to_string().c_str());
            continue;
        }
        const std::string target = bethconv::archive::normalize_vpath(vpath);
        if (!out_dir.empty() && !inspect && !bethconv::archive::is_safe_relative(target)) {
            ++tally.files;
            ++tally.failed;
            std::fprintf(stderr, "error: %s: not a safe relative path, not written\n",
                         vpath.c_str());
            continue;
        }
        handle(vpath, *bytes, std::filesystem::path(target));
    }

    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!tally.formats.empty()) {
        std::printf("\nformats:");
        // Most common first.
        std::vector<std::pair<std::string, std::size_t>> by_count(tally.formats.begin(),
                                                                  tally.formats.end());
        std::ranges::sort(by_count, [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        for (const auto& [name, count] : by_count) {
            std::printf("  %s %zu", name.c_str(), count);
        }
        std::printf("\n");
    }
    std::printf("%zu files, %zu failed | %zu cubemap, %zu volume, %zu dx10 | "
                "%zu short chains",
                tally.files, tally.failed, tally.cubemaps, tally.volumes, tally.dx10,
                tally.short_chains);
    if (no_fix) {
        std::printf(" (left short: --no-fix)\n");
    } else {
        std::printf(" -> %zu completed, %zu already complete, %zu single-level, "
                    "%zu unsupported\n",
                    tally.completed, tally.already_complete, tally.single_level,
                    tally.unsupported);
    }
    std::printf("%.1f MiB read", static_cast<double>(tally.bytes_read) / (1024.0 * 1024.0));
    if (tally.written != 0) {
        std::printf(", %zu written (%.1f MiB, %zu bytes appended)", tally.written,
                    static_cast<double>(tally.bytes_written) / (1024.0 * 1024.0),
                    tally.bytes_added);
    }
    if (tally.bytes_dropped != 0) {
        std::printf(", %zu trailing bytes dropped", tally.bytes_dropped);
    }
    std::printf(" in %.1fs\n", elapsed);
    return tally.failed == 0 ? 0 : 1;
}

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


/// check_target (front_end.hpp), printed: refusals as errors, the rest as
/// warnings.
bool output_target_ok(const std::filesystem::path& out, bool many_files, bool allow) {
    const auto verdict = bethconv::cli::check_target(out, many_files, allow);
    if (verdict.level == bethconv::cli::TargetLevel::ok) {
        return true;
    }
    const bool refused = verdict.level == bethconv::cli::TargetLevel::refuse;
    std::fprintf(stderr, "%s: %s\n", refused ? "error" : "warning", verdict.reason.c_str());
    return !refused;
}

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
    bool no_mip_fix = false;
    bool no_collision = false;
    bool no_skinning = false;
    bool keep_z_up = false;
    float unit_scale = 0.0142875f;
    std::uint32_t max_texture_size = 0;
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
    bethconv::pack::InputRecord input;
    input.kind = args.mo2.empty() ? "data" : "mo2";
    input.edition = std::string(bethconv::install::to_string(bethconv::install::identify(args.data_dir)));
    input.data = path_text(args.data_dir);

    bethconv::install::MountPlan plan;
    if (!args.mo2.empty()) {
        auto instance = bethconv::install::read_mo2_instance(args.mo2);
        if (!instance) {
            return fail(1, instance.error().to_string());
        }
        auto profile = bethconv::install::read_mo2_profile(*instance, args.mo2_profile);
        if (!profile) {
            return fail(1, profile.error().to_string());
        }
        if (instance->edition != bethconv::install::Edition::unknown) {
            input.edition = std::string(bethconv::install::to_string(instance->edition));
        }
        input.mo2_instance = path_text(instance->dir);
        input.mo2_profile = profile->name;
        input.mods = profile->mods.size();
        input.plugin_list = path_text(profile->plugins_file);
        plan = bethconv::install::plan_mo2(args.data_dir, *instance, *profile);
        std::fprintf(text, "profile %s: %zu mods enabled, %zu disabled, %zu missing\n",
                     profile->name.c_str(), profile->mods.size(), profile->disabled,
                     profile->missing.size());
        for (const auto& name : profile->missing) {
            std::fprintf(text, "  missing mod folder: %s\n", name.c_str());
        }
    } else if (args.sources.empty()) {
        plan = bethconv::install::plan_data_folder(args.data_dir);
    }
    if (!args.list_file.empty()) {
        auto list = bethconv::record::read_plugin_list(args.list_file);
        if (!list) {
            return fail(1, list.error().to_string());
        }
        plan.plugins = std::move(*list);
        input.plugin_list = path_text(args.list_file);
    }

    std::optional<bethconv::record::LoadOrder> order;
    if (plan.plugins) {
        auto dirs = plan.plugin_dirs;
        if (dirs.empty()) {
            dirs.push_back(args.data_dir);
        }
        bethconv::record::LoadOrderOptions order_options;
        order_options.always_loaded = bethconv::install::creation_club_plugins(args.data_dir);
        order = bethconv::record::LoadOrder::build(dirs, *plan.plugins, order_options);
    } else {
        auto built = build_order(args.data_dir, {});
        if (!built) {
            return fail(1, built.error().to_string());
        }
        order = std::move(*built);
    }
    std::fprintf(text, "%zu plugins in the order, %zu problems\n", order->entries().size(),
                 order->problems().size());
    for (const auto& problem : order->problems()) {
        std::fprintf(text, "  %s\n", problem.to_string().c_str());
    }

    bethconv::archive::ArchiveSet set;
    std::vector<std::string> mount_failures;
    if (!args.sources.empty() && args.mo2.empty()) {
        (void)mount_all(set, args.sources);
    } else {
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
        mount_failures = bethconv::install::mount(set, plan, mounted);
        for (const auto& failure : mount_failures) {
            std::fprintf(stderr, "warning: skipping %s\n", failure.c_str());
        }
        std::fprintf(text, "mounted %zu archives and %zu folders\n", plan.archives.size(),
                     plan.loose.size());
    }
    if (!plan.unloaded_archives.empty()) {
        std::fprintf(text, "%zu mod archives not mounted: no loaded plugin is named like them\n",
                     plan.unloaded_archives.size());
    }
    std::fprintf(text, "%zu unique virtual paths\n", set.unique_paths());

    if (args.json) {
        auto problems = ordered_json::array();
        for (const auto& problem : order->problems()) {
            problems.push_back(problem.to_string());
        }
        auto failures = ordered_json::array();
        for (const auto& failure : mount_failures) {
            failures.push_back(failure);
        }
        auto unloaded = ordered_json::array();
        for (const auto& path : plan.unloaded_archives) {
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
                          {"plugins", order->entries().size()},
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
    options.fix_mip_tail = !args.no_mip_fix;
    options.max_texture_size = args.max_texture_size;
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

    auto result = bethconv::pack::convert(set, *order, options);
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
        std::fprintf(text, "                %llu unresolved, %llu parse errors, %llu script errors\n",
                     static_cast<unsigned long long>(w.unresolved),
                     static_cast<unsigned long long>(w.parse_errors),
                     static_cast<unsigned long long>(w.script_errors));
    }
    std::fprintf(text, "  assets        %llu written, %llu deduped, %llu distinct "
                 "(%llu meshes, %llu textures, %llu scripts, %llu LOD)\n",
                 static_cast<unsigned long long>(stats.converted),
                 static_cast<unsigned long long>(stats.deduped),
                 static_cast<unsigned long long>(stats.distinct_assets),
                 static_cast<unsigned long long>(stats.meshes),
                 static_cast<unsigned long long>(stats.textures),
                 static_cast<unsigned long long>(stats.scripts),
                 static_cast<unsigned long long>(stats.lod));
    std::fprintf(text, "                %.1f MiB written, %.1f MiB not re-converted\n",
                 static_cast<double>(stats.asset_bytes) / (1024.0 * 1024.0),
                 static_cast<double>(stats.dedupe_saved_bytes) / (1024.0 * 1024.0));
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
                                    {"bytes_written", stats.asset_bytes},
                                    {"store_bytes", stats.store_bytes}}},
            {"textures", ordered_json{{"max_size", args.max_texture_size},
                                      {"shrunk", result->textures_shrunk},
                                      {"kept_large", result->textures_kept_large},
                                      {"bytes_saved", result->texture_bytes_saved}}},
            {"failed", stats.failed},
            {"warnings", stats.warnings},
            {"orphaned_assets", stats.orphaned_assets},
            {"pruned", args.prune},
            {"first_failures", std::move(failures)}});
    }
    return exit_code;
}


/// Game units per exterior cell side.
constexpr float k_cell_units = 4096.0F;

/// The exterior region `grid` +- `radius` of worldspace `world_name`: its
/// cells, the persistent references positioned inside it, and terrain (from
/// the parent worldspace when the world uses its parent's land). With
/// `models`, print unique model and land texture paths (input for
/// `view --from`).
int cmd_cell_region(const bethconv::pack::WorldFile& world, const std::string& world_name,
                    const std::string& grid, int radius, bool models) {
    const auto worlds = world.worldspaces();
    const auto lower = [](std::string text) {
        std::ranges::transform(text, text.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const auto ws = std::ranges::find_if(
        worlds, [&](const auto& w) { return lower(w.editor_id) == lower(world_name); });
    if (ws == worlds.end()) {
        std::fprintf(stderr, "error: no worldspace %s\n", world_name.c_str());
        return 1;
    }
    int cx = 0;
    int cy = 0;
    if (!grid.empty()) {
        const char* end = grid.data() + grid.size();
        const auto x = std::from_chars(grid.data(), end, cx);
        const auto y = x.ec == std::errc{} && x.ptr != end && *x.ptr == ','
                           ? std::from_chars(x.ptr + 1, end, cy)
                           : std::from_chars_result{x.ptr, std::errc::invalid_argument};
        if (y.ec != std::errc{} || y.ptr != end) {
            std::fprintf(stderr, "error: --grid wants X,Y\n");
            return 1;
        }
    }
    // Bit 0 of PNAM: land data comes from the parent.
    const std::uint32_t land_world =
        (ws->parent != 0 && (ws->parent_flags & 0x1u) != 0) ? ws->parent : ws->id;

    std::vector<bethconv::pack::WorldRef> refs;
    std::set<std::uint32_t> land_textures;
    std::set<std::uint32_t> waters;
    if (ws->water != 0) {
        waters.insert(ws->water);
    }
    std::size_t cells = 0;
    std::size_t terrains = 0;
    const auto inside = [&](const bethconv::record::Vec3& p) {
        const int gx = static_cast<int>(std::floor(p.x / k_cell_units));
        const int gy = static_cast<int>(std::floor(p.y / k_cell_units));
        return std::abs(gx - cx) <= radius && std::abs(gy - cy) <= radius;
    };
    for (std::size_t i = 0; i < world.cell_count(); ++i) {
        const auto cell = world.cell_at(i);
        if (!cell || cell->interior() || !cell->grid) {
            continue;
        }
        if (cell->world == ws->id && cell->persistent) {
            std::ranges::copy_if(cell->refs, std::back_inserter(refs),
                                 [&](const auto& r) { return inside(r.position); });
            continue;
        }
        const auto [gx, gy] = *cell->grid;
        if (std::abs(gx - cx) > radius || std::abs(gy - cy) > radius) {
            continue;
        }
        if (cell->world == ws->id) {
            ++cells;
            refs.insert(refs.end(), cell->refs.begin(), cell->refs.end());
            if (cell->water != 0) {
                waters.insert(cell->water);
            }
        }
        if (cell->world == land_world && cell->terrain) {
            ++terrains;
            for (const auto& layer : cell->terrain->layers) {
                land_textures.insert(layer.texture);
            }
        }
    }

    if (models) {
        std::set<std::string> unique;
        for (const auto& ref : refs) {
            if (const auto base = world.base(ref.base); base && !base->model.empty()) {
                unique.insert(base->model);
            }
        }
        for (const auto id : land_textures) {
            if (const auto ltex = world.land_texture(id)) {
                for (const auto* path : {&ltex->diffuse, &ltex->normal}) {
                    if (!path->empty()) {
                        unique.insert(*path);
                    }
                }
            }
        }
        for (const auto id : waters) {
            if (const auto water = world.water(id)) {
                unique.insert(water->noise.begin(), water->noise.end());
            }
        }
        for (const auto& path : unique) {
            std::printf("%s\n", path.c_str());
        }
        return 0;
    }
    std::printf("0x%08X %s around (%d, %d) radius %d: %zu cells, %zu refs, %zu with terrain%s, "
                "%zu land textures, %zu water types\n",
                ws->id, ws->editor_id.c_str(), cx, cy, radius, cells, refs.size(), terrains,
                land_world != ws->id ? " (the parent's)" : "", land_textures.size(), waters.size());
    return 0;
}

/// Inspect cells in a pack's world.fb. With `--models`, print only the unique
/// model paths the cell's references use (input for `view --from`).
void print_scripts(const std::vector<bethconv::record::Script>& scripts) {
    using bethconv::record::ScriptPropertyType;
    for (const auto& script : scripts) {
        std::printf("    %s%s\n", script.name.c_str(), (script.status & 0x2u) != 0 ? " (removed)" : "");
        for (const auto& p : script.properties) {
            std::string value;
            for (const auto& o : p.objects) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), o.alias >= 0 ? " 0x%08X:%d" : " 0x%08X",
                              o.form.value, o.alias);
                value += buffer;
            }
            for (const auto& text : p.strings) {
                value += " \"" + text + "\"";
            }
            for (const auto i : p.integers) {
                value += " " + std::to_string(i);
            }
            for (const auto f : p.floats) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), " %g", static_cast<double>(f));
                value += buffer;
            }
            std::printf("      %s =%s\n", p.name.c_str(), value.c_str());
        }
    }
}

/// The LOD meshes of a worldspace and its tree atlas, as virtual paths the
/// pack has (input for view --from).
int cmd_cell_lod(const std::filesystem::path& pack, const std::string& world_name) {
    auto index = bethconv::pack::VpathIndex::read(pack / "vpath.idx");
    if (!index) {
        std::fprintf(stderr, "error: %s\n", index.error().to_string().c_str());
        return 1;
    }
    std::string name = world_name;
    std::ranges::transform(name, name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string meshes = "meshes/terrain/" + name + "/";
    const std::string atlas = "textures/terrain/" + name + "/trees/" + name + "treelod.dds";
    std::size_t count = 0;
    for (const auto& entry : index->entries()) {
        const bool mesh = entry.vpath.starts_with(meshes) &&
                          (entry.vpath.ends_with(".btr") || entry.vpath.ends_with(".bto"));
        if (mesh || entry.vpath == atlas) {
            std::printf("%s\n", entry.vpath.c_str());
            ++count;
        }
    }
    if (count == 0) {
        std::fprintf(stderr, "error: the pack has no LOD for %s\n", world_name.c_str());
        return 1;
    }
    return 0;
}

int cmd_cell(const std::filesystem::path& pack, const std::string& which, const std::string& filter,
             bool list, bool models, bool worlds, const std::string& world_name,
             const std::string& grid, int radius, bool lod, bool json) {
    using nlohmann::ordered_json;
    auto world = bethconv::pack::WorldFile::open(pack / "world.fb");
    if (!world) {
        std::fprintf(stderr, "error: %s\n", world.error().to_string().c_str());
        if (json) {
            bethconv::cli::emit(ordered_json{
                {"json_version", bethconv::cli::k_json_version},
                {"error", world.error().to_string()}});
        }
        return 1;
    }
    if (worlds && json) {
        auto spaces = ordered_json::array();
        for (const auto& w : world->worldspaces()) {
            spaces.push_back(ordered_json{
                {"id", w.id},
                {"editor_id", bethconv::io::json_text(w.editor_id)},
                {"parent", w.parent},
                {"uses_parent_land", w.parent != 0 && (w.parent_flags & 0x1u) != 0},
                {"bounds", {w.bounds[0], w.bounds[1], w.bounds[2], w.bounds[3]}}});
        }
        bethconv::cli::emit(ordered_json{{"json_version", bethconv::cli::k_json_version},
                                         {"worlds", std::move(spaces)}});
        return 0;
    }
    if ((list || which.empty()) && world_name.empty() && json) {
        auto cells = ordered_json::array();
        for (std::size_t i = 0; i < world->cell_count(); ++i) {
            const auto cell = world->cell_at(i);
            if (!cell || cell->editor_id.empty()) {
                continue;
            }
            cells.push_back(ordered_json{{"id", cell->id},
                                         {"editor_id", bethconv::io::json_text(cell->editor_id)},
                                         {"interior", cell->interior()},
                                         {"refs", cell->refs.size()}});
        }
        bethconv::cli::emit(ordered_json{{"json_version", bethconv::cli::k_json_version},
                                         {"total", world->cell_count()},
                                         {"cells", std::move(cells)}});
        return 0;
    }
    if (worlds) {
        for (const auto& w : world->worldspaces()) {
            std::printf("0x%08X  %-28s parent 0x%08X%s  bounds (%.0f %.0f)-(%.0f %.0f)\n", w.id,
                        w.editor_id.c_str(), w.parent,
                        (w.parent != 0 && (w.parent_flags & 0x1u) != 0) ? " (its land)" : "",
                        static_cast<double>(w.bounds[0]), static_cast<double>(w.bounds[1]),
                        static_cast<double>(w.bounds[2]), static_cast<double>(w.bounds[3]));
        }
        return 0;
    }
    if (lod) {
        if (world_name.empty()) {
            std::fprintf(stderr, "error: --lod needs --world\n");
            return 1;
        }
        return cmd_cell_lod(pack, world_name);
    }
    if (!world_name.empty()) {
        return cmd_cell_region(*world, world_name, grid, radius, models);
    }
    if (list || which.empty()) {
        std::size_t shown = 0;
        for (std::size_t i = 0; i < world->cell_count(); ++i) {
            const auto cell = world->cell_at(i);
            if (!cell || cell->editor_id.empty()) {
                continue;
            }
            std::string lower = cell->editor_id;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (!filter.empty() && lower.find(filter) == std::string::npos) {
                continue;
            }
            std::printf("0x%08X  %-9s %5zu refs  %s\n", cell->id,
                        cell->interior() ? "interior" : "exterior", cell->refs.size(),
                        cell->editor_id.c_str());
            ++shown;
        }
        std::printf("%zu of %zu cells\n", shown, world->cell_count());
        return 0;
    }

    std::optional<bethconv::pack::WorldCell> cell;
    if (which.starts_with("0x") || which.starts_with("0X")) {
        cell = world->cell(static_cast<std::uint32_t>(std::stoul(which, nullptr, 16)));
    } else {
        cell = world->cell_by_editor_id(which);
    }
    if (!cell) {
        std::fprintf(stderr, "error: no cell %s\n", which.c_str());
        return 1;
    }

    if (models) {
        std::set<std::string> unique;
        for (const auto& ref : cell->refs) {
            if (const auto base = world->base(ref.base); base && !base->model.empty()) {
                unique.insert(base->model);
            }
        }
        for (const auto& path : unique) {
            std::printf("%s\n", path.c_str());
        }
        return 0;
    }

    std::printf("0x%08X %s  %s  %zu refs, %zu doors\n", cell->id, cell->editor_id.c_str(),
                cell->interior() ? "interior" : "exterior", cell->refs.size(),
                cell->doors.size());
    if (cell->lighting) {
        std::printf("  lighting: ambient %08X directional %08X fog %.0f-%.0f\n",
                    cell->lighting->ambient, cell->lighting->directional,
                    static_cast<double>(cell->lighting->fog_near),
                    static_cast<double>(cell->lighting->fog_far));
    }
    std::size_t no_base = 0;
    for (const auto& ref : cell->refs) {
        const auto base = world->base(ref.base);
        if (!base) {
            ++no_base;
            continue;
        }
        std::printf("  0x%08X %s %-28s pos (%.0f %.0f %.0f) scale %.2f%s  %s\n", ref.id,
                    base->type.to_string().c_str(), base->editor_id.c_str(),
                    static_cast<double>(ref.position.x), static_cast<double>(ref.position.y),
                    static_cast<double>(ref.position.z), static_cast<double>(ref.scale),
                    (ref.flags & bethconv::pack::k_ref_initially_disabled) != 0 ? " disabled" : "",
                    base->light ? "(light)" : base->model.c_str());
    }
    if (no_base != 0) {
        std::printf("  %zu refs place a base with no model or light\n", no_base);
    }
    for (const auto& door : cell->doors) {
        std::printf("  door 0x%08X -> 0x%08X at (%.0f %.0f %.0f)\n", door.ref, door.destination,
                    static_cast<double>(door.position.x), static_cast<double>(door.position.y),
                    static_cast<double>(door.position.z));
    }
    for (const auto& lock : cell->locks) {
        std::printf("  lock 0x%08X level %u key 0x%08X\n", lock.ref, lock.level, lock.key);
    }
    for (const auto& link : cell->links) {
        std::printf("  link 0x%08X -> 0x%08X keyword 0x%08X\n", link.ref, link.target,
                    link.keyword);
    }
    for (const auto& parent : cell->activate_parents) {
        std::printf("  activate parent 0x%08X of 0x%08X delay %.2f\n", parent.parent, parent.ref,
                    static_cast<double>(parent.delay));
    }
    std::set<std::uint32_t> scripted_bases;
    for (const auto& ref : cell->refs) {
        if (const auto base = world->base(ref.base); base && !base->scripts.empty()) {
            scripted_bases.insert(base->id);
        }
    }
    for (const auto id : scripted_bases) {
        std::printf("  base 0x%08X %s:\n", id, world->base(id)->editor_id.c_str());
        print_scripts(world->base(id)->scripts);
    }
    for (const auto& ref : cell->scripts) {
        std::printf("  ref 0x%08X:\n", ref.ref);
        print_scripts(ref.scripts);
    }
    return 0;
}

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

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"bethconv - ahead-of-time Bethesda data converter"};

    // Commands that write many files refuse slow targets without this flag.
    bool allow_slow_target = false;
    const char* k_allow_slow_help =
        "Write many small files even to a FUSE filesystem or a spinning disk";
    app.set_version_flag("--version", std::string(BETHCONV_VERSION));
    std::vector<std::filesystem::path> texture_sources;
    std::vector<std::string> texture_vpaths;
    std::filesystem::path texture_list;
    std::string texture_filter;
    std::filesystem::path texture_out;
    bool texture_inspect = false;
    bool texture_no_fix = false;
    std::size_t texture_limit = 0;
    bool texture_verbose = false;
    bool texture_quiet = false;
    std::vector<std::filesystem::path> script_sources;
    std::vector<std::string> script_vpaths;
    std::string script_filter;
    bool script_dump = false;
    auto* script_cmd = app.add_subcommand("script", "Decode compiled Papyrus scripts");
    script_cmd->add_option("--source", script_sources, "Archive or directory to mount (repeatable)")
        ->required();
    script_cmd->add_option("vpath", script_vpaths, "Scripts to decode; default every .pex");
    script_cmd->add_option("--filter", script_filter, "Only paths containing this substring");
    script_cmd->add_flag("--dump", script_dump, "Print each script's disassembly");

    auto* texture = app.add_subcommand("texture", "Pass DDS textures through, completing the mip chain");
    texture->add_option("--source", texture_sources,
                        "BSA/BA2, loose directory, or .dds file; repeat, in load order")
        ->required()
        ->check(CLI::ExistingPath);
    texture->add_option("vpath", texture_vpaths,
                        "Virtual paths to convert; omit to sweep every .dds in the set");
    texture->add_option("--from", texture_list, "Read virtual paths from this file, one per line")
        ->check(CLI::ExistingFile);
    texture->add_option("--filter", texture_filter, "Only sweep paths containing this substring");
    texture->add_option("-o,--out", texture_out, "Write .dds files under this directory");
    texture->add_flag("--allow-slow-target", allow_slow_target, k_allow_slow_help);
    texture->add_flag("--inspect", texture_inspect, "Census only; write nothing");
    texture->add_flag("--no-fix", texture_no_fix,
                      "Copy verbatim; do not complete short mip chains");
    texture->add_option("--limit", texture_limit, "Stop after this many files")->default_val(0);
    texture->add_flag("-v,--verbose", texture_verbose, "Print a line per texture");
    texture->add_flag("-q,--quiet", texture_quiet, "Summary and census only, no per-file lines");

    std::filesystem::path lo_data;
    std::filesystem::path lo_list;
    bool lo_include_inactive = false;
    bool lo_no_implicit = false;
    bool lo_verify = false;
    bool lo_verbose = false;
    auto* loadorder =
        app.add_subcommand("loadorder", "Resolve a plugin list into an indexed load order");
    loadorder->add_option("--data", lo_data, "The game's Data folder")
        ->required()
        ->check(CLI::ExistingDirectory);
    loadorder->add_option("--list", lo_list, "plugins.txt or loadorder.txt")
        ->check(CLI::ExistingFile);
    loadorder->add_flag("--include-inactive", lo_include_inactive,
                        "Keep plugins a plugins.txt leaves unmarked");
    loadorder->add_flag("--no-implicit", lo_no_implicit,
                        "Do not prepend Skyrim.esm and the DLC when the list omits them");
    loadorder->add_flag("--verify", lo_verify,
                        "Walk every plugin and remap every record FormID");
    loadorder->add_flag("-v,--verbose", lo_verbose, "One line per plugin while verifying");

    app.require_subcommand(1);

    std::vector<std::filesystem::path> probe_paths;
    auto* probe = app.add_subcommand("probe", "Map files and print size and leading tag");
    probe->add_option("files", probe_paths, "Files to inspect")
        ->required()
        ->check(CLI::ExistingFile);

    std::vector<std::filesystem::path> record_paths;
    bool stats = false;
    std::size_t top_fields = 0;
    std::vector<std::string> size_types;
    auto* records = app.add_subcommand("records", "Parse plugins and report their contents");
    records->add_option("plugins", record_paths, "ESM/ESP/ESL files, in load order")
        ->required()
        ->check(CLI::ExistingFile);
    records->add_flag("--stats", stats, "Print a per-record-type histogram");
    records->add_option("--top-fields", top_fields,
                        "List this many most-common field types per record type")
        ->default_val(0);
    records->add_option("--field-sizes", size_types,
                        "Census the on-disk size of every field of these record types");

    std::vector<std::filesystem::path> form_paths;
    std::vector<std::filesystem::path> form_sources;
    std::string form_language{std::string(bethconv::record::k_default_language)};
    auto* forms = app.add_subcommand(
        "forms", "Parse the record types that have field definitions and report coverage");
    forms->add_option("plugins", form_paths, "ESM/ESP/ESL files")
        ->required()
        ->check(CLI::ExistingFile);
    forms->add_option("--source", form_sources,
                      "BSA/BA2 or loose Data dir holding strings/; repeat, in load order")
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    forms->add_option("--language", form_language, "Which .STRINGS language to resolve against")
        ->default_val(std::string(bethconv::record::k_default_language));

    std::vector<std::filesystem::path> strings_sources;
    std::vector<std::string> strings_plugins;
    std::string strings_language{std::string(bethconv::record::k_default_language)};
    std::size_t strings_list = 0;
    std::vector<std::string> strings_lookups;
    auto* strings = app.add_subcommand("strings", "Read a plugin's .STRINGS tables");
    strings->add_option("--source", strings_sources,
                        "BSA/BA2 or loose Data dir; repeat, in load order")
        ->required()
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    strings->add_option("plugin", strings_plugins, "Plugin filenames, e.g. Skyrim.esm")
        ->required();
    strings->add_option("--language", strings_language, "Language to load")
        ->default_val(std::string(bethconv::record::k_default_language));
    strings->add_option("--list", strings_list, "Show this many resolving ids")->default_val(0);
    strings->add_option("--id", strings_lookups, "Look up these string indices");

    std::filesystem::path merge_data;
    std::filesystem::path merge_list;
    std::vector<std::filesystem::path> merge_sources;
    std::string merge_language{std::string(bethconv::record::k_default_language)};
    std::vector<std::string> merge_types;
    std::size_t merge_sample = 0;
    std::vector<std::string> merge_lookups;
    bool merge_no_strings = false;
    bool merge_no_second_pass = false;
    auto* merge = app.add_subcommand("merge", "Collapse a load order into one flat world");
    merge->add_option("--data", merge_data, "The game's Data folder")
        ->required()
        ->check(CLI::ExistingDirectory);
    merge->add_option("--list", merge_list, "plugins.txt or loadorder.txt")
        ->check(CLI::ExistingFile);
    merge->add_option("--source", merge_sources,
                      "Where to read strings/ from; default is every archive in --data")
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    merge->add_option("--language", merge_language, "Which .STRINGS language to resolve")
        ->default_val(std::string(bethconv::record::k_default_language));
    merge->add_option("--types", merge_types, "Only merge these record types, e.g. CELL REFR");
    merge->add_option("--sample", merge_sample, "Show this many winning records per type")
        ->default_val(0);
    merge->add_option("--find", merge_lookups, "Print the provenance of these global FormIDs");
    merge->add_flag("--no-strings", merge_no_strings, "Leave localized names as indices");
    merge->add_flag("--no-second-pass", merge_no_second_pass,
                    "Index only; skip re-reading the winning records");

    std::filesystem::path snap_data;
    std::filesystem::path snap_list;
    std::vector<std::filesystem::path> snap_sources;
    std::string snap_language{std::string(bethconv::record::k_default_language)};
    std::filesystem::path snap_out{"records.fb"};
    bool snap_no_strings = false;
    auto* snapshot =
        app.add_subcommand("snapshot", "Write the merged world to a records.fb snapshot");
    snapshot->add_option("--data", snap_data, "The game's Data folder")
        ->required()
        ->check(CLI::ExistingDirectory);
    snapshot->add_option("--list", snap_list, "plugins.txt or loadorder.txt")
        ->check(CLI::ExistingFile);
    snapshot->add_option("--source", snap_sources,
                         "Where to read strings/ from; default is every archive in --data")
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    snapshot->add_option("--language", snap_language, "Which .STRINGS language to resolve")
        ->default_val(std::string(bethconv::record::k_default_language));
    snapshot->add_option("-o,--out", snap_out, "Where to write the snapshot")
        ->default_val("records.fb");
    snapshot->add_flag("--no-strings", snap_no_strings, "Leave localized names as indices");


    std::filesystem::path convert_data;
    std::filesystem::path convert_list;
    std::vector<std::filesystem::path> convert_sources;
    std::filesystem::path convert_out{"pack"};
    std::string convert_language{std::string(bethconv::record::k_default_language)};
    std::string convert_filter;
    std::size_t convert_limit = 0;
    bool convert_no_records = false;
    bool convert_no_meshes = false;
    bool convert_no_textures = false;
    bool convert_no_scripts = false;
    bool convert_no_lod = false;
    bool convert_no_mip_fix = false;
    bool convert_no_collision = false;
    bool convert_no_skinning = false;
    bool convert_keep_z_up = false;
    float convert_unit_scale = 0.0142875f;
    bool convert_hash_archives = false;
    bool convert_prune = false;
    std::string convert_store = "blob";
    bool convert_quiet = false;
    bool convert_json = false;
    std::uint32_t convert_max_texture = 0;
    std::filesystem::path convert_mo2;
    std::string convert_profile;
    auto* convert = app.add_subcommand("convert", "Convert an install into a pack");
    convert->add_option("--data", convert_data, "The game's Data folder")
        ->required()
        ->check(CLI::ExistingDirectory);
    convert->add_option("--list", convert_list, "plugins.txt or loadorder.txt")
        ->check(CLI::ExistingFile);
    convert->add_option("--source", convert_sources,
                        "What to mount; default is every archive in --data plus loose files")
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    convert->add_option("-o,--out", convert_out, "Pack directory to write")
        ->default_val("pack");
    convert->add_option("--language", convert_language, "Which .STRINGS language to resolve")
        ->default_val(std::string(bethconv::record::k_default_language));
    convert->add_option("--filter", convert_filter,
                        "Only convert virtual paths containing this substring");
    convert->add_option("--limit", convert_limit, "Stop after this many inputs")
        ->default_val(0);
    convert->add_flag("--no-records", convert_no_records, "Skip the merge and records.fb");
    convert->add_flag("--no-meshes", convert_no_meshes, "Skip the NIF pass");
    convert->add_flag("--no-textures", convert_no_textures, "Skip the DDS pass");
    convert->add_flag("--no-scripts", convert_no_scripts, "Skip the PEX pass");
    convert->add_flag("--no-lod", convert_no_lod, "Skip terrain, object and tree LOD");
    convert->add_flag("--no-mip-fix", convert_no_mip_fix,
                      "Leave short DDS mip chains alone (control run)");
    convert->add_flag("--no-collision", convert_no_collision, "Do not read Havok shapes");
    convert->add_flag("--no-skinning", convert_no_skinning, "Do not read skin data");
    convert->add_flag("--keep-z-up", convert_keep_z_up, "Leave meshes in NIF space");
    convert->add_option("--unit-scale", convert_unit_scale, "Metres per game unit")
        ->default_val(0.0142875f);
    convert->add_flag("--hash-sources", convert_hash_archives,
                      "Hash every mounted archive into the manifest, not only the plugins");
    convert->add_flag("--prune", convert_prune,
                      "Remove assets this run's index does not name (compacts a blob)");
    convert->add_option("--store", convert_store,
                        "blob: one index and one blob file (default); loose: one file per asset")
        ->check(CLI::IsMember({"blob", "loose"}));
    convert->add_flag("--allow-slow-target", allow_slow_target, k_allow_slow_help);
    convert->add_flag("-q,--quiet", convert_quiet, "No progress line");
    convert->add_option("--mo2", convert_mo2,
                        "A Mod Organizer 2 instance folder: mount its profile's mods over --data")
        ->check(CLI::ExistingDirectory)
        ->excludes("--source");
    convert->add_option("--profile", convert_profile,
                        "With --mo2: the profile (default: the one MO2 has selected)")
        ->needs("--mo2");
    convert->add_option("--max-texture-size", convert_max_texture,
                        "Largest texture side in pixels; larger textures lose their top mip "
                        "levels (0: full size)")
        ->check(CLI::NonNegativeNumber);
    convert->add_flag("--json", convert_json,
                      "Progress and result as JSON lines on stdout; text goes to stderr");

    bool front_json = false;
    auto* detect = app.add_subcommand("detect", "Find Skyrim installs and their plugins.txt");
    detect->add_flag("--json", front_json, "One JSON document on stdout");

    std::filesystem::path mo2_dir;
    std::string mo2_profile;
    auto* mo2 = app.add_subcommand("mo2", "Read a Mod Organizer 2 instance and one profile");
    mo2->add_option("instance", mo2_dir, "The instance folder (holds ModOrganizer.ini)")
        ->required()
        ->check(CLI::ExistingDirectory);
    mo2->add_option("--profile", mo2_profile, "Profile to read (default: the selected one)");
    mo2->add_flag("--json", front_json, "One JSON document on stdout");

    std::filesystem::path target_dir;
    auto* target = app.add_subcommand(
        "target", "Check a folder for a pack: storage, free space, whether it is a pack");
    target->add_option("dir", target_dir, "The folder (need not exist)")->required();
    target->add_flag("--json", front_json, "One JSON document on stdout");

    std::filesystem::path info_pack;
    auto* info = app.add_subcommand("info", "Summarize a pack: inputs, size, stale blob bytes");
    info->add_option("pack", info_pack, "The pack directory")
        ->required()
        ->check(CLI::ExistingDirectory);
    info->add_flag("--json", front_json, "One JSON document on stdout");

    std::filesystem::path view_pack;
    std::filesystem::path view_out;
    std::string view_filter;
    std::size_t view_limit = 0;
    bool view_copy = false;
    bool view_all = false;
    bool view_quiet = false;
    std::filesystem::path cell_pack;
    std::string cell_which;
    std::string cell_filter;
    bool cell_list = false;
    bool cell_models = false;
    bool cell_worlds = false;
    std::string cell_world;
    std::string cell_grid;
    int cell_radius = 0;
    bool cell_lod = false;
    bool cell_json = false;
    auto* cell = app.add_subcommand("cell", "Inspect cells in a pack's world.fb");
    cell->add_option("pack", cell_pack, "The pack directory")
        ->required()
        ->check(CLI::ExistingDirectory);
    cell->add_option("cell", cell_which, "Editor id or 0x FormID; omit to list cells");
    cell->add_option("--filter", cell_filter, "With --list: only editor ids containing this (lowercase)");
    cell->add_flag("--list", cell_list, "List cells with an editor id");
    cell->add_flag("--models", cell_models,
                   "Print unique model paths only (with --world, also land textures)");
    cell->add_flag("--worlds", cell_worlds, "List worldspaces");
    cell->add_option("--world", cell_world, "Worldspace editor id: select an exterior region");
    cell->add_option("--grid", cell_grid, "With --world: centre cell as X,Y (default 0,0)");
    cell->add_option("--radius", cell_radius, "With --world: cells around the centre (default 0)");
    cell->add_flag("--json", cell_json, "With --worlds or --list: one JSON document on stdout");
    cell->add_flag("--lod", cell_lod,
                   "With --world: print its LOD meshes and tree atlas (input for view --from)");

    std::filesystem::path view_list;
    auto* view = app.add_subcommand(
        "view", "Resolve a pack's vpath.idx into a directory a glTF consumer can open");
    view->add_option("pack", view_pack, "The pack directory to resolve")
        ->required()
        ->check(CLI::ExistingDirectory);
    view->add_option("-o,--out", view_out, "Directory to materialize the view in")
        ->default_val("view");
    view->add_option("--filter", view_filter,
                     "Only materialize virtual paths containing this substring");
    view->add_option("--limit", view_limit, "Stop after this many index entries")
        ->default_val(0);
    view->add_option("--from", view_list,
                     "Only materialize the virtual paths in this file (one per line), plus "
                     "the textures their meshes use")
        ->check(CLI::ExistingFile);
    view->add_flag("--copy", view_copy,
                   "Copy asset bytes instead of hard-linking them into a loose pack");
    view->add_flag("--all", view_all, "Materialize the whole pack");
    view->add_flag("--allow-slow-target", allow_slow_target, k_allow_slow_help);
    view->add_flag("-q,--quiet", view_quiet, "No progress line");

    std::filesystem::path verify_path;
    std::size_t verify_cells = 20;
    std::filesystem::path verify_against;
    std::filesystem::path verify_against_list;
    std::vector<std::string> verify_lookups;
    bool verify_deep = false;
    bool verify_verbose = false;
    auto* verify = app.add_subcommand("verify", "Re-read a records.fb and check what it claims");
    verify->add_option("snapshot", verify_path, "The records.fb to check")
        ->required()
        ->check(CLI::ExistingFile);
    verify->add_option("--cells", verify_cells, "How many cells to spot-check")->default_val(20);
    verify->add_option("--against", verify_against,
                       "Re-run the merge over this Data folder and compare every form")
        ->check(CLI::ExistingDirectory);
    verify->add_option("--list", verify_against_list, "plugins.txt to use with --against")
        ->check(CLI::ExistingFile);
    verify->add_option("--find", verify_lookups,
                       "Print what the snapshot says about these global FormIDs");
    verify->add_flag("--deep", verify_deep, "Re-hash the payload blob");
    verify->add_flag("-v,--verbose", verify_verbose, "Name every cell that is spot-checked");

    std::vector<std::filesystem::path> scan_paths;
    bool list_conflicts = false;
    std::size_t max_listed = 20;
    auto* scan = app.add_subcommand("scan", "Mount archives/directories and report contents");
    scan->add_option("sources", scan_paths,
                     "BSA/BA2 files and loose directories, in load order")
        ->required()
        ->check(CLI::ExistingPath);
    scan->add_flag("--conflicts", list_conflicts, "List paths provided by more than one source");
    scan->add_option("--max-listed", max_listed, "Cap on listed conflicts")->default_val(20);
    std::string list_filter;
    std::size_t list_count = 0;
    scan->add_option("--list", list_count, "List this many virtual paths")->default_val(0);
    scan->add_option("--filter", list_filter, "Only list paths containing this substring");
    bool read_all = false;
    scan->add_flag("--read-all", read_all,
                   "Read and decompress every entry; reports failures");

    std::vector<std::filesystem::path> extract_sources;
    std::vector<std::string> extract_vpaths;
    std::filesystem::path extract_list;
    std::filesystem::path extract_out;
    bool extract_quiet = false;
    auto* extract = app.add_subcommand("extract", "Read virtual paths out of the mounted set");
    // One value per occurrence, so it does not swallow the positional vpath.
    // Repeat --source per archive, in load order.
    extract->add_option("--source", extract_sources,
                        "BSA/BA2 file or loose directory; repeat, in load order")
        ->required()
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    extract->add_option("vpath", extract_vpaths, "Virtual paths, e.g. meshes/clutter/foo.nif");
    extract->add_option("--from", extract_list, "Read virtual paths from this file, one per line")
        ->check(CLI::ExistingFile);
    extract->add_option("-o,--out", extract_out,
                        "Write here: a file for one path, a directory root for several");
    extract->add_flag("--allow-slow-target", allow_slow_target, k_allow_slow_help);
    extract->add_flag("-q,--quiet", extract_quiet, "Summary only, no per-file line");

    std::vector<std::filesystem::path> mesh_sources;
    std::vector<std::string> mesh_vpaths;
    std::string mesh_filter;
    std::filesystem::path mesh_out;
    bool mesh_inspect = false;
    std::size_t mesh_limit = 0;
    bool mesh_no_collision = false;
    bool mesh_no_skinning = false;
    bool mesh_keep_z_up = false;
    float mesh_unit_scale = 0.0142875f;
    bool mesh_verbose = false;
    auto* mesh = app.add_subcommand("mesh", "Convert NIFs to glTF, or report what is in them");
    mesh->add_option("--source", mesh_sources,
                     "BSA/BA2, loose directory, or .nif file; repeat, in load order")
        ->required()
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    mesh->add_option("vpath", mesh_vpaths,
                     "Virtual paths to convert; omit to sweep every .nif in the set");
    mesh->add_option("--filter", mesh_filter, "Only sweep paths containing this substring");
    mesh->add_option("-o,--out", mesh_out, "Write .glb files under this directory");
    mesh->add_flag("--allow-slow-target", allow_slow_target, k_allow_slow_help);
    mesh->add_flag("--inspect", mesh_inspect, "Report contents only; write nothing");
    mesh->add_option("--limit", mesh_limit, "Stop after this many files")->default_val(0);
    mesh->add_flag("--no-collision", mesh_no_collision, "Skip bhkCollisionObject extraction");
    mesh->add_flag("--no-skinning", mesh_no_skinning, "Skip skin/joint extraction");
    mesh->add_flag("--keep-z-up", mesh_keep_z_up, "Leave the model in NIF axes");
    mesh->add_option("--unit-scale", mesh_unit_scale,
                     "Metres per game unit; 1 leaves game units alone")
        ->default_val(0.0142875f);
    mesh->add_flag("-v,--verbose", mesh_verbose, "Print per-file detail while converting");

    CLI11_PARSE(app, argc, argv);

    if (probe->parsed()) {
        return cmd_probe(probe_paths);
    }
    if (records->parsed()) {
        return cmd_records(record_paths, stats, top_fields, size_types);
    }
    if (forms->parsed()) {
        return cmd_forms(form_paths, form_sources, form_language);
    }
    if (merge->parsed()) {
        return cmd_merge(merge_data, merge_list, merge_sources, merge_language, merge_types,
                         merge_sample, merge_lookups, merge_no_strings, merge_no_second_pass);
    }
    if (snapshot->parsed()) {
        return cmd_snapshot(snap_data, snap_list, snap_sources, snap_language, snap_out,
                            snap_no_strings);
    }
    if (verify->parsed()) {
        return cmd_verify(verify_path, verify_cells, verify_against, verify_against_list,
                          verify_lookups, verify_deep, verify_verbose);
    }
    if (strings->parsed()) {
        return cmd_strings(strings_sources, strings_plugins, strings_language, strings_list,
                           strings_lookups);
    }
    if (scan->parsed()) {
        return cmd_scan(scan_paths, list_conflicts, max_listed, list_filter, list_count,
                        read_all);
    }
    if (extract->parsed()) {
        const bool many = !extract_list.empty() || extract_vpaths.size() > 1;
        if (!extract_out.empty() && !output_target_ok(extract_out, many, allow_slow_target)) {
            return 2;
        }
        return cmd_extract(extract_sources, extract_vpaths, extract_list,
                           extract_out, extract_quiet);
    }
    if (loadorder->parsed()) {
        return cmd_loadorder(lo_data, lo_list, lo_include_inactive, lo_no_implicit, lo_verify,
                             lo_verbose);
    }
    if (mesh->parsed()) {
        if (!mesh_out.empty() && !output_target_ok(mesh_out, true, allow_slow_target)) {
            return 2;
        }
        return cmd_mesh(mesh_sources, mesh_vpaths, mesh_filter, mesh_out, mesh_inspect,
                        mesh_limit, mesh_no_collision, mesh_no_skinning, mesh_keep_z_up,
                        mesh_unit_scale, mesh_verbose);
    }
    if (script_cmd->parsed()) {
        return cmd_script(script_sources, script_vpaths, script_filter, script_dump);
    }
    if (texture->parsed()) {
        if (!texture_out.empty() && !output_target_ok(texture_out, true, allow_slow_target)) {
            return 2;
        }
        return cmd_texture(texture_sources, texture_vpaths, texture_list, texture_filter,
                           texture_out, texture_inspect, texture_no_fix, texture_limit,
                           texture_verbose, texture_quiet);
    }
    if (convert->parsed()) {
        return cmd_convert(ConvertArgs{.data_dir = convert_data,
                                       .list_file = convert_list,
                                       .sources = convert_sources,
                                       .mo2 = convert_mo2,
                                       .mo2_profile = convert_profile,
                                       .out = convert_out,
                                       .language = convert_language,
                                       .filter = convert_filter,
                                       .limit = convert_limit,
                                       .no_records = convert_no_records,
                                       .no_meshes = convert_no_meshes,
                                       .no_textures = convert_no_textures,
                                       .no_scripts = convert_no_scripts,
                                       .no_lod = convert_no_lod,
                                       .no_mip_fix = convert_no_mip_fix,
                                       .no_collision = convert_no_collision,
                                       .no_skinning = convert_no_skinning,
                                       .keep_z_up = convert_keep_z_up,
                                       .unit_scale = convert_unit_scale,
                                       .max_texture_size = convert_max_texture,
                                       .hash_archives = convert_hash_archives,
                                       .prune = convert_prune,
                                       .layout = *bethconv::pack::layout_from_string(convert_store),
                                       .allow_slow_target = allow_slow_target,
                                       .quiet = convert_quiet,
                                       .json = convert_json});
    }
    if (detect->parsed()) {
        return bethconv::cli::cmd_detect(front_json);
    }
    if (mo2->parsed()) {
        return bethconv::cli::cmd_mo2(mo2_dir, mo2_profile, front_json);
    }
    if (target->parsed()) {
        return bethconv::cli::cmd_target(target_dir, front_json);
    }
    if (info->parsed()) {
        return bethconv::cli::cmd_info(info_pack, front_json);
    }
    if (cell->parsed()) {
        return cmd_cell(cell_pack, cell_which, cell_filter, cell_list, cell_models, cell_worlds,
                        cell_world, cell_grid, cell_radius, cell_lod, cell_json);
    }
    if (view->parsed()) {
        return cmd_view(view_pack, view_out, view_filter, view_limit, view_list, view_copy,
                        view_all, allow_slow_target, view_quiet);
    }
    return 0;
}
