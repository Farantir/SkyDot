// SPDX-License-Identifier: GPL-3.0-or-later
//
// A kinematic flight mover for dragons (GAMEPLAY-PLAN.md, step 7b-6), without
// Godot, the pack or the clock. The flight clips carry no root motion, so the
// mover moves the body and the clips only animate it: it takes a Command (fly
// there, orbit that, hover facing it, pass over it, land here, crash) and
// integrates position, heading, speed and climb with limited turn rate, yaw
// acceleration and climb rate, never below a clearance over the highest
// surface (terrain or static) under and ahead of it. Tactics is the combat
// flight choice (take off, orbit, hover, pass, land) with injected dice.
//
// Coordinates are Skyrim's (units, z up). Heading ("yaw", radians) is Skyrim's:
// 0 faces +y (north) and it grows clockwise seen from above, so a heading faces
// (sin yaw, cos yaw) and a positive yaw rate is a right turn. Bank is positive
// with the left wing down (a left turn), so a right turn banks negative. The
// game layer converts to Godot. Numbers marked "data" come from the race, the
// movement type or the combat style; the others are placeholders in Params.
#pragma once

#include <cmath>
#include <functional>
#include <optional>
#include <string>

namespace skydot::flight {

struct Vec3 {
    double x{};
    double y{};
    double z{};
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
/// Horizontal (x, y) distance.
inline double distance_xy(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }
inline double distance(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}
/// The heading (radians, Skyrim convention) that faces from `from` to `to`.
inline double heading_to(const Vec3& from, const Vec3& to) { return std::atan2(to.x - from.x, to.y - from.y); }
/// An angle wrapped into [-pi, pi].
double wrap_pi(double a);

/// The facing of a Godot body (radians about +Y, 0 looking down -Z, as SkydotActor::face takes it)
/// for a Skyrim heading, and back: Godot's z is Skyrim's -y, so the turn changes sense.
inline double facing_from_heading(double heading) { return wrap_pi(-heading); }
inline double heading_from_facing(double facing) { return wrap_pi(-facing); }

/// The highest surface (terrain or static) at a point; NaN when unknown, where
/// the mover keeps the last height it knew.
using Ground = std::function<double(double x, double y)>;

struct Params {
    // Speeds, units per second.
    double cruise_speed = 2000;      ///< data: FLMV Dragon_Flying_MT forward walk.
    double transit_speed = 7400;     ///< data: its run; only when the goal is far.
    double transit_distance = 20000; ///< placeholder: farther than this, fly at transit speed.
    double pass_speed = 1200;        ///< placeholder: a breath pass.
    double acceleration = 600;       ///< placeholder, u/s^2.
    double deceleration = 900;       ///< placeholder, u/s^2.
    // Turning.
    double max_turn_rate = 1.5708;     ///< data: FLMV rotate, rad/s.
    double angular_acceleration = 0.25; ///< data: race, read as rad/s^2.
    double angular_accel_scale = 2.0;   ///< placeholder: 0.25 rad/s^2 alone needs 6 s to reverse a turn.
    double angular_tolerance = 0.0873;  ///< data: race (5 degrees), rad; "aligned".
    double hover_turn_rate = 0.8;       ///< placeholder: turning on the spot, rad/s.
    double aim_tolerance = 0.7287;      ///< data: race aim angle tolerance 41.75 degrees, rad.
    // Altitude.
    double cruise_height = 1500;  ///< placeholder: above the ground under it.
    double placed_in_air = 1000;  ///< placeholder: a flyer placed this far over the ground starts in flight.
    double min_clearance = 400;   ///< max(race flight radius, 400) above the highest surface.
    double max_climb = 800;       ///< placeholder, u/s.
    double max_descent = 700;     ///< placeholder, u/s.
    double vertical_accel = 1000; ///< placeholder, u/s^2.
    double max_pitch = 0.35;      ///< placeholder, rad.
    // Banking.
    double max_bank = 0.6;             ///< placeholder, rad.
    double bank_time_constant = 0.5;   ///< placeholder, s.
    double banked_rate = 0.25;         ///< placeholder: |yaw rate| above which the banked clips play.
    double bank_roll_scale = 0.5;      ///< placeholder: the model rolls by bank * this (the clips bank a little themselves).
    // Orbit, hover, pass.
    double orbit_radius = 2500;
    double orbit_height = 1500;
    double orbit_entry_speed = 0.3;   ///< placeholder: share of the cruise speed kept while the heading is 90 degrees off.
    double orbit_approach_angle = 1.2566; ///< placeholder: rad; the heading turns this far off the tangent, in or out, when far from the circle.
    double orbit_approach_band = 1.0;     ///< placeholder: share of the radius off the circle that gives the full angle.
    double hover_distance = 1800;
    double hover_retreat_ratio = 0.8; ///< placeholder: closer than this share of the hover distance, it flies away first.
    double hover_height = 900;
    double pass_height = 700;
    double dive_height = 400;
    double pass_arm_ahead = 500;      ///< placeholder: once the target is this far ahead, passing over it counts.
    double pass_close = 150;          ///< placeholder: within this of the target the pass is over it.
    double pass_overshoot = 2500;
    // Landing and takeoff.
    double landing_rings[2] = {1200, 2000};
    double landing_sample_radius = 400;
    double landing_flatness = 120;      ///< max height difference of the 7 samples.
    double approach_height = 600;
    double approach_slow_distance = 1500;
    double landing_time = 1.33;         ///< data: mtlandvertical.
    double takeoff_height = 600;
    double takeoff_time = 4.0;          ///< data: mttakeoffvertical.
    // Crash.
    double crash_speed = 1500;
    double crash_slide = 1000;
    // Tactics.
    double ground_seconds_scale = 20; ///< ground_attack_time * this = seconds on the ground.
    double orbit_seconds = 8;         ///< one lap.
    double orbit_band = 0.3;          ///< placeholder: the lap counts while within this share of the radius.
    double orbit_settle_limit = 20;   ///< placeholder: s; the lap counts anyway after this long in orbit.
    int min_air_attacks = 2;
    double injured_recovery = 0.25;   ///< placeholder: an injured dragon flies again at injured health + this (it regenerates ~1% a second).
    double breath_range = 2000;       ///< a pass breathes within this of the target.

    /// The defaults with the data of a race and its flying movement type.
    /// `angular_tolerance_deg` in degrees; speeds in units per second.
    static Params from_race(double max_turn_rate, double angular_acceleration, double angular_tolerance_deg,
                            double flight_radius, double cruise_speed, double run_speed);
};

enum class Mode { grounded, takeoff, cruise, orbit, hover, pass, approach, landing, crash, crashed };

struct Body {
    Vec3 pos;
    double yaw = 0;      ///< radians, see the header.
    double yaw_rate = 0; ///< rad/s, positive clockwise.
    double speed = 0;    ///< horizontal, along the heading.
    double vz = 0;
    double bank = 0;     ///< positive left wing down.
    double pitch = 0;    ///< positive nose up.
    Mode mode = Mode::grounded;
    double mode_time = 0;
    // Bookkeeping of the mover.
    Vec3 anchor;            ///< takeoff: where it left the ground.
    double pass_heading = 0; ///< the line of the current pass.
    int pass_phase = 0;      ///< 0 inbound, 1 passed over, 2 flown on and done.
    bool pass_armed = false; ///< inbound and the target was ahead: passing over it ends the inbound phase.
    double last_ground = 0;
};

enum class CommandKind { keep, takeoff, fly_to, orbit, hover, pass, land, crash };

struct Command {
    CommandKind kind = CommandKind::keep;
    /// fly_to goal, orbit centre, hover/pass target, landing spot.
    Vec3 point;
    double radius = 0; ///< orbit; 0 = Params::orbit_radius.
    double height = 0; ///< above the ground; 0 = the mode's default.
    bool dive = false; ///< pass: dive bomb (Params::dive_height).
};

struct Events {
    bool took_off = false;
    bool landed = false;
    bool crashed = false;
    bool passed_over = false;
};

/// Advances the body by dt seconds (substepped to at most 0.05 s).
Events step(Body& body, const Command& cmd, const Params& p, const Ground& ground, double dt);

/// GetFlyingState: 0 not flying (also crashed), 1 taking off, 2 cruising (any
/// forward flight), 3 hovering, 4 landing (approach, landing, crash).
int flying_state(Mode m);

/// The clip stem (no folder, no extension) for the body.
std::string clip_for(const Body& b, const Params& p, bool hurt);

/// A flat spot to land near `around`: candidates on two rings, nearest to
/// `from` first, accepted when seven samples differ by less than the flatness.
/// The result's z is the ground there.
std::optional<Vec3> find_landing(const Vec3& around, const Vec3& from, const Ground& ground, const Params& p);

enum class Maneuver { ground, takeoff, orbit, hover, pass, land };

/// The combat flight choice and its memory.
struct Tactics {
    Maneuver maneuver = Maneuver::ground;
    double timer = 0;    ///< in the current maneuver (hover: counted while stopped).
    double elapsed = 0;  ///< wall time in the current maneuver.
    int attacks = 0;     ///< since the last landing.
    bool dive = false;
    std::optional<Vec3> landing_spot;
    std::optional<Vec3> forced_spot; ///< the forced landing marker, kept for later landings.
    bool forced_pending = false;     ///< a newly set marker not yet answered.
    bool forced_seen = false;
    bool injured = false;            ///< below the race's injured health; stays until it has healed on.
};

/// The CSFL values of the combat style.
struct Style {
    double hover_chance = 0;
    double dive_bomb_chance = 0;
    double ground_attack_chance = 0;
    double hover_time = 0;
    double ground_attack_time = 0;
    double flying_attack_chance = 0;
};

struct TacticsInput {
    bool in_combat = false;
    bool allowed_to_fly = true;
    double health_fraction = 1;
    double injured_fraction = 0.35; ///< data: race injured health.
    std::optional<Vec3> forced_landing;
    Vec3 target;
    Vec3 self;
    Style style;
};

/// What to do this tick. `roll` returns a die in [0, 1); a chance succeeds when
/// roll < chance. Rolls are drawn in order: flying attack, hover, dive bomb (at
/// the end of a lap), ground attack (after an attack).
Command decide(Tactics& t, const TacticsInput& in, const Body& b, const Params& p, const Ground& ground, double dt,
               const std::function<double()>& roll);

/// True when the breath may start: hovering facing the target within the aim
/// tolerance, or in a pass with the target ahead within it and in range.
bool breath_allowed(const Tactics& t, const Body& b, const Vec3& target, const Params& p);

} // namespace skydot::flight
