// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotPlayer`: a walking character on Godot's physics (Jolt).
//
// The body's origin is at the feet; a cylinder stands on it. Input is set
// each frame (`set_input`, `set_look`, `jump`) and applied on physics ticks:
// accelerate towards walk, run or sprint speed, fall, jump, climb steps up to
// `step_height` (CharacterBody3D cannot), swim when the chest is under
// `water_height`, and push clutter it walks into. `fly` ignores collision and
// moves where the camera looks. `hold` freezes it, for while the ground under
// it is still loading.
//
// The camera should follow `get_eye_position`, which interpolates between
// physics ticks so the view does not judder at high frame rates.
#pragma once

#include <godot_cpp/classes/character_body3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace skydot {

class SkydotPlayer : public godot::CharacterBody3D {
    GDCLASS(SkydotPlayer, godot::CharacterBody3D)

public:
    enum Gait { WALK, RUN, SPRINT };

    /// `move`: x right, y forward, each -1 to 1. `vertical`: up (1) or down
    /// (-1) while flying or swimming.
    void set_input(const godot::Vector2& move, double vertical, int gait);
    /// Yaw about +Y and pitch (radians), as the camera's rotation.
    void set_look(double yaw, double pitch);
    /// Jump on the next tick, if standing.
    void jump();
    /// A shove: `velocity` (metres per second, Godot space) is added to the body's own; the walk's
    /// acceleration takes it out again (a shout's push).
    void impulse(const godot::Vector3& velocity) { set_velocity(get_velocity() + velocity); }
    /// Put the feet at `feet` with no velocity.
    void teleport(const godot::Vector3& feet);
    /// The eye, interpolated between the last two physics ticks.
    godot::Vector3 get_eye_position() const;
    bool is_swimming() const { return swimming_; }
    /// Moving at the sprint gait on the ground (not flying or swimming), and
    /// sprinting is allowed: the game drains Stamina meanwhile.
    bool is_sprinting() const;
    /// False makes the SPRINT gait run (Stamina is out).
    void set_sprint_allowed(bool allowed) { sprint_allowed_ = allowed; }
    bool get_sprint_allowed() const { return sprint_allowed_; }

    void set_fly(bool fly);
    bool get_fly() const { return fly_; }
    void set_hold(bool hold) { hold_ = hold; }
    bool get_hold() const { return hold_; }
    /// Water surface height in Godot space; `clear_water` for none.
    void set_water_height(double height);
    double get_water_height() const { return water_height_; }
    void clear_water() { has_water_ = false; }
    bool has_water() const { return has_water_; }

    void set_eye_height(double v) { eye_height_ = v; }
    double get_eye_height() const { return eye_height_; }
    void set_walk_speed(double v) { walk_speed_ = v; }
    double get_walk_speed() const { return walk_speed_; }
    void set_run_speed(double v) { run_speed_ = v; }
    double get_run_speed() const { return run_speed_; }
    void set_sprint_speed(double v) { sprint_speed_ = v; }
    double get_sprint_speed() const { return sprint_speed_; }
    void set_fly_speed(double v) { fly_speed_ = v; }
    double get_fly_speed() const { return fly_speed_; }
    void set_swim_speed(double v) { swim_speed_ = v; }
    double get_swim_speed() const { return swim_speed_; }
    /// Swimming at the walk gait.
    void set_swim_walk_speed(double v) { swim_walk_speed_ = v; }
    double get_swim_walk_speed() const { return swim_walk_speed_; }
    /// Backwards speed as a fraction of the forward speed (walk and run gait; MOVT back / forward).
    void set_back_walk_ratio(double v) { back_walk_ratio_ = v; }
    double get_back_walk_ratio() const { return back_walk_ratio_; }
    void set_back_run_ratio(double v) { back_run_ratio_ = v; }
    double get_back_run_ratio() const { return back_run_ratio_; }
    void set_jump_height(double v) { jump_height_ = v; }
    double get_jump_height() const { return jump_height_; }
    void set_step_height(double v) { step_height_ = v; }
    double get_step_height() const { return step_height_; }
    /// The body's cylinder; read when the node enters the tree.
    void set_radius(double v) { radius_ = v; }
    double get_radius() const { return radius_; }
    void set_height(double v) { height_ = v; }
    double get_height() const { return height_; }
    double get_yaw() const { return yaw_; }

    void _ready() override;
    void _physics_process(double delta) override;

protected:
    static void _bind_methods();

    /// The layers this body is on and collides with; NPCs differ.
    virtual std::uint32_t body_layer() const;
    virtual std::uint32_t body_mask() const;

private:
    void walk(double delta);
    double back_factor(double ratio) const;
    void swim(double delta);
    /// Climb a step in the way of this tick's motion. True if it did.
    bool step_up(double delta);
    void push_bodies(const godot::Vector3& velocity);
    godot::Basis look_basis() const;

    // Defaults: the body is about the game's (128 units tall, eyes at 120);
    // speeds are the game's for a human (NPC_Default_MT walk 80.1 and run 370
    // units per second, NPC_Sprinting_MT 500, back 71.9 and 205) and the
    // jump is fJumpHeightMin (76 units); the player rig sets the race's.
    double radius_{0.3};
    double height_{1.83};
    double eye_height_{1.7};
    double walk_speed_{1.14};
    double run_speed_{5.29};
    double sprint_speed_{7.14};
    double fly_speed_{10.0};
    double swim_speed_{5.29};
    double swim_walk_speed_{1.14};
    double back_walk_ratio_{0.898};
    double back_run_ratio_{0.554};
    double jump_height_{1.086};
    double step_height_{0.45};
    double max_slope_{0.873}; ///< 50 degrees
    double chest_{1.25};      ///< Above the feet; swimming starts when it is under water.
    double gravity_{9.8};

    godot::Vector2 move_;
    double vertical_{0.0};
    int gait_{RUN};
    bool sprint_allowed_{true};
    int effective_gait() const { return gait_ == SPRINT && !sprint_allowed_ ? RUN : gait_; }
    double yaw_{0.0};
    double pitch_{0.0};
    bool jump_requested_{false};
    bool fly_{false};
    bool hold_{false};
    bool swimming_{false};
    bool has_water_{false};
    double water_height_{0.0};
    godot::Vector3 previous_; ///< Feet at the last two ticks.
    godot::Vector3 current_;
};

} // namespace skydot

VARIANT_ENUM_CAST(skydot::SkydotPlayer::Gait);
