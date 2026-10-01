// SPDX-License-Identifier: GPL-3.0-or-later
//
// Counting sink: record types, their volume and their fields. Behind
// `bethconv records --stats` and the corpus harness's per-type counts.
#pragma once

#include "bethconv/record/plugin.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace bethconv::record {

struct TypeStats {
    std::uint64_t count{};
    std::uint64_t payload_bytes{};
    std::uint64_t compressed{};
    std::uint64_t deleted{};
    /// Records whose fields tiled the payload exactly.
    std::uint64_t fields_ok{};
    /// Records whose field walk failed: overrun, gap, or a bad field header.
    std::uint64_t fields_bad{};
    /// field type -> how many times it appeared in a record of this type.
    std::map<std::uint32_t, std::uint64_t> fields;
    /// field type -> payload size -> count. Only for types passed to
    /// track_field_sizes().
    std::map<std::uint32_t, std::map<std::uint32_t, std::uint64_t>> field_sizes;
};

struct ErrorSample {
    io::ErrorKind kind{};
    std::string message;
};

/// Accumulates per-type counts over one or more plugins.
class Histogram final : public RecordSink {
public:
    /// Also record the size of every field of this record type, to check a
    /// definition against real data before writing it
    /// (`records --field-sizes REFR`).
    void track_field_sizes(io::FourCC type);

    void on_record(const RecordContext& ctx, io::SpanReader& data) override;
    void on_group_enter(const GroupContext& group) override;
    bool on_error(const io::ParseError& error) override;

    [[nodiscard]] const std::map<std::uint32_t, TypeStats>& types() const noexcept {
        return types_;
    }
    [[nodiscard]] const std::map<std::int32_t, std::uint64_t>& group_types() const noexcept {
        return group_types_;
    }
    [[nodiscard]] const std::vector<ErrorSample>& error_samples() const noexcept {
        return error_samples_;
    }
    [[nodiscard]] std::uint64_t total_records() const noexcept { return total_records_; }
    [[nodiscard]] std::uint64_t records_with_bad_fields() const noexcept {
        return records_with_bad_fields_;
    }

    /// Fraction of records whose fields tiled their payload exactly.
    [[nodiscard]] double field_coverage() const noexcept;

    /// Human-readable report. `top_fields` caps how many field types are listed
    /// per record type; 0 lists none.
    [[nodiscard]] std::string report(std::size_t top_fields = 0) const;

    /// The field-size census for the types named to track_field_sizes().
    [[nodiscard]] std::string size_report() const;

private:
    /// Errors are sampled; a systematically misparsed file would otherwise
    /// produce millions of similar lines.
    static constexpr std::size_t k_max_error_samples = 32;

    std::map<std::uint32_t, TypeStats> types_;
    std::set<std::uint32_t> sized_types_;
    std::map<std::int32_t, std::uint64_t> group_types_;
    std::vector<ErrorSample> error_samples_;
    std::uint64_t total_records_{};
    std::uint64_t records_with_bad_fields_{};
    std::uint64_t suppressed_errors_{};
};

} // namespace bethconv::record
