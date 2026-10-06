// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/merge.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace bethconv::record {
namespace {

/// Group types whose label is a FormID (rather than a type tag, block number or
/// grid coordinate). Getting this wrong fails silently: a grid label read as a
/// FormID yields a plausible but wrong parent. Source: UESP, GRUP section.
[[nodiscard]] bool label_is_form(GroupType type) noexcept {
    switch (type) {
    case GroupType::world_children:
    case GroupType::cell_children:
    case GroupType::topic_children:
    case GroupType::cell_persistent_children:
    case GroupType::cell_temporary_children:
    case GroupType::quest_children:
        return true;
    case GroupType::top:
    case GroupType::interior_cell_block:
    case GroupType::interior_cell_sub_block:
    case GroupType::exterior_cell_block:
    case GroupType::exterior_cell_sub_block:
        return false;
    }
    return false;
}

/// The innermost enclosing group whose label is a record, plugin-local. For a
/// REFR under `WRLD > world_children > block > sub-block > cell_children >
/// temporary`, that is the CELL, not the worldspace.
[[nodiscard]] std::optional<FormId> local_parent(std::span<const GroupContext> groups) noexcept {
    for (auto it = groups.rbegin(); it != groups.rend(); ++it) {
        if (label_is_form(it->header.group_type)) {
            return it->header.label.as_form();
        }
    }
    return std::nullopt;
}

/// Pass one: index every wanted record and decide winners.
class IndexPass final : public RecordSink {
public:
    IndexPass(const LoadOrder& order, std::size_t plugin,
              const std::function<bool(FourCC)>& wants,
              std::vector<MergedRecord>& records,
              std::unordered_map<std::uint32_t, std::uint32_t>& by_form,
              MergeStats& stats, const std::function<void(std::string)>& note)
        : order_(order), plugin_(plugin), wants_(wants), records_(records),
          by_form_(by_form), stats_(stats), note_(note) {}

    void on_record(const RecordContext& ctx, io::SpanReader&) override {
        const auto type = ctx.header.type;
        if (!wants_(type)) {
            return;
        }
        ++stats_.visited;

        auto global = order_.resolve(plugin_, ctx.header.form_id);
        if (!global) {
            // Always hit once: vanilla Skyrim.esm's GMST 0x0123C00E. Costs only
            // that record.
            ++stats_.unresolved;
            note_(type.to_string() + " " + ctx.header.form_id.to_string() + " in " +
                  order_.entries()[plugin_].name + ": " + global.error().detail);
            return;
        }

        FormId parent;
        if (const auto local = local_parent(ctx.groups)) {
            if (auto resolved = order_.resolve(plugin_, *local)) {
                parent = *resolved;
            } else {
                ++stats_.unparented;
            }
        }

            // Owner of the FormID space; decides `injected` after the walk.
        std::uint32_t owner = static_cast<std::uint32_t>(plugin_);
        bool owns_it = true;
        if (auto found = order_.owner_of(plugin_, ctx.header.form_id)) {
            owner = static_cast<std::uint32_t>(*found);
            owns_it = *found == plugin_;
        }

        const auto slot = by_form_.find(global->value);
        if (slot == by_form_.end()) {
            MergedRecord record;
            record.form = *global;
            record.type = type;
            record.parent = parent;
            record.winner = static_cast<std::uint32_t>(plugin_);
            record.owner = owner;
            record.overrides = 0;
            record.flags = ctx.header.flags;
            record.deleted = ctx.header.is_deleted();
            // Provisional; cleared when the owning plugin writes it.
            record.injected = !owns_it;
            by_form_.emplace(global->value, static_cast<std::uint32_t>(records_.size()));
            records_.push_back(record);
            return;
        }

        MergedRecord& existing = records_[slot->second];
        if (existing.type != type) {
            // A type change is not applied: the first writer stays the winner,
            // so pass two never hands a payload to the wrong type's parser.
            ++stats_.type_conflicts;
            note_(global->to_string() + ": " + existing.type.to_string() + " in " +
                  order_.entries()[existing.winner].name + " overridden as " +
                  type.to_string() + " in " + order_.entries()[plugin_].name +
                  "; keeping the first");
            return;
        }

        // Walking in load order, so this later writer wins.
        ++existing.overrides;
        ++stats_.collapsed;
        existing.winner = static_cast<std::uint32_t>(plugin_);
        existing.flags = ctx.header.flags;
        existing.deleted = ctx.header.is_deleted();
        // The last writer also decides the parent.
        if (parent.value != 0) {
            existing.parent = parent;
        }
        if (owns_it) {
            existing.injected = false;
        }
    }

    bool on_error(const io::ParseError& error) override {
        ++stats_.errors;
        note_(order_.entries()[plugin_].name + ": " + error.to_string());
        return continue_on_error_;
    }

    void set_continue_on_error(bool value) noexcept { continue_on_error_ = value; }

private:
    const LoadOrder& order_;
    std::size_t plugin_;
    const std::function<bool(FourCC)>& wants_;
    std::vector<MergedRecord>& records_;
    std::unordered_map<std::uint32_t, std::uint32_t>& by_form_;
    MergeStats& stats_;
    const std::function<void(std::string)>& note_;
    bool continue_on_error_ = true;
};

/// Pass two: re-walk and forward only this plugin's winning records.
class WinnerPass final : public RecordSink {
public:
    WinnerPass(const MergedWorld& world, const LoadOrder& order, std::size_t plugin,
               const std::function<bool(FourCC)>& wants, MergedRecordSink& sink,
               FormContext form_ctx)
        : world_(world), order_(order), plugin_(plugin), wants_(wants), sink_(sink),
          form_ctx_(form_ctx) {}

    void on_record(const RecordContext& ctx, io::SpanReader& data) override {
        if (!wants_(ctx.header.type)) {
            return;
        }
        auto global = order_.resolve(plugin_, ctx.header.form_id);
        if (!global) {
            return; // Already counted in pass one.
        }
        const MergedRecord* merged = world_.find(*global);
        if (merged == nullptr) {
            return;
        }
        if (merged->winner != plugin_) {
            if (sink_.wants_superseded(ctx.header.type)) {
                MergedRecord version = *merged;
                version.winner = static_cast<std::uint32_t>(plugin_);
                version.flags = ctx.header.flags;
                version.deleted = ctx.header.is_deleted();
                sink_.on_superseded(version, ctx, data, form_ctx_);
            }
            return;
        }
        sink_.on_record(*merged, ctx, data, form_ctx_);
    }

    bool on_error(const io::ParseError& error) override { return sink_.on_error(error); }

private:
    const MergedWorld& world_;
    const LoadOrder& order_;
    std::size_t plugin_;
    const std::function<bool(FourCC)>& wants_;
    MergedRecordSink& sink_;
    FormContext form_ctx_;
};

} // namespace

bool MergedWorld::wants(FourCC type) const noexcept {
    if (options_.types.empty()) {
        return true;
    }
    return std::ranges::find(options_.types, type) != options_.types.end();
}

void MergedWorld::note(std::string problem) {
    if (problems_.size() < k_max_problems) {
        problems_.push_back(std::move(problem));
    }
}

const MergedRecord* MergedWorld::find(FormId global) const noexcept {
    const auto it = by_form_.find(global.value);
    if (it == by_form_.end()) {
        return nullptr;
    }
    return &records_[it->second];
}

MergedWorld MergedWorld::build(const LoadOrder& order, const MergeOptions& options) {
    MergedWorld world;
    world.order_ = &order;
    world.options_ = options;

    const std::function<bool(FourCC)> wants = [&world](FourCC type) {
        return world.wants(type);
    };
    const std::function<void(std::string)> note = [&world](std::string problem) {
        world.note(std::move(problem));
    };

    world.strings_.resize(order.entries().size());

    for (std::size_t i = 0; i < order.entries().size(); ++i) {
        const auto& entry = order.entries()[i];
        auto plugin = Plugin::open(entry.path);
        if (!plugin) {
            ++world.stats_.unreadable;
            world.note(entry.name + ": " + plugin.error().to_string());
            continue;
        }
        ++world.stats_.plugins;

        // This plugin's own tables, kept for pass two.
        if (world.options_.strings && plugin->header().is_localized()) {
            std::vector<io::ParseError> problems;
            world.strings_[i] = load_string_source(world.options_.strings, entry.name,
                                                   world.options_.language, &problems);
            for (const auto& problem : problems) {
                world.note(problem.to_string());
            }
            if (!world.strings_[i].empty()) {
                ++world.stats_.string_tables;
                world.stats_.strings_repaired += world.strings_[i].repaired();
            }
        }

        IndexPass pass(order, i, wants, world.records_, world.by_form_, world.stats_, note);
        pass.set_continue_on_error(options.continue_on_error);
        (void)plugin->scan(pass);
    }

    world.stats_.forms = world.records_.size();
    for (const auto& record : world.records_) {
        ++world.type_counts_[record.type.value];
        if (record.deleted) {
            ++world.stats_.deleted;
        }
        if (record.injected) {
            ++world.stats_.injected;
        }
    }
    return world;
}

void MergedWorld::for_each_record(MergedRecordSink& sink) const {
    if (order_ == nullptr) {
        return;
    }
    const std::function<bool(FourCC)> wants = [this](FourCC type) { return this->wants(type); };

    for (std::size_t i = 0; i < order_->entries().size(); ++i) {
        const auto& entry = order_->entries()[i];
        auto plugin = Plugin::open(entry.path);
        if (!plugin) {
            continue; // Already counted in pass one.
        }
        const StringSource& strings = strings_[i];
        const FormContext form_ctx{
            .localized = plugin->header().is_localized(),
            .strings = strings.empty() ? nullptr : &strings,
            .tally = sink.tally(),
        };
        WinnerPass pass(*this, *order_, i, wants, sink, form_ctx);
        (void)plugin->scan(pass);
    }
}

std::string MergedWorld::report() const {
    std::ostringstream out;
    out << stats_.plugins << " plugins walked";
    if (stats_.unreadable != 0) {
        out << " (" << stats_.unreadable << " would not open)";
    }
    out << "\n";
    out << stats_.visited << " records visited -> " << stats_.forms << " forms, "
        << stats_.collapsed << " overrides collapsed\n";
    out << stats_.deleted << " deleted, " << stats_.injected << " injected, "
        << stats_.unresolved << " unresolved, " << stats_.unparented << " unparented, "
        << stats_.errors << " structural errors\n";
    if (stats_.type_conflicts != 0) {
        out << stats_.type_conflicts << " records ignored for changing a form's type\n";
    }
    if (stats_.string_tables != 0) {
        out << stats_.string_tables << " plugins with string tables, "
            << stats_.strings_repaired << " entries repaired\n";
    }

    if (!type_counts_.empty()) {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> rows(type_counts_.begin(),
                                                                  type_counts_.end());
        std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.second > b.second; });
        out << "\n  TYPE         FORMS\n  ----  ------------\n";
        const auto shown = std::min<std::size_t>(rows.size(), 24);
        for (std::size_t i = 0; i < shown; ++i) {
            out << "  " << io::FourCC{rows[i].first}.to_string();
            out.width(14);
            out << rows[i].second << "\n";
        }
        if (rows.size() > shown) {
            out << "  ... and " << (rows.size() - shown) << " more types\n";
        }
    }

    if (!problems_.empty()) {
        out << "\nproblems (" << problems_.size();
        if (problems_.size() == k_max_problems) {
            out << ", capped";
        }
        out << "):\n";
        for (const auto& problem : problems_) {
            out << "  " << problem << "\n";
        }
    }
    return out.str();
}

} // namespace bethconv::record
