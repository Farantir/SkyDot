// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotStreamer`: what keeps a worldspace around the camera, for every
// front end, and builds the place behind a load door ahead.
//
// Streaming (streamer.cpp). Every frame it drops exterior cells out of range
// (the radius plus one), continues the builds under way nearest first and
// begins the nearest cells in range whose resources the asset cache has
// loaded, all within `build_budget_usec`: a cell is built in steps, hidden
// until it is done, then shown, masked out of the worldspace's LOD, announced
// (`cell_finished`) and its AI actors attached. The LOD runs beyond the cells.
//
// Preloading (preload.cpp). Near a load door the place behind it (an interior,
// or the cells and LOD around the arrival spot outside) is built a little each
// frame while nothing streams here: the destination's resources on the
// loader's threads, then its references (not its actors) in the scene but
// held (held_place.hpp). Going through that door then only adds the actors.
// One place is kept; walking away from the door drops it. It costs the memory
// of a second place.
//
// The front end calls `update` and `preload_step` once a frame, in that
// order, and enters places itself: it leaves the old one (`clear`), starts
// the worldspace (`start`), hands over the LOD and what was prepared
// (`lod`, `adopt`).
#pragma once

#include "ai/ai.hpp"
#include "assets/pack.hpp"
#include "physics/player.hpp"
#include "render/lod.hpp"
#include "world/world.hpp"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace skydot {

/// A cell's grid square (x, y) in a worldspace.
using CellKey = std::pair<std::int32_t, std::int32_t>;

/// The place behind a load door, built ahead, and how far along it is.
class SkydotPreparation : public godot::RefCounted {
    GDCLASS(SkydotPreparation, godot::RefCounted)

public:
    /// The door it was started for, and the door on the other side.
    std::int64_t get_door() const { return door_; }
    std::int64_t get_destination() const { return destination_; }
    /// Everything that can be built ahead is.
    bool is_done() const { return done_; }
    bool is_interior() const { return interior_; }
    /// An interior's root (null until begun), or an exterior's LOD.
    godot::Node3D* get_root() const;
    SkydotLod* get_lod() const;

protected:
    static void _bind_methods();

private:
    friend class SkydotStreamer;

    std::int64_t door_ = 0;
    std::int64_t destination_ = 0;
    bool done_ = false;
    std::uint64_t started_usec_ = 0;
    /// An interior: its record and its root (instance id; 0 until begun).
    bool interior_ = false;
    std::int64_t cell_ = 0;
    std::uint64_t root_ = 0;
    /// An exterior: the worldspace, the camera's eye on arrival, the cells to
    /// build nearest first, those begun (instance id; 0 where nothing is),
    /// those whose references are all placed, and the LOD (instance id).
    std::int64_t world_ = 0;
    godot::Vector3 eye_;
    std::vector<CellKey> keys_;
    std::map<CellKey, std::uint64_t> cells_;
    std::set<CellKey> placed_;
    std::uint64_t lod_ = 0;
};

class SkydotStreamer : public godot::RefCounted {
    GDCLASS(SkydotStreamer, godot::RefCounted)

public:
    static constexpr std::int64_t LOD_BUDGET_USEC = 3000;     ///< Per frame for the LOD.
    static constexpr std::int64_t PRELOAD_BUDGET_USEC = 4000; ///< Per frame for the place behind a door.
    static constexpr int PRELOAD_SCAN_FRAMES = 15;            ///< Between looks for the nearest door.

    /// Cells are added to `host`, around `camera`'s cell. `ai` (may be null)
    /// gets the actors of cells built.
    void setup(const godot::Ref<SkydotWorld>& world, const godot::Ref<SkydotPack>& pack,
               godot::Node3D* host, godot::Camera3D* camera, const godot::Ref<SkydotAi>& ai);

    // ---- Options ----------------------------------------------------------

    /// Cells around the camera built in full.
    void set_radius(std::int64_t radius) { radius_ = radius; }
    std::int64_t get_radius() const { return radius_; }
    /// Microseconds per frame for streaming cells in.
    void set_build_budget_usec(std::int64_t usec) { build_budget_usec_ = usec; }
    std::int64_t get_build_budget_usec() const { return build_budget_usec_; }
    /// SkydotLod.split_distance for every LOD made.
    void set_lod_split(double split) { lod_split_ = split; }
    double get_lod_split() const { return lod_split_; }
    /// No LOD is made when off.
    void set_lod_enabled(bool enabled) { lod_enabled_ = enabled; }
    bool get_lod_enabled() const { return lod_enabled_; }
    /// SkydotLod.tree_distance for every LOD made; unset keeps the LOD's own.
    void set_tree_distance(double cells) { tree_distance_ = cells; }
    /// Metres from the feet to the eyes, for the camera's place on arrival.
    void set_eye_height(double metres) { eye_height_ = metres; }
    double get_eye_height() const { return eye_height_; }
    /// Build ahead at all, and how near the feet must be to a load door, in
    /// metres.
    void set_preload_enabled(bool enabled) { preload_enabled_ = enabled; }
    bool get_preload_enabled() const { return preload_enabled_; }
    void set_preload_distance(double metres) { preload_distance_ = metres; }
    double get_preload_distance() const { return preload_distance_; }

    // ---- State ------------------------------------------------------------

    /// The worldspace being streamed, or 0 inside.
    std::int64_t get_world_id() const { return world_id_; }
    /// Exterior cells in range were still loading at the last update.
    bool is_streaming() const { return streaming_; }
    /// The LOD still had work queued at the last update.
    bool is_lod_busy() const { return lod_busy_; }
    /// The slowest update in microseconds, for the benchmark.
    void set_max_usec(std::int64_t usec) { max_usec_ = usec; }
    std::int64_t get_max_usec() const { return max_usec_; }
    /// The worldspace's LOD (the front end adds it to the scene), or null.
    void set_lod(SkydotLod* lod);
    SkydotLod* get_lod() const;
    /// The cell built at grid square `key`, or null (not loaded, or nothing
    /// there).
    godot::Node3D* get_loaded_cell(const godot::Vector2i& key) const;
    /// Every cell built.
    godot::Array get_loaded_cells() const;

    // ---- Streaming (streamer.cpp) ------------------------------------------

    /// The grid square of a position in the engine's space.
    static godot::Vector2i cell_at(const godot::Vector3& position);
    /// The camera's cell in the worldspace grid.
    godot::Vector2i camera_cell() const;
    /// How far the camera sees without LOD: past the cells in range.
    double view_distance() const;

    /// Stream `id` from now on (after clear).
    void start(std::int64_t id);
    /// Free every cell and forget the worldspace and its load doors (its LOD
    /// is freed with the place).
    void clear();
    /// The worldspace's LOD, set up as the options say; null if it has none
    /// or the LOD is off. Not in the scene.
    SkydotLod* make_lod(std::int64_t id);
    /// Cells built ahead join the streaming ones; each is released when it is
    /// finished, so their cost is spread over frames, but the one under the
    /// camera at once, for the ground. Null cells are known to be empty.
    void adopt(const godot::Ref<SkydotPreparation>& prepared);

    /// Every frame in a worldspace: stream, then the LOD.
    void update();
    /// Build everything in range and the whole LOD now, waiting for the
    /// loaders (a screenshot or benchmark wants a complete world).
    void load_everything();
    /// Drop cells out of range, then load cells in range nearest first.
    /// Returns false when every cell in range is loaded.
    bool step();

    /// Hold the player while the ground under it is still being built (and
    /// while `fading` through a door), and give it the water level of the
    /// cell it is in.
    void hold_player(SkydotPlayer* player, bool fading);
    /// Finer or coarser LOD: the split distance times `factor`, within
    /// limits. Returns the new one.
    double scale_lod_split(double factor);
    /// More or fewer cells in full detail, within limits. Returns the new
    /// radius.
    std::int64_t change_radius(std::int64_t by);

    // ---- Preloading (preload.cpp) -------------------------------------------

    /// Remember the load doors among the references of `cell`, just built.
    void register_doors(godot::Node* cell);
    /// Door reference -> its node, in the place shown.
    godot::Dictionary get_load_doors() const;
    /// O: returns true when building ahead.
    bool toggle_preload();
    /// Every frame: look for the nearest door now and then, and build a
    /// little of the place behind it. `feet` is the player's position.
    void preload_step(const godot::Vector3& feet);
    /// The place behind the nearest load door; null if none is being built.
    godot::Ref<SkydotPreparation> get_preparation() const { return current_; }
    /// Going through `door` (get_door): what was built ahead for it, handed
    /// over (null if nothing, or for another door); the rest is dropped.
    godot::Ref<SkydotPreparation> take_prepared(const godot::Dictionary& door);
    /// Free what was built ahead.
    void drop_prepared();
    /// Undo what holding a prepared place did, on arrival.
    static void release_held(godot::Node3D* node);

protected:
    static void _bind_methods();

private:
    godot::Node3D* host() const;
    godot::Camera3D* camera() const;
    void finish_cell(const CellKey& key, godot::Node3D* cell);
    godot::Ref<SkydotPreparation> begin_preparation(const godot::Dictionary& door);
    void advance_preparation(std::int64_t budget_usec);

    godot::Ref<SkydotWorld> world_;
    godot::Ref<SkydotPack> pack_;
    godot::Ref<SkydotAi> ai_; ///< Null with the AI off.
    std::uint64_t host_ = 0;  ///< Instance ids: the nodes belong to the scene.
    std::uint64_t camera_ = 0;

    std::int64_t radius_ = 2;
    std::int64_t build_budget_usec_ = 8000;
    double lod_split_ = 1.5;
    bool lod_enabled_ = true;
    std::optional<double> tree_distance_;
    double eye_height_ = 1.7;
    bool preload_enabled_ = true;
    double preload_distance_ = 15.0;

    std::int64_t world_id_ = 0;
    std::uint64_t lod_ = 0;
    /// Cells built, by grid square (0: known to be empty), and those built in
    /// steps, hidden until done, in the order they were begun.
    std::map<CellKey, std::uint64_t> loaded_;
    std::vector<std::pair<CellKey, std::uint64_t>> building_;
    bool streaming_ = false;
    bool lod_busy_ = false;
    std::int64_t max_usec_ = 0;

    /// Load door reference -> its node, in the order they were found.
    std::vector<std::pair<std::uint32_t, std::uint64_t>> load_doors_;
    godot::Ref<SkydotPreparation> current_;
    int scan_ = 0; ///< Frames until looking for the nearest door again.
};

} // namespace skydot
