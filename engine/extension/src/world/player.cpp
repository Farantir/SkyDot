// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/player.hpp"

#include "world/collision.hpp"

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/cylinder_shape3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/kinematic_collision3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rigid_body3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

using godot::Basis;
using godot::Ref;
using godot::Transform3D;
using godot::Vector3;
using godot::real_t;

namespace skydot {

namespace {

real_t r(double v) { return static_cast<real_t>(v); }

constexpr double k_terminal_speed = 60.0;
// How fast speed approaches the wanted one, per second.
constexpr double k_ground_accel = 12.0;
constexpr double k_air_accel = 1.5;
constexpr double k_water_accel = 4.0;
// While swimming, the chest floats this far under the surface; it leaves the
// water once it is this far above.
constexpr double k_float_depth = 0.1;
constexpr double k_leave_water = 0.25;
// How far past a step's edge the feet go when climbing it.
constexpr godot::real_t k_step_overlap = 0.08F;

} // namespace

void SkydotPlayer::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;
    using godot::PropertyInfo;
    using godot::Variant;

    BIND_ENUM_CONSTANT(WALK);
    BIND_ENUM_CONSTANT(RUN);
    BIND_ENUM_CONSTANT(SPRINT);
    // Physics layer bits (world/collision.hpp), for scripts' queries.
    const godot::StringName self = get_class_static();
    ClassDB::bind_integer_constant(self, "", "LAYER_WORLD", physics_layer::world);
    ClassDB::bind_integer_constant(self, "", "LAYER_CLUTTER", physics_layer::clutter);
    ClassDB::bind_integer_constant(self, "", "LAYER_ACTOR", physics_layer::actor);
    ClassDB::bind_integer_constant(self, "", "LAYER_TERRAIN", physics_layer::terrain);
    ClassDB::bind_integer_constant(self, "", "LAYER_NPC", physics_layer::npc);

    ClassDB::bind_method(D_METHOD("set_input", "move", "vertical", "gait"), &SkydotPlayer::set_input);
    ClassDB::bind_method(D_METHOD("set_look", "yaw", "pitch"), &SkydotPlayer::set_look);
    ClassDB::bind_method(D_METHOD("jump"), &SkydotPlayer::jump);
    ClassDB::bind_method(D_METHOD("teleport", "feet"), &SkydotPlayer::teleport);
    ClassDB::bind_method(D_METHOD("get_eye_position"), &SkydotPlayer::get_eye_position);
    ClassDB::bind_method(D_METHOD("is_swimming"), &SkydotPlayer::is_swimming);
    ClassDB::bind_method(D_METHOD("clear_water"), &SkydotPlayer::clear_water);
    ClassDB::bind_method(D_METHOD("has_water"), &SkydotPlayer::has_water);

#define SKYDOT_PROPERTY(name, type)                                                              \
    ClassDB::bind_method(D_METHOD("set_" #name, "value"), &SkydotPlayer::set_##name);          \
    ClassDB::bind_method(D_METHOD("get_" #name), &SkydotPlayer::get_##name);                    \
    ADD_PROPERTY(PropertyInfo(Variant::type, #name), "set_" #name, "get_" #name)

    SKYDOT_PROPERTY(fly, BOOL);
    SKYDOT_PROPERTY(hold, BOOL);
    SKYDOT_PROPERTY(water_height, FLOAT);
    SKYDOT_PROPERTY(eye_height, FLOAT);
    SKYDOT_PROPERTY(walk_speed, FLOAT);
    SKYDOT_PROPERTY(run_speed, FLOAT);
    SKYDOT_PROPERTY(sprint_speed, FLOAT);
    SKYDOT_PROPERTY(fly_speed, FLOAT);
    SKYDOT_PROPERTY(swim_speed, FLOAT);
    SKYDOT_PROPERTY(jump_height, FLOAT);
    SKYDOT_PROPERTY(step_height, FLOAT);
    SKYDOT_PROPERTY(radius, FLOAT);
    SKYDOT_PROPERTY(height, FLOAT);
#undef SKYDOT_PROPERTY
    ClassDB::bind_method(D_METHOD("get_yaw"), &SkydotPlayer::get_yaw);
}

void SkydotPlayer::_ready() {
    bool shaped = false;
    for (std::int32_t i = 0; i < get_child_count(); ++i) {
        shaped = shaped || godot::Object::cast_to<godot::CollisionShape3D>(get_child(i)) != nullptr;
    }
    if (!shaped) {
        // Flat-bottomed: a capsule's round bottom meets a step's edge at a
        // slant and slides off it instead of standing on it.
        Ref<godot::CylinderShape3D> cylinder;
        cylinder.instantiate();
        cylinder->set_radius(r(radius_));
        cylinder->set_height(r(height_));
        auto* shape = memnew(godot::CollisionShape3D);
        shape->set_name("Body");
        shape->set_shape(cylinder);
        shape->set_position(Vector3(0, r(height_ / 2), 0));
        add_child(shape);
    }
    set_collision_layer(body_layer());
    set_collision_mask(body_mask());
    set_floor_max_angle(r(max_slope_));
    // Long enough to stay on the ground walking down a step.
    set_floor_snap_length(r(step_height_ + 0.05));
    set_floor_constant_speed_enabled(true);
    set_floor_block_on_wall_enabled(true);
    const godot::Variant g =
        godot::ProjectSettings::get_singleton()->get_setting("physics/3d/default_gravity", 9.8);
    gravity_ = static_cast<double>(g);
    previous_ = current_ = get_global_position();
    set_physics_process(true);
}

std::uint32_t SkydotPlayer::body_layer() const {
    return physics_layer::actor;
}

// The player bumps into NPCs; they are not solid to clutter or rays.
std::uint32_t SkydotPlayer::body_mask() const {
    return physics_layer::solid | physics_layer::npc;
}

void SkydotPlayer::set_input(const godot::Vector2& move, double vertical, int gait) {
    move_ = move.limit_length(1);
    vertical_ = std::clamp(vertical, -1.0, 1.0);
    gait_ = std::clamp(gait, static_cast<int>(WALK), static_cast<int>(SPRINT));
}

void SkydotPlayer::set_look(double yaw, double pitch) {
    yaw_ = yaw;
    pitch_ = pitch;
}

void SkydotPlayer::jump() { jump_requested_ = true; }

void SkydotPlayer::teleport(const Vector3& feet) {
    set_global_position(feet);
    set_velocity(Vector3());
    previous_ = current_ = feet;
    swimming_ = false;
}

void SkydotPlayer::set_fly(bool fly) {
    fly_ = fly;
    // Flying passes through everything.
    set_collision_mask(fly ? 0 : body_mask());
    set_velocity(Vector3());
}

void SkydotPlayer::set_water_height(double height) {
    water_height_ = height;
    has_water_ = true;
}

Vector3 SkydotPlayer::get_eye_position() const {
    const double t = godot::Engine::get_singleton()->get_physics_interpolation_fraction();
    return previous_.lerp(current_, r(t)) + Vector3(0, r(eye_height_), 0);
}

Basis SkydotPlayer::look_basis() const {
    return Basis(Vector3(0, 1, 0), r(yaw_)) * Basis(Vector3(1, 0, 0), r(pitch_));
}

void SkydotPlayer::_physics_process(double delta) {
    previous_ = current_;
    if (hold_) {
        set_velocity(Vector3());
        jump_requested_ = false;
        current_ = get_global_position();
        return;
    }
    if (fly_) {
        const Vector3 dir = (look_basis().xform(Vector3(move_.x, 0, -move_.y)) +
                             Vector3(0, r(vertical_), 0))
                                .limit_length(1);
        const double speed = fly_speed_ * (gait_ == SPRINT ? 4.0 : gait_ == WALK ? 0.25 : 1.0);
        set_velocity(dir * r(speed));
        set_global_position(get_global_position() + dir * r(speed * delta));
        jump_requested_ = false;
        current_ = get_global_position();
        return;
    }

    const double chest = static_cast<double>(get_global_position().y) + chest_;
    if (!has_water_) {
        swimming_ = false;
    } else if (swimming_) {
        swimming_ = chest < water_height_ + k_leave_water;
    } else {
        swimming_ = chest < water_height_;
    }
    const Vector3 before = get_velocity();
    if (swimming_) {
        swim(delta);
    } else {
        walk(delta);
    }
    jump_requested_ = false;
    push_bodies(before);
    current_ = get_global_position();
}

void SkydotPlayer::walk(double delta) {
    const Basis yaw(Vector3(0, 1, 0), r(yaw_));
    const Vector3 wish = yaw.xform(Vector3(move_.x, 0, -move_.y));
    const double speed = gait_ == WALK ? walk_speed_ : gait_ == SPRINT ? sprint_speed_ : run_speed_;
    const bool on_floor = is_on_floor();

    Vector3 v = get_velocity();
    const double accel = on_floor ? k_ground_accel : k_air_accel;
    const Vector3 horizontal =
        Vector3(v.x, 0, v.z).lerp(wish * r(speed), r(std::min(1.0, accel * delta)));
    v.x = horizontal.x;
    v.z = horizontal.z;
    if (on_floor && jump_requested_) {
        v.y = r(std::sqrt(2.0 * gravity_ * jump_height_));
    } else if (!on_floor) {
        v.y = r(std::max(static_cast<double>(v.y) - gravity_ * delta, -k_terminal_speed));
    }
    set_velocity(v);

    if (on_floor && v.y <= 0 && step_up(delta)) {
        // Already moved forward and up; slide in place to settle the floor.
        set_velocity(Vector3());
        move_and_slide();
        set_velocity(Vector3(v.x, 0, v.z));
        return;
    }
    move_and_slide();
}

void SkydotPlayer::swim(double delta) {
    Vector3 target = (look_basis().xform(Vector3(move_.x, 0, -move_.y)) +
                      Vector3(0, r(vertical_), 0))
                         .limit_length(1) *
                     r(swim_speed_);
    if (vertical_ == 0.0) {
        // Float with the chest just under the surface.
        const double chest = static_cast<double>(get_global_position().y) + chest_;
        const double rise = std::clamp((water_height_ - k_float_depth - chest) * 2.0, -1.0, 1.0);
        target.y = r(std::max(static_cast<double>(target.y), rise));
    }
    set_velocity(get_velocity().lerp(target, r(std::min(1.0, k_water_accel * delta))));
    move_and_slide();
}

bool SkydotPlayer::step_up(double delta) {
    const Vector3 v = get_velocity();
    const Vector3 motion = Vector3(v.x, 0, v.z) * r(delta);
    if (motion.length_squared() < 1e-8F) {
        return false;
    }
    const Transform3D from = get_global_transform();
    Ref<godot::KinematicCollision3D> hit;
    hit.instantiate();
    if (!test_move(from, motion, hit)) {
        return false; // nothing in the way
    }
    if (static_cast<double>(hit->get_normal().y) >= std::cos(max_slope_)) {
        return false; // a slope it can walk up anyway
    }
    if (godot::Object::cast_to<godot::RigidBody3D>(hit->get_collider()) != nullptr) {
        return false; // loose clutter: push it instead
    }
    // Up as far as there is room, forward, then down onto the step.
    Vector3 rise(0, r(step_height_), 0);
    Ref<godot::KinematicCollision3D> above;
    above.instantiate();
    if (test_move(from, rise, above)) {
        rise = above->get_travel();
    }
    if (rise.y < 0.05F) {
        return false;
    }
    // Far enough onto the step that the flat bottom rests on it rather than
    // on its rounded edge (Jolt rounds convex shapes by their margin).
    const Vector3 dir = motion.normalized();
    Vector3 to_contact = hit->get_position() - from.origin;
    to_contact.y = 0;
    const real_t reach = std::clamp(to_contact.dot(dir) - r(radius_) + k_step_overlap,
                                    motion.length(), r(radius_));
    const Transform3D raised = from.translated(rise);
    const Vector3 forward = dir * reach;
    if (test_move(raised, forward)) {
        return false; // a wall, or a step too shallow to stand on
    }
    const Transform3D moved = raised.translated(forward);
    Ref<godot::KinematicCollision3D> down;
    down.instantiate();
    if (!test_move(moved, Vector3(0, -rise.y - 0.05F, 0), down)) {
        return false; // nothing to stand on
    }
    if (static_cast<double>(down->get_normal().y) < std::cos(max_slope_)) {
        return false;
    }
    const Vector3 landed = moved.origin + down->get_travel();
    if (landed.y - from.origin.y < 0.01F) {
        return false;
    }
    set_global_position(landed);
    return true;
}

void SkydotPlayer::push_bodies(const Vector3& velocity) {
    for (std::int32_t i = 0; i < get_slide_collision_count(); ++i) {
        const Ref<godot::KinematicCollision3D> hit = get_slide_collision(i);
        auto* body = godot::Object::cast_to<godot::RigidBody3D>(hit->get_collider());
        if (body == nullptr) {
            continue;
        }
        const Vector3 normal = hit->get_normal();
        const real_t into = -velocity.dot(normal);
        if (into <= 0) {
            continue;
        }
        // Light things fly off, heavy ones budge.
        const real_t mass = std::min(body->get_mass(), 30.0F);
        if (auto* clutter = godot::Object::cast_to<SkydotDynamicBody>(body)) {
            clutter->wake();
        }
        body->set_sleeping(false);
        body->apply_impulse(-normal * into * mass * 0.4F,
                            hit->get_position() - body->get_global_position());
    }
}

} // namespace skydot
