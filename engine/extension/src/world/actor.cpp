// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/actor.hpp"

#include "world/collision.hpp"

#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/navigation_server3d.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using godot::Vector3;
using godot::real_t;

namespace skydot {
namespace {

real_t r(double v) { return static_cast<real_t>(v); }

constexpr double k_tau = 2.0 * std::numbers::pi;
/// Radians per second.
constexpr double k_turn_rate = 5.0;
/// A corner counts as reached this close (metres, horizontally).
constexpr double k_corner_reach = 0.35;
/// Farther than this off the navmesh, a walk does not start.
constexpr double k_off_mesh = 1.5;
/// Less than this progress in `k_stuck_window` seconds gives up a walk.
constexpr double k_stuck_distance = 0.2;
constexpr double k_stuck_window = 1.5;
/// Below this horizontal speed the actor idles.
constexpr double k_idle_speed = 0.15;
/// Seconds between looks for ground while held.
constexpr double k_ground_poll = 0.25;
/// Falling this far below home puts it back there.
constexpr double k_lost_depth = 30.0;
constexpr double k_blend = 0.25;

double wrap(double a) {
    a = std::fmod(a + std::numbers::pi, k_tau);
    if (a < 0) {
        a += k_tau;
    }
    return a - std::numbers::pi;
}

} // namespace

void SkydotActor::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;
    using godot::PropertyInfo;
    using godot::Variant;
    ClassDB::bind_method(D_METHOD("walk_to", "target", "run"), &SkydotActor::walk_to);
    ClassDB::bind_method(D_METHOD("stop"), &SkydotActor::stop);
    ClassDB::bind_method(D_METHOD("is_walking"), &SkydotActor::is_walking);
    ClassDB::bind_method(D_METHOD("get_path"), &SkydotActor::get_path);
    ClassDB::bind_method(D_METHOD("get_state"), &SkydotActor::get_state);
    ClassDB::bind_method(D_METHOD("set_clip_speeds", "walk", "run"), &SkydotActor::set_clip_speeds);
    ClassDB::bind_method(D_METHOD("set_seed", "seed"), &SkydotActor::set_seed);
    ClassDB::bind_method(D_METHOD("face", "yaw"), &SkydotActor::face);
    ClassDB::bind_method(D_METHOD("set_home", "value"), &SkydotActor::set_home);
    ClassDB::bind_method(D_METHOD("get_home"), &SkydotActor::get_home);
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "home"), "set_home", "get_home");
    ClassDB::bind_method(D_METHOD("set_wander", "value"), &SkydotActor::set_wander);
    ClassDB::bind_method(D_METHOD("get_wander"), &SkydotActor::get_wander);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wander"), "set_wander", "get_wander");
    ClassDB::bind_method(D_METHOD("set_wander_radius", "value"), &SkydotActor::set_wander_radius);
    ClassDB::bind_method(D_METHOD("get_wander_radius"), &SkydotActor::get_wander_radius);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wander_radius"), "set_wander_radius", "get_wander_radius");
}

std::uint32_t SkydotActor::body_layer() const {
    return physics_layer::npc;
}

std::uint32_t SkydotActor::body_mask() const {
    return physics_layer::solid | physics_layer::actor | physics_layer::npc;
}

void SkydotActor::set_seed(std::int64_t seed) {
    if (rng_.is_null()) {
        rng_.instantiate();
    }
    rng_->set_seed(static_cast<std::uint64_t>(seed));
}

void SkydotActor::set_clip_speeds(double walk, double run) {
    clip_walk_ = walk;
    clip_run_ = run;
    if (walk > 0) {
        set_walk_speed(walk);
    }
    // Without a run clip it walks wherever it would run.
    set_run_speed(run > 0 ? run : walk);
}

void SkydotActor::face(double yaw) {
    facing_ = wrap(yaw);
    set_look(facing_, 0.0);
    set_rotation(Vector3(0, r(facing_), 0));
}

void SkydotActor::_ready() {
    if (rng_.is_null()) {
        set_seed(static_cast<std::int64_t>(get_instance_id()));
    }
    // The body takes the node's facing; the cylinder does not care.
    const Vector3 forward = get_global_transform().basis.xform(Vector3(0, 0, -1));
    SkydotPlayer::_ready();
    face(std::atan2(-static_cast<double>(forward.x), -static_cast<double>(forward.z)));
    if (!home_set_) {
        set_home(get_global_position());
    }
    set_hold(true);
    // Spread the first walks out.
    wait_ = rng_->randf_range(2.0F, 15.0F);
}

godot::String SkydotActor::get_state() const {
    if (get_hold()) {
        return "held";
    }
    if (path_.is_empty()) {
        return "idle";
    }
    return run_ ? "run" : "walk";
}

bool SkydotActor::walk_to(const Vector3& target, bool run) {
    path_.clear();
    if (!is_inside_tree()) {
        return false;
    }
    auto* nav = godot::NavigationServer3D::get_singleton();
    const godot::RID map = get_world_3d()->get_navigation_map();
    if (nav == nullptr || !map.is_valid() || nav->map_get_iteration_id(map) == 0) {
        return false;
    }
    const Vector3 here = get_global_position();
    const Vector3 from = nav->map_get_closest_point(map, here);
    if (from.distance_to(here) > r(k_off_mesh)) {
        return false;
    }
    const Vector3 to = nav->map_get_closest_point(map, target);
    godot::PackedVector3Array path = nav->map_get_path(map, from, to, true);
    if (path.size() < 2) {
        return false;
    }
    path_ = path;
    corner_ = 1;
    run_ = run;
    stuck_time_ = 0.0;
    stuck_from_ = here;
    return true;
}

void SkydotActor::stop() {
    path_.clear();
    corner_ = 0;
    set_input(godot::Vector2(), 0.0, WALK);
}

bool SkydotActor::find_ground() {
    ground_wait_ -= get_physics_process_delta_time();
    if (ground_wait_ > 0.0 || !is_inside_tree()) {
        return false;
    }
    ground_wait_ = k_ground_poll;
    auto* space = get_world_3d()->get_direct_space_state();
    if (space == nullptr) {
        return false;
    }
    const Vector3 feet = get_global_position();
    const auto query = godot::PhysicsRayQueryParameters3D::create(
        feet + Vector3(0, r(get_step_height() + 0.1), 0), feet - Vector3(0, 4, 0),
        physics_layer::world | physics_layer::terrain);
    const godot::Dictionary hit = space->intersect_ray(query);
    if (hit.is_empty()) {
        return false;
    }
    const Vector3 ground = hit["position"];
    teleport(ground);
    return true;
}

void SkydotActor::think(double delta) {
    if (!wander_ || clip_walk_ <= 0.0 || !path_.is_empty()) {
        return;
    }
    wait_ -= delta;
    if (wait_ > 0.0) {
        return;
    }
    // A point within the radius, uniform over the disc.
    const double angle = static_cast<double>(rng_->randf()) * k_tau;
    const double dist = std::sqrt(static_cast<double>(rng_->randf())) * wander_radius_;
    const Vector3 target = home_ + Vector3(r(std::cos(angle) * dist), 0, r(std::sin(angle) * dist));
    if (walk_to(target, false)) {
        // Not round the houses: a path much longer than the radius is not a stroll.
        double length = 0.0;
        for (std::int64_t i = 1; i < path_.size(); ++i) {
            length += static_cast<double>(path_[i].distance_to(path_[i - 1]));
        }
        if (length > 3.0 * wander_radius_ || length < 1.0) {
            stop();
        }
    }
    wait_ = path_.is_empty() ? rng_->randf_range(1.0F, 3.0F) : rng_->randf_range(6.0F, 20.0F);
}

void SkydotActor::steer(double delta) {
    if (path_.is_empty()) {
        set_input(godot::Vector2(), 0.0, WALK);
        return;
    }
    const Vector3 here = get_global_position();
    Vector3 to = path_[corner_] - here;
    to.y = 0;
    while (static_cast<double>(to.length()) < k_corner_reach) {
        if (++corner_ >= path_.size()) {
            stop();
            return;
        }
        to = path_[corner_] - here;
        to.y = 0;
    }
    const double want = std::atan2(-static_cast<double>(to.x), -static_cast<double>(to.z));
    const double diff = wrap(want - facing_);
    const double step = k_turn_rate * delta;
    face(facing_ + std::clamp(diff, -step, step));
    // Turn on the spot when the corner is behind, ease in when it is to the side.
    const double ahead = std::abs(diff) < std::numbers::pi / 2 ? std::cos(diff) : 0.0;
    set_input(godot::Vector2(0, r(ahead)), 0.0, run_ ? RUN : WALK);

    stuck_time_ += delta;
    if (stuck_time_ >= k_stuck_window) {
        if (static_cast<double>(here.distance_to(stuck_from_)) < k_stuck_distance) {
            stop();
            wait_ = rng_->randf_range(3.0F, 8.0F);
        }
        stuck_time_ = 0.0;
        stuck_from_ = here;
    }
}

void SkydotActor::animate() {
    auto* player = godot::Object::cast_to<godot::AnimationPlayer>(get_node_or_null("AnimationPlayer"));
    if (player == nullptr) {
        return;
    }
    const Vector3 v = get_velocity();
    const double speed = get_hold() ? 0.0 : static_cast<double>(Vector3(v.x, 0, v.z).length());
    godot::String want = "idle";
    double clip = 0.0;
    if (speed >= k_idle_speed) {
        if (run_ && clip_run_ > 0.0 && player->has_animation("run")) {
            want = "run";
            clip = clip_run_;
        } else if (player->has_animation("walk")) {
            want = "walk";
            clip = clip_walk_;
        }
    }
    if (!player->has_animation(want)) {
        return;
    }
    // Feet keep pace with the ground: the clip plays as fast as the body moves.
    player->set_speed_scale(clip > 0.0 ? r(std::clamp(speed / clip, 0.5, 2.0)) : 1.0F);
    if (want != playing_ || !player->is_playing()) {
        player->play(want, k_blend);
        playing_ = want;
    }
}

void SkydotActor::_physics_process(double delta) {
    if (get_hold()) {
        if (find_ground()) {
            set_hold(false);
        }
    } else {
        think(delta);
        steer(delta);
    }
    SkydotPlayer::_physics_process(delta);
    if (!get_hold() && get_global_position().y < home_.y - r(k_lost_depth)) {
        // Fell through something not built yet: back home, wait for ground.
        stop();
        teleport(home_);
        set_hold(true);
    }
    animate();
}

} // namespace skydot
