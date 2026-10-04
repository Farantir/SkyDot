// SPDX-License-Identifier: GPL-3.0-or-later
//
// Walks the load order, remaps every FormID, collapses overrides and produces
// one set of forms. The engine never sees plugins or mod indices.
//
// Rule: the last plugin in the order to write a form wins.
//
// Only record headers are needed, so the merge covers every record type
// regardless of field definitions.
//
// Two passes: pass one builds the index (~40 bytes per record, ~50 MB for SE);
// pass two (`for_each_record`) re-walks and passes each winner's bytes to a
// callback. Storing payloads instead would cost 250 MB for Skyrim.esm alone;
// the second walk takes about 12 s for SE.
#pragma once

#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/plugin.hpp"
#include "bethconv/record/strings.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace bethconv::record {

/// One form after applying the load order. No payload; see above.
struct MergedRecord {
    FormId form;    ///< Global id; the only one downstream code should use.
    FourCC type;
    FormId parent;  ///< Enclosing CELL/WRLD/DIAL/QUST, global. Null at top level.

    /// Load-order position of the winning plugin.
    std::uint32_t winner{};

    /// Load-order position of the plugin owning the FormID space (not always
    /// the first writer; see `injected`).
    std::uint32_t owner{};

    /// Number of earlier plugins that also wrote this form.
    std::uint32_t overrides{};

    /// The winning record's header flags.
    std::uint32_t flags{};

    /// The winner has the deleted flag. Kept so "deleted by a mod" and "never
    /// existed" stay distinguishable; consumers filter.
    bool deleted{};

    /// No plugin that wrote this form owns its FormID space (e.g. a mod adding
    /// to Skyrim.esm's space). Only known after the whole walk.
    bool injected{};
};

/// Merge statistics, each checkable against a real install.
struct MergeStats {
    std::uint64_t plugins{};    ///< Plugins actually opened and walked.
    std::uint64_t visited{};    ///< Records seen across all of them.
    std::uint64_t forms{};      ///< Distinct forms in the merged world.
    std::uint64_t collapsed{};  ///< Records superseded by a later plugin.
    std::uint64_t deleted{};    ///< Forms whose winner is flagged deleted.
    std::uint64_t injected{};   ///< Forms no plugin that wrote them owns.
    std::uint64_t unresolved{}; ///< Records whose FormID would not remap.
    std::uint64_t unparented{}; ///< Records whose parent group label would not remap.
    /// Records ignored because they wrote a known form under another type; the
    /// first writer's type and payload stand.
    std::uint64_t type_conflicts{};
    std::uint64_t errors{};     ///< Structural errors from the plugin walks.
    /// Plugins that failed to open during the merge. Normally 0, since
    /// LoadOrder::build already drops unreadable plugins; covers files vanishing
    /// in between.
    std::uint64_t unreadable{};

    /// String tables, if the merge had a StringFetch.
    std::uint64_t string_tables{};
    std::uint64_t strings_repaired{};
};

/// How a merge is run.
struct MergeOptions {
    /// Source of `strings/...` bytes, or empty to leave indices unresolved.
    StringFetch strings;
    std::string language{std::string(k_default_language)};

    /// Index only these record types (empty: all). Limits memory when only
    /// e.g. CELL/WRLD/REFR are needed.
    std::vector<FourCC> types;

    /// Keep walking a plugin after structural errors; a bad group costs only
    /// that group.
    bool continue_on_error = true;
};

/// One winning record, in the second pass.
class MergedRecordSink {
public:
    MergedRecordSink() = default;
    MergedRecordSink(const MergedRecordSink&) = delete;
    MergedRecordSink& operator=(const MergedRecordSink&) = delete;
    virtual ~MergedRecordSink() = default;

    /// `data` is the winning payload, inflated and bounded. `ctx` carries the
    /// winning plugin's localization and string tables, which are only
    /// available during the walk.
    virtual void on_record(const MergedRecord& merged, const RecordContext& ctx,
                           io::SpanReader& data, const FormContext& form_ctx) = 0;

    virtual bool on_error(const io::ParseError&) { return true; }

    /// Where to report unhandled and over-long fields; null for no census.
    [[nodiscard]] virtual FieldTally* tally() { return nullptr; }
};

/// The load order, applied.
class MergedWorld {
public:
    /// Walk `order` and collapse it. `order` must outlive the result; pass two
    /// reopens its plugins.
    [[nodiscard]] static MergedWorld build(const LoadOrder& order,
                                           const MergeOptions& options = {});

    /// In first-seen order (load order for forms nobody overrode).
    [[nodiscard]] std::span<const MergedRecord> records() const noexcept { return records_; }

    /// Null if no plugin defines this form.
    [[nodiscard]] const MergedRecord* find(FormId global) const noexcept;

    [[nodiscard]] const MergeStats& stats() const noexcept { return stats_; }

    /// Per-type totals after merging.
    [[nodiscard]] const std::map<std::uint32_t, std::uint64_t>& type_counts() const noexcept {
        return type_counts_;
    }

    /// Non-fatal problems, capped.
    [[nodiscard]] std::span<const std::string> problems() const noexcept { return problems_; }

    /// Re-walk the order and pass every winning record's bytes to `sink`.
    void for_each_record(MergedRecordSink& sink) const;

    [[nodiscard]] std::string report() const;

private:
    const LoadOrder* order_{};
    MergeOptions options_;

    std::vector<MergedRecord> records_;
    std::unordered_map<std::uint32_t, std::uint32_t> by_form_; ///< form -> records_ slot.
    std::map<std::uint32_t, std::uint64_t> type_counts_;
    std::vector<std::string> problems_;
    MergeStats stats_;

    /// One StringSource per plugin, parallel to the order. Loaded in pass one
    /// and reused in pass two.
    std::vector<StringSource> strings_;

    static constexpr std::size_t k_max_problems = 32;

    void note(std::string problem);
    [[nodiscard]] bool wants(FourCC type) const noexcept;
};

} // namespace bethconv::record
