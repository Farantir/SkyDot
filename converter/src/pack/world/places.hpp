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

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bethconv::pack::detail {

struct CellEntry {
    std::uint32_t id{};
    std::string editor_id;
    std::uint32_t world{};
    wfb::CellFlags flags{};
    std::optional<record::Cell::Grid> grid;
    float water_height{};
    std::optional<WorldCellLighting> lighting;
    std::uint32_t lighting_template{};
    std::uint32_t image_space{};
    bool persistent{};
    std::uint32_t water{};
};

/// LGTM: DATA has XCLL's layout up to the light fade distances (its
/// directional ambient block is unused); the directional ambient is DALC.
struct LightingTemplateEntry {
    WorldCellLighting lighting;
};

/// Per-cell data about its references beyond placement.
struct CellExtras {
    std::vector<WorldRefScripts> scripts;
    std::vector<WorldLock> locks;
    std::vector<WorldLink> links;
    std::vector<WorldActivateParent> activate_parents;
    std::vector<WorldPrimitive> primitives;
    std::vector<WorldLightOverride> light_overrides;
};

class PlaceCollector {
public:
    explicit PlaceCollector(CollectContext& shared) : shared_(shared) {}

    /// CELL, REFR, ACHR, LAND, NAVM or LGTM; nothing for another type.
    void collect(const record::MergedRecord& merged, const record::RecordContext& ctx,
                 io::SpanReader& data, const record::FormContext& form_ctx);

    /// Every cell with its references, doors, extras, navmeshes, terrain and
    /// lighting, in id order.
    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Cell>> write_cells(
        flatbuffers::FlatBufferBuilder& builder);

    /// The placed actors, sorted by reference.
    [[nodiscard]] flatbuffers::Offset<flatbuffers::Vector<const wfb::ActorRef*>> write_actors(
        flatbuffers::FlatBufferBuilder& builder);

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
    std::unordered_map<std::uint32_t, std::vector<WorldRef>> refs_;
    std::unordered_map<std::uint32_t, std::vector<WorldDoor>> doors_;
    std::unordered_map<std::uint32_t, CellExtras> extras_;
    std::unordered_map<std::uint32_t, WorldTerrain> terrains_;
    std::unordered_map<std::uint32_t, std::vector<WorldNavMesh>> navmeshes_;
    std::map<std::uint32_t, LightingTemplateEntry> lighting_templates_;
    std::vector<WorldActor> actors_;
};

} // namespace bethconv::pack::detail
