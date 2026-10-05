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
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/forms_game.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"
#include "bethconv/record/vmad.hpp"
#include "skydot_formats/flags.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bethconv::pack {

/// Bumped whenever the meaning of anything in world.fbs changes.
inline constexpr std::uint32_t k_world_format_version = 10;

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

struct WorldRef {
    std::uint32_t id{};
    std::uint32_t base{};
    record::Vec3 position;
    record::Vec3 rotation;
    float scale{1.0F};
    wfb::RefFlags flags{};
    std::uint32_t enable_parent{};
};

struct WorldDoor {
    std::uint32_t ref{};
    std::uint32_t destination{};
    record::Vec3 position;
    record::Vec3 rotation;
};

/// Scripts of one reference. FormIDs in properties are global.
struct WorldRefScripts {
    std::uint32_t ref{};
    std::vector<std::unique_ptr<wfb::ScriptT>> scripts;
};

struct WorldLock {
    std::uint32_t ref{};
    std::uint8_t level{};
    std::uint8_t flags{};
    std::uint32_t key{};
};

/// See world.fbs `LightOverride`.
struct WorldLightOverride {
    std::uint32_t ref{};
    bool has_radius{};
    float radius{};
    bool has_light_data{};
    float fov{};
    float fade{};
    float end_distance_cap{};
    float shadow_depth_bias{};
};

struct WorldLink {
    std::uint32_t ref{};
    std::uint32_t keyword{};
    std::uint32_t target{};
};

struct WorldActivateParent {
    std::uint32_t ref{};
    std::uint32_t parent{};
    float delay{};
};

struct WorldPrimitive {
    std::uint32_t ref{};
    record::Vec3 bounds;
    std::uint32_t type{};
};

struct WorldCellLighting {
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

struct WorldTerrainLayer {
    std::uint32_t texture{};
    std::uint8_t quadrant{};
    std::int16_t layer{};
    std::vector<std::uint16_t> points;
    std::vector<std::uint8_t> opacity;
};

/// See world.fbs `Terrain`.
struct WorldTerrain {
    static constexpr std::size_t k_grid = 33;
    float height_offset{};
    std::vector<std::int8_t> height_deltas;
    std::vector<std::uint8_t> colours;
    std::vector<WorldTerrainLayer> layers;
};

/// See world.fbs `NavMesh`.
struct WorldNavMesh {
    struct Triangle {
        std::array<std::uint16_t, 3> vertices{};
        std::array<std::int16_t, 3> edges{};
        wfb::NavTriangleFlags flags{};
        std::uint16_t cover{};
    };
    struct Link {
        std::uint32_t type{};
        std::uint32_t navmesh{};
        std::int16_t triangle{};
    };
    struct Door {
        std::int16_t triangle{};
        std::uint32_t door{};
    };
    std::uint32_t id{};
    std::vector<record::Vec3> vertices;
    std::vector<Triangle> triangles;
    std::vector<Link> links;
    std::vector<Door> doors;
};

struct WorldLight {
    std::uint32_t radius{};
    std::uint32_t color{};
    wfb::LightFlags flags{};
    float falloff_exponent{};
    float fov{};
    float near_clip{};
    float fade{};
    float flicker_period{};
    float flicker_intensity{};
    float flicker_movement{};
};

/// See world.fbs `Water`.
struct WorldWater {
    struct Layer {
        float wind_direction{};
        float wind_speed{};
        float uv_scale{};
        float amplitude{};
    };
    std::uint32_t id{};
    std::string editor_id;
    std::uint8_t opacity{};
    std::uint8_t flags{};
    std::uint32_t shallow_color{};
    std::uint32_t deep_color{};
    std::uint32_t reflection_color{};
    float sun_specular_power{};
    float reflectivity{};
    float fresnel{};
    float fog_near{};
    float fog_far{};
    float specular_power{};
    float refraction_magnitude{};
    float reflection_magnitude{};
    std::array<Layer, 3> layers{};
    std::vector<std::string> noise;
};

struct Worldspace {
    std::uint32_t id{};
    std::string editor_id;
    std::uint32_t parent{};
    wfb::ParentFlags parent_flags{};
    std::uint8_t flags{};
    std::optional<std::array<float, 2>> defaults; ///< Land, water height.
    std::uint32_t water{};
    std::uint32_t climate{};
    std::array<float, 4> bounds{}; ///< min x, min y, max x, max y

    /// The land of this worldspace is its parent's.
    [[nodiscard]] bool uses_parent_land() const noexcept {
        return parent != 0 && skydot::formats::has_flag(parent_flags, wfb::ParentFlags::land_data);
    }
};

/// See world.fbs `Climate`.
struct WorldClimate {
    std::uint32_t id{};
    std::string editor_id;
    std::vector<std::pair<std::uint32_t, std::int32_t>> weathers; ///< weather, chance
    std::array<float, 4> sun{}; ///< sunrise begin/end, sunset begin/end (hours)
    std::string sun_texture;       ///< Virtual paths.
    std::string sun_glare_texture;
    std::string sky;               ///< The night sky's model.
    std::uint8_t volatility{};
    std::uint8_t moons{};          ///< Bit 0 Masser, bit 1 Secunda.
    std::uint8_t phase_length{};   ///< Days per moon phase.
};

/// See world.fbs `CloudLayer`.
struct WorldCloudLayer {
    std::string texture;
    float speed_x{};
    float speed_y{};
    std::array<std::uint32_t, 4> colors{};
    std::array<float, 4> alphas{};
    bool enabled{};
};

/// See world.fbs `Weather`.
struct WorldWeather {
    std::uint32_t id{};
    std::string editor_id;
    std::vector<std::uint32_t> colors;
    std::vector<float> fog;
    std::vector<std::uint32_t> directional_ambient;
    std::vector<WorldCloudLayer> clouds;
    float wind_speed{};
    float wind_direction{};
    float wind_direction_range{};
    float transition_delta{};
    float sun_glare{};
    float sun_damage{};
    float precipitation_begin{};
    float precipitation_end{};
    float thunder_begin{};
    float thunder_end{};
    float thunder_frequency{};
    wfb::WeatherClass classification{};
    std::uint32_t lightning_color{};
    std::uint32_t precipitation{};
    std::string aurora;
    /// IMSP: sunrise, day, sunset, night; empty if none.
    std::vector<std::uint32_t> image_spaces;
};

/// See world.fbs `ImageSpace`.
struct WorldImageSpace {
    std::uint32_t id{};
    std::string editor_id;
    std::vector<float> hdr;
    std::vector<float> cinematic;
    std::vector<float> tint;
};

/// See world.fbs `Precipitation`.
struct WorldPrecipitation {
    std::uint32_t id{};
    std::string editor_id;
    std::string texture;
    float gravity_velocity{};
    float rotation_velocity{};
    float size_x{};
    float size_y{};
    float center_offset_min{};
    float center_offset_max{};
    float rotation_range{};
    std::uint32_t subtextures_x{};
    std::uint32_t subtextures_y{};
    std::uint8_t type{};
    std::uint32_t box_size{};
    float density{};
};

/// See world.fbs `Region`.
struct WorldRegion {
    struct Weather {
        std::uint32_t weather{};
        std::int32_t chance{};
        std::uint32_t global{};
    };
    std::uint32_t id{};
    std::string editor_id;
    std::uint32_t world{};
    /// Polygons as x, y pairs in game units.
    std::vector<std::vector<float>> areas;
    std::vector<Weather> weathers;
    std::uint8_t weather_priority{};
    bool weather_override{};
};

/// See world.fbs `Quest` and its parts.
struct WorldQuestAlias {
    std::uint32_t id{};
    std::string name;
    bool location{};
    wfb::AliasFlags flags{};
    std::uint32_t forced{};
    std::uint32_t unique_actor{};
    std::uint32_t external_quest{};
    std::int32_t external_alias{-1};
    std::uint32_t created_object{};
    std::uint32_t create_at{};
    std::uint16_t conditions{};
    std::uint32_t display_name{};
    std::vector<std::unique_ptr<wfb::ScriptT>> scripts;
};
struct WorldQuestLogEntry {
    wfb::LogEntryFlags flags{};
    std::string text;
    std::uint16_t conditions{};
};
struct WorldQuestStage {
    std::uint16_t index{};
    wfb::StageFlags flags{};
    std::vector<WorldQuestLogEntry> log;
};
struct WorldQuestObjective {
    std::uint16_t index{};
    std::uint32_t flags{};
    std::string text;
    std::vector<std::int32_t> targets;
};
struct WorldQuestFragment {
    std::uint16_t stage{};
    std::int32_t log_entry{};
    std::string function;
};
struct WorldQuest {
    std::uint32_t id{};
    std::string editor_id;
    std::string name;
    wfb::QuestFlags flags{};
    std::uint8_t priority{};
    std::uint32_t type{};
    std::uint32_t event{};
    std::vector<std::unique_ptr<wfb::ScriptT>> scripts;
    std::string fragment_script;
    std::vector<WorldQuestFragment> fragments;
    std::vector<WorldQuestStage> stages;
    std::vector<WorldQuestObjective> objectives;
    std::vector<WorldQuestAlias> aliases;
};
struct WorldGlobal {
    std::uint32_t id{};
    std::string editor_id;
    char kind{};
    float value{};
};
/// NPC_ as written to world.fb; FormIDs global.
struct WorldNpc {
    std::uint32_t id{};
    std::string editor_id;
    std::string name;
    wfb::NpcFlags flags{}; ///< ACBS.
    std::uint16_t level{};
    std::uint32_t race{};
    std::uint32_t template_form{};
    wfb::NpcTemplateFlags template_flags{};
    std::uint32_t skin{};
    std::uint32_t default_outfit{};
    std::uint32_t sleeping_outfit{};
    float height{};
    float weight{};
    std::vector<std::uint32_t> head_parts;
    std::vector<std::pair<std::uint32_t, std::int32_t>> items;
    std::string face_model;
    std::array<float, 3> skin_tone{1, 1, 1};
    std::vector<std::uint32_t> packages{};         ///< PKID
    std::vector<std::uint32_t> default_packages{}; ///< DPLT's FLST, expanded.
    std::uint32_t default_package_list{};          ///< DPLT; not written.
    std::vector<std::pair<std::uint32_t, std::int32_t>> factions{}; ///< SNAM: faction, rank.
};

/// PACK as written to world.fb; FormIDs global (see world.fbs, Package).
struct WorldPackage {
    std::uint32_t id{};
    std::string editor_id{};
    std::uint8_t type{};
    wfb::PackageFlags flags{};
    std::uint8_t interrupt_override{};
    std::uint8_t speed{};
    std::uint16_t interrupt_flags{};
    record::Package::Schedule schedule{};
    std::vector<record::Condition> conditions{};
    std::uint32_t template_package{};
    struct Input {
        std::int8_t key{-1};
        std::string type{};
        std::string name{};
        float number{};
        record::Package::Location location{};///< type -1 if absent.
        record::Package::Target target{};///< type -1 if absent.
    };
    std::vector<Input> inputs{};
    struct Branch {
        std::string type{};
        std::vector<record::Condition> conditions{};
        std::uint32_t children{};
        wfb::BranchFlags flags{};
        std::string procedure{};
        bool success_completes{};
        std::vector<std::uint8_t> inputs{};
        std::uint32_t set_flags{};
        std::uint32_t clear_flags{};
        std::int8_t speed{-1};
    };
    std::vector<Branch> branches{};
    std::uint8_t idle_flags{};
    float idle_timer{};
    std::vector<std::uint32_t> idles{};
    std::uint32_t owner_quest{};
    std::uint32_t combat_style{};
    std::uint32_t on_begin_idle{};
    std::uint32_t on_end_idle{};
    std::uint32_t on_change_idle{};
};

/// RACE as written to world.fb; index 0 male, 1 female.
struct WorldRace {
    std::uint32_t id{};
    std::string editor_id;
    std::array<std::string, 2> skeletons;
    std::array<std::string, 2> behaviours;
    std::uint32_t skin{};
    std::array<float, 2> heights{1, 1};
    std::array<float, 2> weights{1, 1};
    std::uint32_t flags{};
    struct BodyPart {
        bool female{};
        std::uint32_t index{};
        std::string model;
    };
    std::vector<BodyPart> body_parts;
    std::array<std::vector<std::uint32_t>, 2> head_parts;
    std::uint32_t armor_race{};
};

struct WorldArmor {
    std::uint32_t id{};
    std::string editor_id;
    std::uint32_t slots{};
    std::uint32_t race{};
    std::vector<std::uint32_t> addons;
};

struct WorldArmorAddon {
    std::uint32_t id{};
    std::string editor_id;
    std::uint32_t slots{};
    std::uint32_t race{};
    std::vector<std::uint32_t> additional_races;
    std::array<std::string, 2> models;
    std::array<std::uint8_t, 2> priorities{};
    std::array<std::uint8_t, 2> weight_sliders{};
};

struct WorldOutfit {
    std::uint32_t id{};
    std::vector<std::uint32_t> items;
};

struct WorldLeveledList {
    std::uint32_t id{};
    std::uint32_t type{}; ///< LVLI or LVLN as a FourCC value.
    wfb::LeveledListFlags flags{};
    std::uint8_t chance_none{};
    struct Entry {
        std::uint16_t level{};
        std::uint16_t count{};
        std::uint32_t form{};
    };
    std::vector<Entry> entries;
};

struct WorldActor {
    std::uint32_t ref{};
    std::uint32_t base{};
    std::uint32_t cell{};
    record::Vec3 position;
    record::Vec3 rotation;
    wfb::RefFlags flags{};
};

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
