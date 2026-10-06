// SPDX-License-Identifier: GPL-3.0-or-later
#include "actors/actor.hpp"

#include "render/animator.hpp"
#include "physics/collision.hpp"

#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/navigation_server3d.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
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
/// Doors: how often to look for one ahead (seconds), how far past the body
/// (metres), how long to stand while it swings, and when to close it behind
/// (seconds after opening, metres from its hinge).
constexpr double k_door_probe = 0.2;
constexpr double k_door_reach = 0.8;
constexpr double k_door_wait = 0.8;
constexpr double k_door_close_after = 2.0;
constexpr double k_door_close_distance = 2.0;
/// GetOpenState's values (SkydotPapyrus).
constexpr std::int64_t k_door_open = 1;
constexpr std::int64_t k_door_closed = 3;

/// Level of detail (see actor.hpp). Distances to the camera in metres: a cell
/// is 57.6 m wide, so near is a cell and a bit (what a radius of 1 shows), and
/// the level is left only 10% past the border so that an actor walking along
/// it does not flip back and forth. Seconds between steps of the physics, and
/// between advances of the animation (the clips are sampled at 30 fps).
constexpr double k_lod_near = 80.0;
constexpr double k_lod_far = 160.0;
constexpr double k_lod_hysteresis = 0.1;
constexpr double k_lod_interval = 0.1;
constexpr double k_mid_step = 1.0 / 20.0;
constexpr double k_far_step = 1.0 / 10.0;
/// A standing actor only checks that the ground is still there.
constexpr double k_stand_step = 1.0;
constexpr double k_mid_anim = 1.0 / 15.0;
constexpr double k_far_anim = 1.0 / 7.5;
constexpr double k_mid_ai = 0.2;
constexpr double k_far_ai = 0.5;

/// The placed model a collision object belongs to: its nearest ancestor
/// with a reference.
godot::Node3D* model_of(godot::Object* collider) {
    auto* node = godot::Object::cast_to<godot::Node>(collider);
    for (int depth = 0; node != nullptr && depth < 16; ++depth, node = node->get_parent()) {
        if (node->has_meta("skydot_ref")) {
            return godot::Object::cast_to<godot::Node3D>(node);
        }
    }
    return nullptr;
}

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
    ClassDB::bind_method(D_METHOD("get_lod"), &SkydotActor::get_lod);
    ADD_SIGNAL(godot::MethodInfo("door_toggled", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::INT, "open_state")));
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
    wait_ = static_cast<double>(rng_->randf_range(2.0F, 15.0F));
    // And the work of the levels of detail, so that actors do not all step,
    // animate and look at the camera in the same frame.
    ground_wait_ = static_cast<double>(rng_->randf_range(0.0F, static_cast<float>(k_ground_poll)));
    lod_wait_ = static_cast<double>(rng_->randf_range(0.0F, static_cast<float>(k_lod_interval)));
    step_wait_ = static_cast<double>(rng_->randf_range(0.0F, static_cast<float>(k_far_step)));
    anim_wait_ = static_cast<double>(rng_->randf_range(0.0F, static_cast<float>(k_far_anim)));
    set_process(true);
}

double SkydotActor::ai_interval(std::int32_t lod) {
    return lod <= 0 ? 0.0 : lod == 1 ? k_mid_ai : k_far_ai;
}

godot::AnimationPlayer* SkydotActor::player() const {
    return godot::Object::cast_to<godot::AnimationPlayer>(get_node_or_null("AnimationPlayer"));
}

void SkydotActor::update_lod() {
    const godot::Viewport* viewport = get_viewport();
    const godot::Camera3D* camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
    if (camera == nullptr) {
        lod_ = 0;
        on_screen_ = true;
        return;
    }
    const Vector3 feet = get_global_position();
    const double d = static_cast<double>(feet.distance_to(camera->get_global_position()));
    const double near = lod_ == 0 ? k_lod_near * (1.0 + k_lod_hysteresis) : k_lod_near;
    const double far = lod_ == 2 ? k_lod_far : k_lod_far * (1.0 - k_lod_hysteresis);
    lod_ = d < near ? 0 : d < far ? 1 : 2;
    // On screen: its feet, middle or head are in view (a body's width at the
    // screen's edge is a sliver). Near actors stay as they are: behind the
    // camera they still cast shadows into view.
    const Vector3 up(0, r(get_height()), 0);
    on_screen_ = lod_ == 0 || camera->is_position_in_frustum(feet) ||
                 camera->is_position_in_frustum(feet + up * 0.5F) ||
                 camera->is_position_in_frustum(feet + up);
}

void SkydotActor::animate_lod(double delta) {
    godot::AnimationPlayer* anim = player();
    if (anim == nullptr) {
        return;
    }
    // Near actors' players run themselves; the others are advanced here, at
    // a rate that falls with distance, and not while off screen.
    const bool by_hand = lod_ > 0;
    if (by_hand != manual_) {
        manual_ = by_hand;
        anim->set_callback_mode_process(manual_ ? godot::AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL
                                                : godot::AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_IDLE);
        if (manual_ && !posed_) {
            // Never in view while near: show the clip's first pose at least.
            anim->advance(0.0);
            posed_ = true;
        }
        anim_wait_ = 0.0;
    }
    if (!manual_) {
        posed_ = true;
        return;
    }
    if (!on_screen_) {
        anim_wait_ = 0.0;
        return;
    }
    anim_wait_ += delta;
    if (anim_wait_ >= (lod_ == 1 ? k_mid_anim : k_far_anim)) {
        anim->advance(anim_wait_);
        anim_wait_ = 0.0;
    }
}

void SkydotActor::_process(double delta) {
    lod_wait_ -= delta;
    if (lod_wait_ <= 0.0) {
        lod_wait_ = k_lod_interval;
        update_lod();
    }
    animate_lod(delta);
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
    wake_ = true;
    corner_ = 1;
    run_ = run;
    stuck_time_ = 0.0;
    stuck_from_ = here;
    return true;
}

void SkydotActor::stop() {
    wake_ = true;
    path_.clear();
    corner_ = 0;
    set_input(godot::Vector2(), 0.0, WALK);
}

bool SkydotActor::find_ground(double delta) {
    ground_wait_ -= delta;
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
    wait_ = static_cast<double>(path_.is_empty() ? rng_->randf_range(1.0F, 3.0F) : rng_->randf_range(6.0F, 20.0F));
}

void SkydotActor::steer(double delta) {
    if (path_.is_empty()) {
        set_input(godot::Vector2(), 0.0, WALK);
        return;
    }
    const Vector3 here = get_global_position();
    Vector3 to = path_[corner_] - here;
    to.y = 0;
    // A step of a lower rate may carry it further than the usual reach.
    const Vector3 v = get_velocity();
    const double reach =
        std::max(k_corner_reach, static_cast<double>(Vector3(v.x, 0, v.z).length()) * delta);
    while (static_cast<double>(to.length()) < reach) {
        if (++corner_ >= path_.size()) {
            stop();
            return;
        }
        to = path_[corner_] - here;
        to.y = 0;
    }
    if (door_wait_ > 0.0) {
        door_wait_ -= delta;
        set_input(godot::Vector2(), 0.0, WALK);
        stuck_time_ = 0.0;
        stuck_from_ = here;
        return;
    }
    door_probe_ -= delta;
    if (door_probe_ <= 0.0) {
        door_probe_ = k_door_probe;
        if (open_door_ahead(to.normalized())) {
            door_wait_ = k_door_wait;
            return;
        }
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
            wait_ = static_cast<double>(rng_->randf_range(3.0F, 8.0F));
        }
        stuck_time_ = 0.0;
        stuck_from_ = here;
    }
}

bool SkydotActor::open_door_ahead(const Vector3& dir) {
    auto* space = get_world_3d()->get_direct_space_state();
    if (space == nullptr) {
        return false;
    }
    // Waist high: over thresholds and rugs, under lintels.
    const Vector3 from = get_global_position() + Vector3(0, 1, 0);
    const auto query = godot::PhysicsRayQueryParameters3D::create(
        from, from + dir * r(get_radius() + k_door_reach), physics_layer::world);
    const godot::Dictionary hit = space->intersect_ray(query);
    if (hit.is_empty()) {
        return false;
    }
    godot::Node3D* door = model_of(hit["collider"]);
    if (door == nullptr || !door->has_meta("skydot_plain_door") || door->get_meta("skydot_open", false)) {
        return false;
    }
    auto* animator = godot::Object::cast_to<SkydotAnimator>(door->get_node_or_null("SkydotAnimator"));
    if (animator == nullptr || !animator->play("Open")) {
        return false;
    }
    door->set_meta("skydot_open", true);
    door->set_meta("skydot_opened_by", static_cast<std::int64_t>(get_instance_id()));
    opened_.push_back({door->get_instance_id(), 0.0});
    emit_signal("door_toggled", door->get_meta("skydot_ref"), k_door_open);
    return true;
}

void SkydotActor::close_doors(double delta) {
    const Vector3 here = get_global_position();
    std::erase_if(opened_, [&](OpenedDoor& o) {
        o.time += delta;
        auto* door = godot::Object::cast_to<godot::Node3D>(godot::ObjectDB::get_instance(o.model));
        if (door == nullptr || !door->is_inside_tree()) {
            return true;
        }
        // Someone else closed or took it over meanwhile.
        if (!door->get_meta("skydot_open", false) ||
            static_cast<std::int64_t>(door->get_meta("skydot_opened_by", 0)) !=
                static_cast<std::int64_t>(get_instance_id())) {
            return true;
        }
        Vector3 away = door->get_global_position() - here;
        away.y = 0;
        if (o.time < k_door_close_after || static_cast<double>(away.length()) < k_door_close_distance) {
            return false;
        }
        auto* animator = godot::Object::cast_to<SkydotAnimator>(door->get_node_or_null("SkydotAnimator"));
        if (animator != nullptr && animator->play("Close")) {
            door->set_meta("skydot_open", false);
            emit_signal("door_toggled", door->get_meta("skydot_ref"), k_door_closed);
        }
        return true;
    });
}

void SkydotActor::animate() {
    godot::AnimationPlayer* anim = player();
    if (anim == nullptr) {
        return;
    }
    const Vector3 v = get_velocity();
    const double speed = get_hold() ? 0.0 : static_cast<double>(Vector3(v.x, 0, v.z).length());
    godot::String want = "idle";
    double clip = 0.0;
    if (speed >= k_idle_speed) {
        if (run_ && clip_run_ > 0.0 && anim->has_animation("run")) {
            want = "run";
            clip = clip_run_;
        } else if (anim->has_animation("walk")) {
            want = "walk";
            clip = clip_walk_;
        }
    }
    if (!anim->has_animation(want)) {
        return;
    }
    // Feet keep pace with the ground: the clip plays as fast as the body moves.
    anim->set_speed_scale(clip > 0.0 ? r(std::clamp(speed / clip, 0.5, 2.0)) : 1.0F);
    if (want != playing_ || !anim->is_playing()) {
        anim->play(want, k_blend);
        playing_ = want;
    }
}

void SkydotActor::_physics_process(double engine_delta) {
    // Level of detail: a farther actor steps less often, and then for the
    // time since its last step. A standing one only looks for ground.
    double delta = engine_delta;
    if (lod_ > 0) {
        step_wait_ += engine_delta;
        const Vector3 v = get_velocity();
        const bool standing = !get_hold() && path_.is_empty() && is_on_floor() &&
                              static_cast<double>(Vector3(v.x, 0, v.z).length()) < k_idle_speed;
        const double every = standing ? k_stand_step : lod_ == 1 ? k_mid_step : k_far_step;
        if (step_wait_ < every && !wake_) {
            return;
        }
        delta = step_wait_;
    }
    step_wait_ = 0.0;
    wake_ = false;
    if (get_hold()) {
        if (find_ground(delta)) {
            set_hold(false);
        }
    } else {
        think(delta);
        steer(delta);
    }
    close_doors(delta);
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
