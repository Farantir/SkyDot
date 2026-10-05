// SPDX-License-Identifier: GPL-3.0-or-later
//
// CELL, REFR, ACHR, LAND, NAVM and LGTM: the places, what stands in them and
// how they are lit. A cell's references, doors, extras, navmeshes and terrain
// are keyed by the cell and written with it. Private to pack/world/.
#pragma once

#include "context.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/pack/world_generated.h"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/merge.hpp"
#include "bethconv/record/plugin.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace bethconv::pack::detail {

/// XCLL as a cell and a lighting template give it. wfb::CellLighting is
/// read-only, and the directional ambient belongs to the cell table, not to
/// that struct, so merging the two works on this.
struct Lighting {
    std::uint32_t ambient{};
    std::uint32_t directional{};
    std::uint32_t fog_near_color{};
    std::uint32_t fog_far_color{};
    float fog_near{};
    float fog_far{};
    float fog_power{};
    float fog_max{};
    std::int32_t directional_rotation_xy{};
    std::int32_t directional_rotation_z{};
    float directional_fade{};
    float light_fade_begin{};
    float light_fade_end{};
    std::uint32_t inherit{};
    /// x+, x-, y+, y-, z+, z-, RGBA bytes; all 0 if unknown.
    std::array<std::uint32_t, 6> directional_ambient{};
};

/// A cell: its table, which the CELL record fills in and its children (the
/// references, navmeshes and land that name it as their parent) add to, and
/// the XCLL, which is resolved against the lighting templates once every
/// record is in.
struct CellEntry {
    wfb::CellT cell;
    std::optional<Lighting> xcll;
    /// False if only children named the cell: they are orphans.
    bool has_record{};
};

/// LGTM: DATA has XCLL's layouts, 92, 72 or 64 bytes, up to the light fade
/// distances (its directional ambient block is unused, and it has no inherit
/// flags); the directional ambient is DALC.
struct LightingTemplateEntry {
    Lighting lighting;
};

class PlaceCollector {
public:
    explicit PlaceCollector(CollectContext& shared) : shared_(shared) {}

    /// CELL, REFR, ACHR, LAND, NAVM or LGTM; nothing for another type.
    void collect(const record::MergedRecord& merged, const record::RecordContext& ctx,
                 io::SpanReader& data, const record::FormContext& form_ctx);

    /// Every cell with its references, doors, extras, navmeshes, terrain and
    /// lighting, in id order, and the placed actors, sorted by reference, into
    /// `world`.
    void finish(wfb::WorldT& world);

private:
    void on_cell(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);
    void on_reference(const record::MergedRecord& merged, const record::RecordContext& ctx,
                      io::SpanReader& data, const record::FormContext& form_ctx);
    void on_land(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);
    void on_actor(const record::MergedRecord& merged, const record::RecordContext& ctx,
                  io::SpanReader& data, const record::FormContext& form_ctx);
    void on_navmesh(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx);
    void on_lighting_template(const record::MergedRecord& merged, io::SpanReader& data);

    CollectContext& shared_;
    std::map<std::uint32_t, CellEntry> cells_;
    std::map<std::uint32_t, LightingTemplateEntry> lighting_templates_;
    std::vector<wfb::ActorRef> actors_;
};

} // namespace bethconv::pack::detail
