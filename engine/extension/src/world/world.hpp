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
#include "actors/actor_placement.hpp"
#include "build/cell_builder.hpp"
#include "world/queries.hpp"
#include "data/world_data.hpp"
#include "skydot_formats/units.hpp"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
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
#include <unordered_map>
#include <vector>

namespace bethconv::pack::wfb {
struct World;
struct Base;
struct Ref;
struct ActorRef;
struct Script;
} // namespace bethconv::pack::wfb

namespace skydot {

struct ActorPlan;

class SkydotWorld : public godot::RefCounted {
    GDCLASS(SkydotWorld, godot::RefCounted)

public:
    /// The world.fb format versions this engine reads (see WorldData).
    static constexpr int WORLD_FORMAT_VERSION = WorldData::FORMAT_VERSION;
    static constexpr int WORLD_FORMAT_VERSION_MIN = WorldData::FORMAT_VERSION_MIN;
    /// Metres per game unit, as used by the converter's mesh writer; scripts
    /// read it with `unit_scale()`.
    static constexpr double UNIT_SCALE = formats::k_metres_per_unit;
    /// Game units along the side of an exterior cell.
    static constexpr int CELL_UNITS = formats::k_cell_units;

    /// Read and verify a world.fb. On failure the world stays closed and
    /// `get_error` explains. A world opens once: a second call is refused
    /// with ERR_ALREADY_IN_USE (every index points into the open file).
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
    /// An IMGS: id, editor_id, hdr (nine floats), cinematic (three), tint
    /// (four); see world.fbs. Empty if there is none.
    godot::Dictionary get_image_space(std::int64_t id) const;
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
    /// The usable reference the segment `from`-`to` (Godot space) points at,
    /// among the cells built under `root`: ref, cell, node, distance,
    /// position. In a scene tree with collision, the first body hit decides
    /// (its reference, or nothing behind it); otherwise, and for references
    /// without collision, the nearest model bounds crossed. Empty if none.
    godot::Dictionary pick_ref(godot::Node* root, const godot::Vector3& from,
                               const godot::Vector3& to) const;
    /// Build one reference of `cell` under a new Node3D, even if it starts
    /// disabled (for a script enabling it). Null if there is no such reference.
    godot::Node3D* build_ref(std::int64_t cell, std::int64_t ref);
    /// References of `cell` whose base or own VMAD names scripts, sorted.
    godot::PackedInt64Array get_scripted_refs(std::int64_t cell) const;
    /// The cell holding reference `ref`, or 0.
    std::int64_t get_ref_cell(std::int64_t ref) const;
    /// References activated when `ref` is: Array of ref, cell, delay.
    godot::Array get_activate_children(std::int64_t ref) const;
    /// References whose enable state follows `ref`'s: Array of ref ids.
    godot::PackedInt64Array get_enable_children(std::int64_t ref) const;

    // Quests, globals and placed actors (world/queries_quests.cpp).
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
    /// Form id of the NPC_ with this editor id (case-insensitive), or 0.
    std::int64_t find_npc(const godot::String& editor_id) const;

    /// Build a cell under a new Node3D: models (from the pack's asset cache)
    /// for references with one, lights for LIGH bases. Initially disabled
    /// references and editor markers are skipped; billboard nodes get a
    /// SkydotBillboard. Statistics are stored as node metadata "skydot_stats".
    godot::Node3D* build_cell(std::int64_t id);
    /// The same in steps: navmeshes now, references and actors by
    /// `continue_build`. Null as build_cell.
    godot::Node3D* begin_cell(std::int64_t id);
    /// Virtual paths of the resources interior `id` needs, and a request
    /// for them as `request_exterior`.
    godot::PackedStringArray get_cell_resources(std::int64_t id);
    std::int64_t request_cell(std::int64_t id);

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
    godot::Node3D* build_exterior(std::int64_t world, std::int64_t x, std::int64_t y);
    /// The same in steps, so a large cell does not stall a frame: terrain and
    /// water now, references by `continue_build`. Null as build_exterior.
    godot::Node3D* begin_exterior(std::int64_t world, std::int64_t x, std::int64_t y);
    /// Place references of a cell `begin_exterior` or `begin_cell` returned until
    /// `budget_usec` is spent. True once all are placed (and "skydot_stats"
    /// is set); also true for a root with no build under way.
    bool continue_build(godot::Node3D* root, std::int64_t budget_usec);
    /// Only the references (models, lights) of such a build; true once they
    /// are placed. Actors come with the next `continue_build`, from where they
    /// are then: a place prepared ahead of arriving builds this far.
    bool continue_build_static(godot::Node3D* root, std::int64_t budget_usec);

    /// The pack's asset cache, which every model and texture comes from.
    /// Set by SkydotPack::open_world; without it nothing loads.
    void set_assets(std::shared_ptr<AssetCache> assets) { builder_.set_assets(std::move(assets)); }

    /// Virtual paths of the resources (models, land and water textures)
    /// exterior cell (x, y) of `world` needs.
    godot::PackedStringArray get_exterior_resources(std::int64_t world, std::int64_t x,
                                                    std::int64_t y);
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
    /// Shadows on every placed light, not only those whose record asks for
    /// them (LIGH shadow flags; the default). For lights built from now on;
    /// apply_light_shadows changes those already placed.
    void set_all_light_shadows(bool enabled) { builder_.options().all_light_shadows = enabled; }
    bool get_all_light_shadows() const { return builder_.options().all_light_shadows; }
    /// Set the shadows of the lights placed under `root` as the setting
    /// says: all of them, or only those the records flag. Returns how many
    /// changed.
    static std::int64_t apply_light_shadows(godot::Node* root, bool all);
    /// Grass on exterior terrain (GRAS); on by default.
    void set_grass(bool enabled) { builder_.options().grass = enabled; }
    bool get_grass() const { return builder_.options().grass; }

    /// Build placed actors (ACHR) in cells: body, worn outfit and head on
    /// an animated skeleton (see actors.hpp). On by default.
    void set_actors(bool enabled) { builder_.options().actors = enabled; }
    bool get_actors() const { return builder_.options().actors; }
    /// The plan for placed actor `ref` (keys: npc, race, female, scale,
    /// skeleton, idle, parts, missing), for tools and tests.
    godot::Dictionary get_actor_plan(std::int64_t ref) const;
    /// Actors walk around their place while no AI packages are read (see
    /// actor.hpp). On by default; off, they stand in their idle.
    void set_actor_wander(bool enabled) { builder_.options().actor_wander = enabled; }
    bool get_actor_wander() const { return builder_.options().actor_wander; }
    /// The clips that move actors of a behaviour project (keys: idle, walk,
    /// run, each with name, file, playback, speed in game units per second;
    /// missing).
    godot::Dictionary get_locomotion(const godot::String& behaviour);
    /// Build placed actor `ref` alone, at its place; null if it has none.
    godot::Node3D* build_actor(std::int64_t ref);
    /// The placed actors (ACHR refs) of an interior or exterior cell, as
    /// placed in the editor.
    godot::PackedInt64Array get_cell_actors(std::int64_t cell) const;

    /// Where actors are, when not where the editor placed them (SkydotAi
    /// moves them): a space (an interior cell, or a worldspace for the
    /// outside), a position (Skyrim space) and a facing about Z. Cells built
    /// afterwards build an actor where its place is, and not where it was
    /// placed.
    void set_actor_place(std::int64_t ref, std::int64_t space, const godot::Vector3& position,
                         double rotation_z);
    void clear_actor_place(std::int64_t ref);
    void clear_actor_places();
    /// space, interior, cell (the interior, or the exterior cell at the
    /// position; 0 if none), position, rotation_z, moved (whether it is not
    /// the editor's). Empty if `ref` is no placed actor.
    godot::Dictionary get_actor_place(std::int64_t ref) const;
    /// The space of a cell: itself for an interior, its worldspace outside.
    std::int64_t get_cell_space(std::int64_t cell) const;
    /// The nearest point on a navmesh of `space` near `position` (Skyrim
    /// space), or `position` itself if none is within `reach` game units.
    godot::Vector3 nearest_nav_point(std::int64_t space, const godot::Vector3& position,
                                     double reach) const;

    /// Give models and terrain physics bodies (see collision.hpp). On by
    /// default.
    void set_collision(bool enabled);
    bool get_collision() const { return builder_.options().collision; }

    /// Wake frozen clutter under `root` within `radius` metres of `centre`
    /// (Godot space), as when what it rests on is disabled or moves. Returns
    /// how many bodies woke.
    static std::int64_t wake_clutter(godot::Node* root, const godot::Vector3& centre, double radius);

    /// Give cells their navmeshes as navigation regions (see navmesh.hpp). On
    /// by default.
    void set_navigation(bool enabled) { builder_.options().navigation = enabled; }
    bool get_navigation() const { return builder_.options().navigation; }
    /// The cell's navmeshes, as get_navmesh describes them.
    godot::Array get_navmeshes(std::int64_t cell) const;
    /// One navmesh: id, cell, vertices, triangles, water, links and doors (see
    /// navmesh_info in navmesh.hpp). Empty if there is no such navmesh.
    godot::Dictionary get_navmesh(std::int64_t id) const;

    /// Metres per game unit.
    static double unit_scale() { return UNIT_SCALE; }
    /// Skyrim position (game units), rotation (radians) and scale to a Godot
    /// transform.
    static godot::Transform3D skyrim_transform(const godot::Vector3& position,
                                               const godot::Vector3& rotation, double scale);
    /// Skyrim position (game units) to Godot space (metres).
    static godot::Vector3 skyrim_position(const godot::Vector3& position);
    /// Godot space (metres) back to a Skyrim position (game units).
    static godot::Vector3 godot_to_skyrim(const godot::Vector3& position);

    // For the other subsystems (SkydotAi, SkydotWeather); C++ only, not bound.
    /// The open world.fb and its indexes; a closed WorldData until `open`
    /// succeeds.
    const WorldData& data() const { return *data_; }
    /// Where actors are, when not where the editor placed them.
    ActorPlacement& placement() { return placement_; }
    const ActorPlacement& placement() const { return placement_; }
    /// A resource from the cache, else loaded now (and cached).
    godot::Ref<godot::Resource> resource(const godot::String& vpath) const;

protected:
    static void _bind_methods();

private:
    godot::Error fail(godot::Error code, const godot::String& why);

    /// The open world.fb; a closed WorldData until `open` succeeds.
    std::shared_ptr<const WorldData> data_{std::make_shared<const WorldData>()};
    /// Where actors are, when not where the editor placed them.
    ActorPlacement placement_{data_};
    /// Builds cells and holds the settings and caches that go with it.
    CellBuilder builder_{data_, placement_};
    godot::String error_;
};

} // namespace skydot
