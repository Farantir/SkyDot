// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/form_census.hpp"

#include "bethconv/record/forms_actor.hpp"
#include "bethconv/record/forms_game.hpp"
#include "bethconv/record/forms_object.hpp"
#include "bethconv/record/forms_world.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace bethconv::record {
namespace {

/// Run one definition and reduce the result to ok/error.
template <typename T>
io::ParseResult<void> discard(io::ParseResult<T>&& result) {
    if (!result) {
        return std::unexpected(std::move(result).error());
    }
    return {};
}

/// Erase a parser's return type so all fit in one table.
template <auto Parse>
io::ParseResult<void> run(io::SpanReader& data, const FormContext& ctx) {
    return discard(Parse(data, ctx));
}

/// Type tag -> definition. A tag in `defined_types()` without an entry here is
/// reported as a failure when first encountered.
struct Dispatch {
    FourCC type;
    io::ParseResult<void> (*run)(io::SpanReader&, const FormContext&);
};

constexpr Dispatch k_dispatch[] = {
    {FourCC{"STAT"}, &run<parse_static>},
    {FourCC{"DOOR"}, &run<parse_door>},
    {FourCC{"LIGH"}, &run<parse_light>},
    {FourCC{"CELL"}, &run<parse_cell>},
    {FourCC{"WRLD"}, &run<parse_worldspace>},
    {FourCC{"TXST"}, &run<parse_texture_set>},
    {FourCC{"ACTI"}, &run<parse_activator>},
    {FourCC{"CONT"}, &run<parse_container>},
    {FourCC{"MISC"}, &run<parse_misc_item>},
    {FourCC{"MSTT"}, &run<parse_movable_static>},
    {FourCC{"FURN"}, &run<parse_furniture>},
    {FourCC{"FLOR"}, &run<parse_flora>},
    {FourCC{"TREE"}, &run<parse_tree>},
    {FourCC{"KEYM"}, &run<parse_key>},
    {FourCC{"ALCH"}, &run<parse_ingestible>},
    {FourCC{"AMMO"}, &run<parse_ammo>},
    {FourCC{"WEAP"}, &run<parse_weapon>},
    {FourCC{"PROJ"}, &run<parse_projectile>},
    {FourCC{"IDLM"}, &run<parse_idle_marker>},
    {FourCC{"LVLN"}, &run<parse_leveled_npc>},
    {FourCC{"LTEX"}, &run<parse_land_texture>},
    {FourCC{"IMGS"}, &run<parse_image_space>},
    {FourCC{"VOLI"}, &run<parse_volumetric_lighting>},
    {FourCC{"CLMT"}, &run<parse_climate>},
    {FourCC{"WTHR"}, &run<parse_weather>},
    {FourCC{"REGN"}, &run<parse_region>},
    {FourCC{"SPGD"}, &run<parse_shader_particle_geometry>},
    {FourCC{"LCTN"}, &run<parse_location>},
    {FourCC{"LAND"}, &run<parse_landscape>},
    {FourCC{"NAVM"}, &run<parse_nav_mesh>},
    {FourCC{"NAVI"}, &run<parse_navigation_index>},
    {FourCC{"GMST"}, &run<parse_game_setting>},
    {FourCC{"GLOB"}, &run<parse_global>},
    {FourCC{"CLAS"}, &run<parse_actor_class>},
    {FourCC{"FACT"}, &run<parse_faction>},
    {FourCC{"ENCH"}, &run<parse_enchantment>},
    {FourCC{"SPEL"}, &run<parse_spell>},
    {FourCC{"NPC_"}, &run<parse_npc>},
    {FourCC{"QUST"}, &run<parse_quest>},
    {FourCC{"FLST"}, &run<parse_form_list>},
    {FourCC{"PACK"}, &run<parse_package>},
    {FourCC{"ARMO"}, &run<parse_armor>},
    {FourCC{"ARMA"}, &run<parse_armor_addon>},
    {FourCC{"OTFT"}, &run<parse_outfit>},
    {FourCC{"LVLI"}, &run<parse_leveled_item>},
    {FourCC{"RACE"}, &run<parse_race>},
};

} // namespace

void FormCensus::on_record(const RecordContext& ctx, io::SpanReader& data) {
    const auto type = ctx.header.type;
    if (!is_defined_type(type)) {
        return;
    }

    auto& stats = types_[type.value];
    ++stats.seen;

    const FormContext form_ctx{.localized = localized_, .strings = strings_, .tally = this};

    // REFR and ACHR also need the record header (header flags), so they are
    // not in the table.
    io::ParseResult<void> result;
    if (type == FourCC{"REFR"}) {
        result = discard(parse_reference(ctx.header, data, form_ctx));
    } else if (type == FourCC{"ACHR"}) {
        result = discard(parse_actor_reference(ctx.header, data, form_ctx));
    } else {
        const Dispatch* found = nullptr;
        for (const auto& entry : k_dispatch) {
            if (entry.type == type) {
                found = &entry;
                break;
            }
        }
        if (found == nullptr) {
            // Listed in `defined_types()` but missing here: fail loudly rather
            // than count it as parsed.
            result = data.fail(io::ErrorKind::bad_value,
                               type.to_string() + " is listed in defined_types() but has no "
                                                  "entry in the census dispatch table");
        } else {
            result = found->run(data, form_ctx);
        }
    }

    if (result) {
        ++stats.parsed;
        ++total_parsed_;
        return;
    }

    ++stats.failed;
    ++total_failed_;
    if (error_samples_.size() < k_max_error_samples) {
        error_samples_.push_back(type.to_string() + " " + ctx.header.form_id.to_string() +
                                 ": " + result.error().to_string());
    } else {
        ++suppressed_errors_;
    }
}

bool FormCensus::on_error(const io::ParseError&) {
    return true; // Structural errors are counted by the histogram.
}

void FormCensus::unhandled(FourCC record, FourCC field, std::uint32_t) {
    ++types_[record.value].unhandled[field.value];
}

void FormCensus::leftover(FourCC record, FourCC field, std::size_t) {
    ++types_[record.value].leftover[field.value];
}

void FormCensus::localized_string(FourCC record, FourCC, std::uint32_t, bool resolved) {
    auto& stats = types_[record.value];
    ++stats.localized_strings;
    if (resolved) {
        ++total_resolved_strings_;
    } else {
        ++stats.unresolved_strings;
        ++total_unresolved_strings_;
    }
}

namespace {

std::string top_fields(const std::map<std::uint32_t, std::uint64_t>& counts,
                       std::size_t limit) {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> rows(counts.begin(), counts.end());
    std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::ostringstream out;
    const auto shown = std::min(limit, rows.size());
    for (std::size_t i = 0; i < shown; ++i) {
        out << ' ' << io::FourCC{rows[i].first}.to_string() << '(' << rows[i].second << ')';
    }
    if (rows.size() > shown) {
        out << " +" << (rows.size() - shown) << " more";
    }
    return out.str();
}

} // namespace

std::string FormCensus::report() const {
    std::ostringstream out;
    out << "  TYPE          SEEN      PARSED    FAILED\n";
    out << "  ----  ------------  ----------  --------\n";
    for (const auto& [type, stats] : types_) {
        out << "  " << io::FourCC{type}.to_string();
        out.width(14);
        out << stats.seen;
        out.width(12);
        out << stats.parsed;
        out.width(10);
        out << stats.failed << "\n";
        if (!stats.leftover.empty()) {
            // The definition is too short, or the field grew.
            out << "        leftover:" << top_fields(stats.leftover, 12) << "\n";
        }
        if (!stats.unhandled.empty()) {
            out << "        no definition:" << top_fields(stats.unhandled, 16) << "\n";
        }
        if (stats.unresolved_strings != 0) {
            out << "        unresolved strings: " << stats.unresolved_strings << " of "
                << stats.localized_strings << "\n";
        }
    }

    out << "\nparsed " << total_parsed_ << " records, " << total_failed_ << " failed\n";
    if (total_resolved_strings_ + total_unresolved_strings_ != 0) {
        out << "localized strings: " << total_resolved_strings_ << " resolved, "
            << total_unresolved_strings_ << " unresolved\n";
    }
    if (!error_samples_.empty()) {
        out << "\nfailures (" << error_samples_.size();
        if (suppressed_errors_ > 0) {
            out << " shown, " << suppressed_errors_ << " more suppressed";
        }
        out << "):\n";
        for (const auto& sample : error_samples_) {
            out << "  " << sample << "\n";
        }
    }
    return out.str();
}

} // namespace bethconv::record
