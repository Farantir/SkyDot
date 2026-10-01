// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotWorld`: reads a pack's `world.fb` (formats/pack-format.md,
// schema formats/schema/world.fbs) and builds cells from it.
//
// Coordinates in world.fb are Skyrim's: Z-up, game units, radians. They are
// converted with the same axis rotation (Z-up -> Y-up, -90 degrees about X) and
// unit scale (0.0142875 m per unit) the converter applies to every mesh, so a
// model placed with `skyrim_transform` lands where the game puts it.
#pragma once

#include "assets/asset_cache.hpp"
#include "world/materials.hpp"
#include "world/navmesh.hpp"
#include "world/terrain.hpp"
#include "world/water.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace bethconv::pack::wfb {
struct World;
struct Cell;
struct Base;
struct Ref;
struct Worldspace;
struct Water;
struct DoorLink;
struct Script;
} // namespace bethconv::pack::wfb

namespace skydot {

class SkydotWorld : public godot::RefCounted {
    GDCLASS(SkydotWorld, godot::RefCounted)

public:
    /// The world.fb format version this engine reads.
    static constexpr int WORLD_FORMAT_VERSION = 5;
    /// Metres per game unit, as used by the converter's mesh writer.
    static constexpr double UNIT_SCALE = 0.0142875;

    /// Read and verify a world.fb. On failure the world stays closed and
    /// `get_error` explains.
    godot::Error open(const godot::String& path);
    bool is_open() const;
    godot::String get_error() const;

    std::int64_t get_cell_count() const;
    std::int64_t get_base_count() const;

    /// Form id of the cell with this editor id (case-insensitive), or 0.
    std::int64_t find_cell(const godot::String& editor_id) const;
    /// Cells whose editor id contains `filter` (case-insensitive), as
    /// dictionaries with id, editor_id, interior and ref_count.
    godot::Array list_cells(const godot::String& filter, bool interior_only) const;
    /// Cell details without references: id, editor_id, interior, world, grid
    /// (Vector2i or null), water_height (metres), lighting (Dictionary or null),
    /// ref_count, door_count.
    godot::Dictionary get_cell(std::int64_t id) const;
    /// The cell's references: id, base, transform (Godot space), scale,
    /// disabled, persistent, enable_parent.
    godot::Array get_refs(std::int64_t cell_id) const;
    /// id, type, editor_id, model, light (Dictionary or null), flags, scripts
    /// (as in get_ref_info). Empty if the base has no model, light or script.
    godot::Dictionary get_base(std::int64_t id) const;

    /// A load door: ref, cell, destination (the door it leads to),
    /// destination_cell, destination_world (0 for an interior),
    /// destination_interior, arrival (Transform3D in Godot space, facing where
    /// the player faces on arrival). Empty if `ref` is not a load door.
    godot::Dictionary get_door(std::int64_t ref) const;
    /// What activating a reference involves: id, cell, base, type (the base's
    /// record type), editor_id (the base's), activatable, disabled (initially),
    /// position and rotation (Skyrim space: game units, radians), scale,
    /// primitive (XPRM bounds in game units and type, or null),
    /// parent_activate_only,
    /// lock (level, key; or null), door (get_door, or null), links (Array of
    /// keyword, target), activate_parents (Array of parent, delay) and
    /// scripts: the base's, then the reference's, each a Dictionary of name,
    /// status, from_ref, properties (name -> value; objects as FormIDs, or a
    /// Dictionary of form and alias for quest aliases; arrays as Arrays).
    /// Empty if the cell has no such reference.
    godot::Dictionary get_ref_info(std::int64_t cell, std::int64_t ref) const;
    /// The nearest activatable reference whose model's bounds the segment
    /// `from`-`to` (Godot space) crosses, among the cells built under `root`:
    /// ref, cell, node, distance, position. Empty if none.
    godot::Dictionary pick_ref(godot::Node* root, const godot::Vector3& from,
                               const godot::Vector3& to) const;
    /// Build one reference of `cell` under a new Node3D, even if it starts
    /// disabled (for a script enabling it). Null if there is no such reference.
    godot::Node3D* build_ref(std::int64_t cell, std::int64_t ref) const;
    /// References of `cell` whose base or own VMAD names scripts, sorted.
    godot::PackedInt64Array get_scripted_refs(std::int64_t cell) const;
    /// The cell holding reference `ref`, or 0. The first call builds the index.
    std::int64_t get_ref_cell(std::int64_t ref) const;
    /// References activated when `ref` is: Array of ref, cell, delay.
    godot::Array get_activate_children(std::int64_t ref) const;
    /// References whose enable state follows `ref`'s: Array of ref ids.
    godot::PackedInt64Array get_enable_children(std::int64_t ref) const;

    // Quests, globals and placed actors (world/quests.cpp).
    std::int64_t get_quest_count() const;
    bool has_quest(std::int64_t id) const;
    /// Quests whose editor id contains `filter` (case-insensitive): id,
    /// editor_id, name, start_game_enabled.
    godot::Array list_quests(const godot::String& filter) const;
    /// Form id of the quest with this editor id (case-insensitive), or 0.
    std::int64_t find_quest(const godot::String& editor_id) const;
    /// id, editor_id, name, flags, priority, type, event, start_game_enabled,
    /// run_once, allow_repeated_stages, scripts (as get_ref_info),
    /// fragment_script, fragments (stage, log_entry, function), stages (index,
    /// start_up, shut_down, log: flags, text, conditions), objectives (index,
    /// flags, text, targets) and aliases (id, name, location, flags, optional,
    /// forced, unique_actor, external_quest, external_alias, created_object,
    /// create_at, conditions, display_name, scripts). Empty if not a quest.
    godot::Dictionary get_quest(std::int64_t id) const;
    /// id, editor_id, kind ("s", "l" or "f"), value. Empty if not a global.
    godot::Dictionary get_global(std::int64_t id) const;
    /// A placed actor (ACHR): ref, base, cell, position, rotation (Skyrim
    /// space), disabled, persistent. Empty if `ref` is none.
    godot::Dictionary get_actor(std::int64_t ref) const;
    /// Game.GetFormFromFile: the global form id of object `id` of `plugin`
    /// (any case), or 0 if the plugin is not loaded.
    std::int64_t get_form_from_file(std::int64_t id, const godot::String& plugin) const;
    /// The placed actor of NPC_ `npc` (the lowest ref if there are several),
    /// or 0.
    std::int64_t find_actor_of(std::int64_t npc) const;

    /// Build a cell under a new Node3D: models (from the pack's asset cache)
    /// for references with one, lights for LIGH bases. Initially disabled
    /// references and editor markers are skipped; billboard nodes get a
    /// SkydotBillboard. Statistics are stored as node metadata "skydot_stats".
    godot::Node3D* build_cell(std::int64_t id) const;

    /// Worldspaces as dictionaries: id, editor_id, parent, land_world (the
    /// worldspace whose terrain it shows), default_water_height (metres or
    /// null), bounds (Rect2 in game units, x/y).
    godot::Array list_worlds() const;
    /// Form id of the worldspace with this editor id (case-insensitive), or 0.
    std::int64_t find_world(const godot::String& editor_id) const;
    /// Form id of `world`'s exterior cell at grid (x, y), or 0.
    std::int64_t get_exterior_cell(std::int64_t world, std::int64_t x, std::int64_t y) const;
    /// Build exterior cell (x, y) of `world` under a new Node3D: terrain (from
    /// the land world), water, the cell's references and the worldspace's
    /// persistent references positioned in it. Works without a CELL record if
    /// there is terrain. Statistics as for build_cell, plus "terrain" and
    /// "water". Null if there is neither a cell nor terrain.
    godot::Node3D* build_exterior(std::int64_t world, std::int64_t x, std::int64_t y) const;
    /// The same in steps, so a large cell does not stall a frame: terrain and
    /// water now, references by `continue_build`. Null as build_exterior.
    godot::Node3D* begin_exterior(std::int64_t world, std::int64_t x, std::int64_t y) const;
    /// Place references of a cell `begin_exterior` returned until
    /// `budget_usec` is spent. True once all are placed (and "skydot_stats"
    /// is set); also true for a root with no build under way.
    bool continue_build(godot::Node3D* root, std::int64_t budget_usec) const;

    /// The pack's asset cache, which every model and texture comes from.
    /// Set by SkydotPack::open_world; without it nothing loads.
    void set_assets(std::shared_ptr<AssetCache> assets) { assets_ = std::move(assets); }

    /// Virtual paths of the resources (models, land and water textures)
    /// exterior cell (x, y) of `world` needs.
    godot::PackedStringArray get_exterior_resources(std::int64_t world, std::int64_t x,
                                                    std::int64_t y) const;
    /// Start loading those resources on the asset cache's threads. Returns how
    /// many are still pending; 0 means build_exterior will not load anything
    /// itself. Call again to poll.
    std::int64_t request_exterior(std::int64_t world, std::int64_t x, std::int64_t y);
    /// Loaded resources kept for reuse, and those still loading.
    std::int64_t get_cached_resource_count() const;
    std::int64_t get_pending_resource_count() const;
    /// Drop cached resources nothing else holds.
    void trim_cache();

    /// Form id of the weather with this editor id (case-insensitive), or 0.
    std::int64_t find_weather(const godot::String& editor_id) const;
    /// Sky, light and fog of `world` at `hour` (0-24) under `weather`, or the
    /// climate's most likely weather if 0: weather (editor id), sky_upper,
    /// sky_lower, horizon, ambient, sunlight, fog_near_color, fog_far_color
    /// (Colors), fog_near, fog_far (metres), fog_power, fog_max, daylight
    /// (0 night to 1 day), sun_direction (towards the sun or, at night, the
    /// moon; Godot space). Empty if the worldspace has no climate or weather.
    godot::Dictionary get_sky(std::int64_t world, double hour, std::int64_t weather = 0) const;

    /// Compile every material and terrain shader variant now, so that building
    /// cells later never stalls on a first-time compile. Returns the number of
    /// variants.
    std::int64_t warm_up();

    /// Land texture repeats per cell side (default 8).
    void set_terrain_tiling(double repeats);
    double get_terrain_tiling() const;

    /// Replace imported materials with Skyrim-style shader materials in
    /// `build_cell` (see materials.hpp). On by default.
    void set_skyrim_materials(bool enabled);
    bool get_skyrim_materials() const;

    /// Play models' controllers and particle systems and flicker lights (see
    /// animator.hpp). On by default.
    void set_effects(bool enabled);
    bool get_effects() const;

    /// Give models and terrain physics bodies (see collision.hpp). On by
    /// default.
    void set_collision(bool enabled);
    bool get_collision() const { return collision_; }

    /// Give cells their navmeshes as navigation regions (see navmesh.hpp). On
    /// by default.
    void set_navigation(bool enabled) { navigation_ = enabled; }
    bool get_navigation() const { return navigation_; }
    /// The cell's navmeshes, as get_navmesh describes them.
    godot::Array get_navmeshes(std::int64_t cell) const;
    /// One navmesh: id, cell, vertices, triangles, water, links and doors (see
    /// navmesh_info in navmesh.hpp). Empty if there is no such navmesh.
    godot::Dictionary get_navmesh(std::int64_t id) const;

    /// Skyrim position (game units), rotation (radians) and scale to a Godot
    /// transform.
    static godot::Transform3D skyrim_transform(const godot::Vector3& position,
                                               const godot::Vector3& rotation, double scale);
    /// Skyrim position (game units) to Godot space (metres).
    static godot::Vector3 skyrim_position(const godot::Vector3& position);
    /// Godot space (metres) back to a Skyrim position (game units).
    static godot::Vector3 godot_to_skyrim(const godot::Vector3& position);

protected:
    static void _bind_methods();

private:
    godot::Error fail(godot::Error code, const godot::String& why);
    const bethconv::pack::wfb::Cell* cell_ptr(std::int64_t id) const;
    const bethconv::pack::wfb::Base* base_ptr(std::int64_t id) const;
    const bethconv::pack::wfb::Worldspace* world_ptr(std::int64_t id) const;
    const bethconv::pack::wfb::Cell* exterior_ptr(std::uint32_t world, std::int32_t x,
                                                   std::int32_t y) const;
    std::uint32_t land_world(std::uint32_t world) const;
    const bethconv::pack::wfb::Water* water_ptr(std::uint32_t id) const;
    /// The water type of exterior cell (x, y): its XCWT, else the worldspace's.
    std::uint32_t water_type(std::uint32_t world, const bethconv::pack::wfb::Cell* cell) const;
    void build_indexes();
    std::array<std::string, 2> land_texture_paths(std::uint32_t ltex) const;
    /// A resource from the cache, else loaded now (and cached).
    godot::Ref<godot::Resource> resource(const godot::String& vpath) const;
    /// materials_, created on first use and attached to the asset cache.
    SkydotMaterials& materials() const;

    struct BuildStats;
    struct BuildJob;
    /// Instance the reference's model and light under `root`.
    void place_ref(godot::Node3D* root, const bethconv::pack::wfb::Ref& ref, std::uint32_t cell,
                   BuildStats& stats, bool include_disabled = false) const;
    /// Initially disabled, following the enable parent chain.
    bool initially_disabled(const bethconv::pack::wfb::Ref& ref) const;
    /// Whether activating a reference of this base can do anything.
    bool activatable(const bethconv::pack::wfb::Base* base, std::uint32_t cell,
                     std::uint32_t ref) const;
    const bethconv::pack::wfb::DoorLink* door_ptr(std::uint32_t ref) const;
    godot::Dictionary stats_dictionary(const BuildStats& stats) const;

    godot::PackedByteArray bytes_;
    bool skyrim_materials_{true};
    bool effects_{true};
    bool collision_{true};
    bool navigation_{true};
    mutable godot::Ref<SkydotMaterials> materials_;
    const bethconv::pack::wfb::World* root_{};
    godot::String error_;

    /// (world, x, y) -> exterior cell, persistent cells excluded.
    std::unordered_map<std::uint64_t, const bethconv::pack::wfb::Cell*> exteriors_;
    /// (world, x, y) -> persistent references positioned in that cell.
    std::unordered_map<std::uint64_t, std::vector<const bethconv::pack::wfb::Ref*>> persistent_;
    NavIndex navmeshes_;
    /// Worldspace -> its persistent cell.
    std::unordered_map<std::uint32_t, std::uint32_t> persistent_cells_;
    /// (ref, cell) sorted by ref; built on first use.
    mutable std::vector<std::pair<std::uint32_t, std::uint32_t>> ref_cells_;
    /// Activate parent -> (child, cell, delay).
    std::unordered_multimap<std::uint32_t, std::tuple<std::uint32_t, std::uint32_t, float>>
        activate_children_;
    /// Enable parent -> its enable children.
    std::unordered_multimap<std::uint32_t, std::uint32_t> enable_children_;
    /// Enable parents by id.
    std::unordered_map<std::uint32_t, const bethconv::pack::wfb::Ref*> enable_parents_;
    /// NPC_ -> its lowest placed actor; built on first use.
    mutable std::unordered_map<std::uint32_t, std::uint32_t> actor_of_;
    /// Load door ref -> the cell holding it and its link.
    std::unordered_map<std::uint32_t,
                       std::pair<const bethconv::pack::wfb::Cell*,
                                 const bethconv::pack::wfb::DoorLink*>>
        doors_;
    mutable std::unique_ptr<TerrainBuilder> terrain_;
    /// Builds under way, by root instance id.
    mutable std::unordered_map<std::uint64_t, std::shared_ptr<BuildJob>> jobs_;
    mutable WaterMaterials water_;
    std::shared_ptr<AssetCache> assets_;
    double terrain_tiling_{8.0};
};

} // namespace skydot
