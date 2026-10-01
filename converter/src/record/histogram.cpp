// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/histogram.hpp"

#include "bethconv/record/field_walk.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

namespace bethconv::record {

void Histogram::track_field_sizes(io::FourCC type) { sized_types_.insert(type.value); }

void Histogram::on_record(const RecordContext& ctx, io::SpanReader& data) {
    ++total_records_;

    auto& stats = types_[ctx.header.type.value];
    ++stats.count;
    stats.payload_bytes += data.size();
    if (ctx.header.is_compressed()) {
        ++stats.compressed;
    }
    if (ctx.header.is_deleted()) {
        ++stats.deleted;
    }

    const bool sized = sized_types_.contains(ctx.header.type.value);
    const auto walk = for_each_field(data, [&](const FieldHeader& field, io::SpanReader& body) {
        ++stats.fields[field.type.value];
        if (sized) {
            ++stats.field_sizes[field.type.value][static_cast<std::uint32_t>(body.size())];
        }
    });

    if (walk) {
        ++stats.fields_ok;
    } else {
        ++stats.fields_bad;
        ++records_with_bad_fields_;
        if (error_samples_.size() < k_max_error_samples) {
            error_samples_.push_back(ErrorSample{
                .kind = walk.error().kind,
                .message = ctx.header.type.to_string() + " " +
                           ctx.header.form_id.to_string() + ": " +
                           walk.error().to_string(),
            });
        } else {
            ++suppressed_errors_;
        }
    }
}

void Histogram::on_group_enter(const GroupContext& group) {
    ++group_types_[static_cast<std::int32_t>(group.header.group_type)];
}

bool Histogram::on_error(const io::ParseError& error) {
    if (error_samples_.size() < k_max_error_samples) {
        error_samples_.push_back(ErrorSample{.kind = error.kind,
                                             .message = "structure: " + error.to_string()});
    } else {
        ++suppressed_errors_;
    }
    return true; // keep going
}

double Histogram::field_coverage() const noexcept {
    if (total_records_ == 0) {
        return 1.0;
    }
    const auto good = total_records_ - records_with_bad_fields_;
    return static_cast<double>(good) / static_cast<double>(total_records_);
}

namespace {

std::string percent(std::uint64_t part, std::uint64_t whole) {
    if (whole == 0) {
        return "  0.00%";
    }
    const double pct = 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    std::ostringstream out;
    out.precision(2);
    out << std::fixed;
    out.width(6);
    out << pct << '%';
    return out.str();
}

} // namespace

std::string Histogram::report(std::size_t top_fields) const {
    std::ostringstream out;

    struct Row {
        std::uint32_t type;
        const TypeStats* stats;
    };
    std::vector<Row> rows;
    rows.reserve(types_.size());
    for (const auto& [type, stats] : types_) {
        rows.push_back(Row{type, &stats});
    }
    std::ranges::sort(rows, [](const Row& a, const Row& b) {
        return a.stats->count > b.stats->count;
    });

    out << "record types: " << types_.size() << "   instances: " << total_records_ << "\n\n";
    out << "  TYPE     INSTANCES    PAYLOAD MiB   COMPRESSED    DELETED   FIELDS-OK\n";
    out << "  ----  ------------  -------------  -----------  ---------  ----------\n";
    for (const auto& row : rows) {
        const auto& s = *row.stats;
        out << "  " << io::FourCC{row.type}.to_string();
        out.width(14);
        out << s.count;
        out.width(15);
        out.precision(2);
        out << std::fixed << static_cast<double>(s.payload_bytes) / (1024.0 * 1024.0);
        out.width(13);
        out << s.compressed;
        out.width(11);
        out << s.deleted;
        out << "  " << percent(s.fields_ok, s.count) << "\n";

        if (top_fields > 0 && !s.fields.empty()) {
            std::vector<std::pair<std::uint32_t, std::uint64_t>> fields(s.fields.begin(),
                                                                       s.fields.end());
            std::ranges::sort(fields, [](const auto& a, const auto& b) {
                return a.second > b.second;
            });
            out << "        fields:";
            const auto shown = std::min(top_fields, fields.size());
            for (std::size_t i = 0; i < shown; ++i) {
                out << ' ' << io::FourCC{fields[i].first}.to_string() << '('
                    << fields[i].second << ')';
            }
            if (fields.size() > shown) {
                out << " +" << (fields.size() - shown) << " more";
            }
            out << "\n";
        }
    }

    out << "\ngroup types:\n";
    for (const auto& [type, count] : group_types_) {
        out << "  " << to_string(static_cast<GroupType>(type)) << ": " << count << "\n";
    }

    out << "\nfield-level structural coverage: " << percent(
        total_records_ - records_with_bad_fields_, total_records_)
        << " of " << total_records_ << " records\n";

    if (!error_samples_.empty()) {
        out << "\nerrors (" << error_samples_.size();
        if (suppressed_errors_ > 0) {
            out << " shown, " << suppressed_errors_ << " more suppressed";
        }
        out << "):\n";
        for (const auto& sample : error_samples_) {
            out << "  [" << io::to_string(sample.kind) << "] " << sample.message << "\n";
        }
    }

    return out.str();
}

std::string Histogram::size_report() const {
    std::ostringstream out;
    for (const auto type : sized_types_) {
        const auto it = types_.find(type);
        out << io::FourCC{type}.to_string() << ": ";
        if (it == types_.end()) {
            out << "not present\n";
            continue;
        }
        out << it->second.count << " records\n";
        for (const auto& [field, sizes] : it->second.field_sizes) {
            out << "  " << io::FourCC{field}.to_string() << " ";
            // Most common size first. One size means a fixed layout; many mean
            // an array or a string.
            std::vector<std::pair<std::uint32_t, std::uint64_t>> rows(sizes.begin(),
                                                                     sizes.end());
            std::ranges::sort(rows, [](const auto& a, const auto& b) {
                return a.second > b.second;
            });
            const auto shown = std::min<std::size_t>(8, rows.size());
            for (std::size_t i = 0; i < shown; ++i) {
                out << ' ' << rows[i].first << 'B' << 'x' << rows[i].second;
            }
            if (rows.size() > shown) {
                out << " +" << (rows.size() - shown) << " sizes";
            }
            out << "\n";
        }
    }
    return out.str();
}

} // namespace bethconv::record
