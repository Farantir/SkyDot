// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs every field definition over real records and reports per type:
//
//   * parsed and failed counts,
//   * `leftover`: fields the definition did not fully read (a bug),
//   * `unhandled`: fields with no definition (the to-do list).
#pragma once

#include "bethconv/record/forms.hpp"
#include "bethconv/record/plugin.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bethconv::record {

struct FormTypeStats {
    std::uint64_t seen{};
    std::uint64_t parsed{};
    std::uint64_t failed{};
    /// Localized string indices in this type and how many were not found. Zero
    /// without a StringSource.
    std::uint64_t localized_strings{};
    std::uint64_t unresolved_strings{};
    /// field -> occurrences, for fields with no definition on this type.
    std::map<std::uint32_t, std::uint64_t> unhandled;
    /// field -> occurrences, for fields the definition did not fully read.
    std::map<std::uint32_t, std::uint64_t> leftover;
};

class FormCensus final : public RecordSink, public FieldTally {
public:
    /// From the plugin's TES4 header. Set per plugin before scanning it.
    void set_localized(bool localized) noexcept { localized_ = localized; }

    /// The plugin's string tables, or null. Set together with `localized`.
    void set_strings(const StringSource* strings) noexcept { strings_ = strings; }

    void on_record(const RecordContext& ctx, io::SpanReader& data) override;
    bool on_error(const io::ParseError& error) override;

    void unhandled(FourCC record, FourCC field, std::uint32_t size) override;
    void leftover(FourCC record, FourCC field, std::size_t bytes) override;
    void localized_string(FourCC record, FourCC field, std::uint32_t id,
                          bool resolved) override;

    [[nodiscard]] const std::map<std::uint32_t, FormTypeStats>& types() const noexcept {
        return types_;
    }
    [[nodiscard]] std::uint64_t total_parsed() const noexcept { return total_parsed_; }
    [[nodiscard]] std::uint64_t total_failed() const noexcept { return total_failed_; }

    /// Strings resolved from tables versus indices that resolved to nothing.
    [[nodiscard]] std::uint64_t total_resolved_strings() const noexcept {
        return total_resolved_strings_;
    }
    [[nodiscard]] std::uint64_t total_unresolved_strings() const noexcept {
        return total_unresolved_strings_;
    }

    [[nodiscard]] std::string report() const;

private:
    static constexpr std::size_t k_max_error_samples = 16;

    std::map<std::uint32_t, FormTypeStats> types_;
    std::vector<std::string> error_samples_;
    std::uint64_t total_parsed_{};
    std::uint64_t total_failed_{};
    std::uint64_t suppressed_errors_{};
    std::uint64_t total_resolved_strings_{};
    std::uint64_t total_unresolved_strings_{};
    const StringSource* strings_{};
    bool localized_{};
};

} // namespace bethconv::record
