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

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Worldspace>> write_worlds(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Water>> write_waters(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Climate>> write_climates(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Weather>> write_weathers(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::ImageSpace>> write_image_spaces(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Precipitation>> write_precipitations(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Region>> write_regions(
        flatbuffers::FlatBufferBuilder& builder);

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
    std::map<std::uint32_t, Worldspace> worlds_;
    std::map<std::uint32_t, WorldWater> waters_;
    std::map<std::uint32_t, WorldClimate> climates_;
    std::map<std::uint32_t, WorldWeather> weathers_;
    std::map<std::uint32_t, WorldImageSpace> image_spaces_;
    std::map<std::uint32_t, WorldPrecipitation> precipitations_;
    std::map<std::uint32_t, WorldRegion> regions_;
};

} // namespace bethconv::pack::detail
