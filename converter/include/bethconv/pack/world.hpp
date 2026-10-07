// SPDX-License-Identifier: GPL-3.0-or-later
//
// `world.fb`: cells, their references and the base objects they place, decoded
// for the engine. Schema: formats/schema/world.fbs.
//
// Built during a merge pass because FormIDs inside record payloads are
// plugin-local; only the merge still knows which plugin each winning record
// came from, and so how to resolve them.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/pack/world_generated.h"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"
#include "skydot_formats/flags.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::pack {

/// Bumped whenever the meaning of anything in world.fbs changes.
inline constexpr std::uint32_t k_world_format_version = 12;

struct WorldStats {
    std::uint64_t cells{};
    std::uint64_t interior_cells{};
    std::uint64_t refs{};
    std::uint64_t doors{};
    std::uint64_t bases{};
    std::uint64_t lights{};
    std::uint64_t worlds{};
    std::uint64_t terrains{};
    std::uint64_t terrain_layers{};
    std::uint64_t land_textures{};
    std::uint64_t waters{};
    std::uint64_t climates{};
    std::uint64_t weathers{};
    /// Scripts attached to references and bases.
    std::uint64_t scripts{};
    std::uint64_t locks{};
    std::uint64_t links{};
    std::uint64_t activate_parents{};
    std::uint64_t primitives{};
    std::uint64_t quests{};
    std::uint64_t quest_aliases{};
    std::uint64_t quest_fragments{};
    std::uint64_t globals{};
    std::uint64_t actors{};
    std::uint64_t precipitations{};
    std::uint64_t regions{};
    std::uint64_t navmeshes{};
    std::uint64_t nav_triangles{};
    /// What actors are built from (world.fb format 8).
    std::uint64_t npcs{};
    std::uint64_t races{};
    std::uint64_t armors{};
    std::uint64_t armor_addons{};
    std::uint64_t outfits{};
    std::uint64_t leveled_lists{};
    /// AI packages and templates (format 9).
    std::uint64_t packages{};
    /// IMGS and LGTM (format 10).
    std::uint64_t image_spaces{};
    std::uint64_t lighting_templates{};
    /// Large references (format 11): kept, the cell lists they appear in, and
    /// RNAM entries dropped because the reference is deleted, absent,
    /// initially disabled or in another worldspace (each reference once).
    std::uint64_t large_refs{};
    std::uint64_t large_ref_cells{};
    std::uint64_t large_refs_dropped{};
    /// Navmeshes whose parent is not a cell; not included.
    std::uint64_t orphan_navmeshes{};
    /// References whose base or other FormIDs could not be resolved; the
    /// reference is kept with the unresolved field set to 0.
    std::uint64_t unresolved{};
    /// References whose parent is not a CELL; not included.
    std::uint64_t orphan_refs{};
    /// Records whose payload failed to parse; not included.
    std::uint64_t parse_errors{};
    /// Bases whose VMAD failed to parse; included without scripts.
    std::uint64_t script_errors{};
    std::uint64_t file_bytes{};
};

/// Run one merge pass and write `out`. `order` must be the order `world` was
/// built from.
[[nodiscard]] io::ParseResult<WorldStats> write_world(const record::MergedWorld& world,
                                                      const record::LoadOrder& order,
                                                      const std::filesystem::path& out);

// ---- reading --------------------------------------------------------------

/// The vertices along one side of a cell's terrain grid.
inline constexpr std::size_t k_terrain_grid = 33;

/// Heights in game units, row-major from the south-west corner.
[[nodiscard]] std::vector<float> terrain_heights(const wfb::TerrainT& terrain);

[[nodiscard]] inline bool is_interior(const wfb::CellT& cell) noexcept {
    return skydot::formats::has_flag(cell.flags, wfb::CellFlags::interior);
}

/// The land of this worldspace is its parent's.
[[nodiscard]] inline bool uses_parent_land(const wfb::WorldspaceT& world) noexcept {
    return world.parent != 0 &&
           skydot::formats::has_flag(world.parent_flags, wfb::ParentFlags::land_data);
}

/// A verified `world.fb`. Lookups copy a table out as flatc's object type
/// (`wfb::CellT` for `wfb::Cell`, and so on), so a field added to world.fbs
/// reads here without an edit.
class WorldFile {
public:
    WorldFile(WorldFile&&) noexcept;
    WorldFile& operator=(WorldFile&&) noexcept;
    ~WorldFile();

    [[nodiscard]] static io::ParseResult<WorldFile> open(const std::filesystem::path& path);
    /// `bytes` must outlive the result.
    [[nodiscard]] static io::ParseResult<WorldFile> from_bytes(std::span<const std::byte> bytes,
                                                               std::string_view origin);

    [[nodiscard]] std::size_t cell_count() const noexcept;
    [[nodiscard]] std::size_t base_count() const noexcept;
    [[nodiscard]] std::optional<wfb::CellT> cell(std::uint32_t id) const;
    [[nodiscard]] std::optional<wfb::CellT> cell_at(std::size_t index) const;
    /// Case-insensitive editor id match; linear.
    [[nodiscard]] std::optional<wfb::CellT> cell_by_editor_id(std::string_view editor_id) const;
    [[nodiscard]] std::optional<wfb::BaseT> base(std::uint32_t id) const;
    [[nodiscard]] std::vector<wfb::WorldspaceT> worldspaces() const;
    [[nodiscard]] std::optional<wfb::LandTextureT> land_texture(std::uint32_t id) const;
    [[nodiscard]] std::optional<wfb::WaterT> water(std::uint32_t id) const;
    [[nodiscard]] std::optional<wfb::ClimateT> climate(std::uint32_t id) const;
    [[nodiscard]] std::optional<wfb::WeatherT> weather(std::uint32_t id) const;
    [[nodiscard]] std::optional<wfb::ImageSpaceT> image_space(std::uint32_t id) const;
    [[nodiscard]] std::optional<wfb::PrecipitationT> precipitation(std::uint32_t id) const;
    /// Regions with weather data.
    [[nodiscard]] std::vector<wfb::RegionT> regions() const;
    [[nodiscard]] std::size_t quest_count() const noexcept;
    [[nodiscard]] std::optional<wfb::QuestT> quest(std::uint32_t id) const;
    [[nodiscard]] std::optional<wfb::GlobalT> global(std::uint32_t id) const;
    [[nodiscard]] std::vector<wfb::ActorRef> actors() const;
    [[nodiscard]] std::optional<wfb::NpcT> npc(std::uint32_t id) const;
    [[nodiscard]] std::optional<wfb::PackageT> package(std::uint32_t id) const;
    [[nodiscard]] std::size_t package_count() const noexcept;
    [[nodiscard]] std::optional<wfb::RaceT> race(std::uint32_t id) const;
    [[nodiscard]] std::optional<wfb::ArmorAddonT> armor_addon(std::uint32_t id) const;
    /// Plugin names and FormID prefixes, in load order.
    [[nodiscard]] std::vector<wfb::PluginT> plugins() const;
    /// The exterior cell of `world` at grid (x, y); linear.
    [[nodiscard]] std::optional<wfb::CellT> cell_at_grid(std::uint32_t world, std::int32_t x,
                                                        std::int32_t y) const;

private:
    class Impl;
    explicit WorldFile(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace bethconv::pack
