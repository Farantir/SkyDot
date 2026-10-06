// SPDX-License-Identifier: GPL-3.0-or-later
//
// WRLD, WATR, CLMT, WTHR, SPGD, REGN and IMGS: the worldspaces and what makes
// their weather, water and look. Private to pack/world/.
#pragma once

#include "context.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/pack/world_generated.h"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/merge.hpp"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace bethconv::pack::detail {

class EnvironmentCollector {
public:
    explicit EnvironmentCollector(CollectContext& shared) : shared_(shared) {}

    /// WRLD, WATR, CLMT, WTHR, SPGD, REGN or IMGS; nothing for another type.
    void collect(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);

    /// A WRLD a later plugin overrides: only its RNAM lists are taken.
    void collect_large_refs(const record::MergedRecord& merged, io::SpanReader& data,
                            const record::FormContext& form_ctx);

    /// The worldspaces, waters, climates, weathers, image spaces,
    /// precipitations and regions, each in id order, into `world`.
    void finish(wfb::WorldT& world);

private:
    void on_worldspace(const record::MergedRecord& merged, io::SpanReader& data,
                       const record::FormContext& form_ctx);
    void on_water(const record::MergedRecord& merged, io::SpanReader& data);
    void on_climate(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx);
    void on_weather(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx);
    void on_image_space(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx);
    void on_precipitation(const record::MergedRecord& merged, io::SpanReader& data,
                          const record::FormContext& form_ctx);
    void on_region(const record::MergedRecord& merged, io::SpanReader& data,
                   const record::FormContext& form_ctx);

    /// One RNAM entry with its FormID made global: the list's cell (y, x)
    /// and the reference's own.
    struct LargeEntry {
        std::uint32_t ref{};
        std::int16_t list_y{};
        std::int16_t list_x{};
        std::int16_t cell_y{};
        std::int16_t cell_x{};
    };
    void take_large_lists(const record::MergedRecord& merged, const record::Worldspace& w,
                          bool& failed);
    void resolve_large_refs(const wfb::WorldT& world, wfb::WorldspaceT& out,
                            const std::map<std::pair<std::int16_t, std::int16_t>,
                                           std::vector<LargeEntry>>& lists_by_cell);

    CollectContext& shared_;
    std::map<std::uint32_t, wfb::WorldspaceT> worlds_;
    /// RNAM lists by worldspace and list cell (y, x), from every version of
    /// the WRLD in load order, the last to list a cell deciding it; resolved
    /// in `finish` once the cells' references are known.
    std::map<std::uint32_t, std::map<std::pair<std::int16_t, std::int16_t>,
                                     std::vector<LargeEntry>>>
        large_entries_;
    std::map<std::uint32_t, wfb::WaterT> waters_;
    std::map<std::uint32_t, wfb::ClimateT> climates_;
    std::map<std::uint32_t, wfb::WeatherT> weathers_;
    std::map<std::uint32_t, wfb::ImageSpaceT> image_spaces_;
    std::map<std::uint32_t, wfb::PrecipitationT> precipitations_;
    std::map<std::uint32_t, wfb::RegionT> regions_;
};

} // namespace bethconv::pack::detail
