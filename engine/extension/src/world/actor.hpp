// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotActor`: a placed actor that walks. It is a `SkydotPlayer` (the same
// cylinder, gravity, slopes and step-up) steered by itself instead of by
// input: `walk_to` follows a path on the navigation map, turning towards
// each corner, and its AnimationPlayer child plays `idle`, `walk` or `run`
// with the playback speed matched to how fast it actually moves, so feet do
// not slide.
//
// Until AI packages are read, `wander` stands in for them: the actor idles a
// while, then walks to a random point on the navmesh near its home (where
// the cell placed it), and idles again. Deterministic per reference.
//
// It holds still until there is ground under it (cells stream their
// collision in after their actors), and gives up a walk when it stops making
// progress (another actor, a ledge the path did not know). A closed plain
// door in its way it opens, waits for, and closes again once past it
// (`door_toggled` tells, for SkydotPapyrus's open state).
#pragma once

#include "world/player.hpp"

#include <godot_cpp/classes/random_number_generator.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <vector>

namespace skydot {

class SkydotActor : public SkydotPlayer {
    GDCLASS(SkydotActor, SkydotPlayer)

public:
    /// Walk (or run) to `target` along the navigation map. False when no
    /// path leads there (yet): the map may still be synchronizing.
    bool walk_to(const godot::Vector3& target, bool run);
    void stop();
    bool is_walking() const { return !path_.is_empty(); }
    godot::PackedVector3Array get_path() const { return path_; }
    /// "held", "idle", "walk" or "run".
    godot::String get_state() const;

    /// Clip speeds in metres per second at playback speed 1 (0: no clip).
    void set_clip_speeds(double walk, double run);

    void set_home(const godot::Vector3& v) {
        home_ = v;
        home_set_ = true;
    }
    godot::Vector3 get_home() const { return home_; }
    void set_wander(bool v) { wander_ = v; }
    bool get_wander() const { return wander_; }
    void set_wander_radius(double v) { wander_radius_ = v; }
    double get_wander_radius() const { return wander_radius_; }
    void set_seed(std::int64_t seed);
    /// Facing about +Y, radians, as `SkydotPlayer.set_look` takes it.
    void face(double yaw);

    void _ready() override;
    void _physics_process(double delta) override;

protected:
    static void _bind_methods();
    std::uint32_t body_layer() const override;
    std::uint32_t body_mask() const override;

private:
    void think(double delta);
    void steer(double delta);
    void animate();
    bool find_ground();
    /// Open a closed plain door within reach along `dir`; true if it did.
    bool open_door_ahead(const godot::Vector3& dir);
    /// Close the doors it opened once it is past them.
    void close_doors(double delta);

    godot::PackedVector3Array path_;
    std::int32_t corner_{0};
    bool run_{false};
    bool wander_{true};
    double wander_radius_{7.3}; ///< 512 game units, the CK's default sandbox radius.
    double wait_{0.0};          ///< Idle time left before the next wander.
    double facing_{0.0};
    double clip_walk_{0.0};
    double clip_run_{0.0};
    double stuck_time_{0.0};
    godot::Vector3 stuck_from_;
    godot::Vector3 home_;
    bool home_set_{false};
    double ground_wait_{0.0};
    godot::String playing_;
    double door_probe_{0.0}; ///< Seconds before the next look for a door.
    double door_wait_{0.0};  ///< Seconds left standing while a door opens.
    struct OpenedDoor {
        std::uint64_t model{}; ///< The door's instance id.
        double time{};         ///< Seconds since it opened it.
    };
    std::vector<OpenedDoor> opened_;
    godot::Ref<godot::RandomNumberGenerator> rng_;
};

} // namespace skydot
