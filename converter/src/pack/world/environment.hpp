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
#include "bethconv/record/merge.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace bethconv::pack::detail {

class EnvironmentCollector {
public:
    explicit EnvironmentCollector(CollectContext& shared) : shared_(shared) {}

    /// WRLD, WATR, CLMT, WTHR, SPGD, REGN or IMGS; nothing for another type.
    void collect(const record::MergedRecord& merged, io::SpanReader& data,
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

    CollectContext& shared_;
    std::map<std::uint32_t, wfb::WorldspaceT> worlds_;
    std::map<std::uint32_t, wfb::WaterT> waters_;
    std::map<std::uint32_t, wfb::ClimateT> climates_;
    std::map<std::uint32_t, wfb::WeatherT> weathers_;
    std::map<std::uint32_t, wfb::ImageSpaceT> image_spaces_;
    std::map<std::uint32_t, wfb::PrecipitationT> precipitations_;
    std::map<std::uint32_t, wfb::RegionT> regions_;
};

} // namespace bethconv::pack::detail
