// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/pack/inputs.hpp"
#include "bethconv/pack/snapshot.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/merge.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::cli {
namespace {

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
        options.strings = bethconv::pack::string_fetch(set);
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

struct VerifyArgs {
    std::filesystem::path path;
    std::size_t cells = 20;
    std::filesystem::path against;
    std::filesystem::path against_list;
    std::vector<std::string> lookups;
    bool deep = false;
    bool verbose = false;
};

} // namespace

void register_verify(CLI::App& app) {
    auto args = std::make_shared<VerifyArgs>();
    auto* verify = app.add_subcommand("verify", "Re-read a records.fb and check what it claims");
    verify->add_option("snapshot", args->path, "The records.fb to check")
        ->required()
        ->check(CLI::ExistingFile);
    verify->add_option("--cells", args->cells, "How many cells to spot-check")->default_val(20);
    verify->add_option("--against", args->against,
                       "Re-run the merge over this Data folder and compare every form")
        ->check(CLI::ExistingDirectory);
    verify->add_option("--list", args->against_list, "plugins.txt to use with --against")
        ->check(CLI::ExistingFile);
    verify->add_option("--find", args->lookups,
                       "Print what the snapshot says about these global FormIDs");
    verify->add_flag("--deep", args->deep, "Re-hash the payload blob");
    verify->add_flag("-v,--verbose", args->verbose, "Name every cell that is spot-checked");
    verify->callback([args] {
        set_exit_status(cmd_verify(args->path, args->cells, args->against, args->against_list,
                                   args->lookups, args->deep, args->verbose));
    });
}

} // namespace bethconv::cli
