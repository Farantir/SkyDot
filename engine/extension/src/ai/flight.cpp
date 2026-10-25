// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/flight.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace skydot::flight {

namespace {

constexpr double k_pi = std::numbers::pi;
constexpr double k_max_substep = 0.05;
constexpr double k_min_cap_speed = 100;
constexpr double k_steer_gain = 2.0; // 1/s, the proportional part of the heading control

double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

double ground_at(Body& b, const Ground& g, double x, double y) {
    const double h = g ? g(x, y) : std::nan("");
    if (std::isnan(h)) return b.last_ground;
    return h;
}

void set_mode(Body& b, Mode m) {
    if (b.mode == m) return;
    b.mode = m;
    b.mode_time = 0;
}

bool airborne(Mode m) {
    return m != Mode::grounded && m != Mode::crashed;
}

/// Steers the heading towards `desired` with the rate and acceleration limits:
/// the wanted rate shrinks with the square root of the error so that the turn
/// ends on the heading without overshoot.
void steer(Body& b, const Params& p, double desired, double max_rate, double h, double feed_forward = 0) {
    const double a = p.angular_acceleration * p.angular_accel_scale;
    const double err = wrap_pi(desired - b.yaw);
    const double mag = std::min(k_steer_gain * std::fabs(err), std::sqrt(2 * a * std::fabs(err)));
    const double want = clampd(feed_forward + std::copysign(mag, err), -max_rate, max_rate);
    b.yaw_rate += clampd(want - b.yaw_rate, -a * h, a * h);
    b.yaw_rate = clampd(b.yaw_rate, -p.max_turn_rate, p.max_turn_rate);
    b.yaw = wrap_pi(b.yaw + b.yaw_rate * h);
}

void settle_yaw(Body& b, const Params& p, double h) {
    const double a = p.angular_acceleration * p.angular_accel_scale;
    b.yaw_rate += clampd(-b.yaw_rate, -a * h, a * h);
    b.yaw = wrap_pi(b.yaw + b.yaw_rate * h);
}

void approach_speed(Body& b, const Params& p, double target, double h) {
    const double d = target - b.speed;
    b.speed += clampd(d, -p.deceleration * h, p.acceleration * h);
    if (b.speed < 0) b.speed = 0;
}

struct Probe {
    double distance;
    double floor;
};

/// The clearance floors along the heading: at the body and ahead.
std::vector<Probe> probe_ahead(Body& b, const Params& p, const Ground& g) {
    static constexpr double k_distances[] = {0, 500, 1000, 1500, 2000, 3000, 4000, 6000};
    std::vector<Probe> out;
    const double sx = std::sin(b.yaw);
    const double sy = std::cos(b.yaw);
    // Far probes matter at speed only: a dragon hanging over its landing spot looks 600 units ahead, not past the trees 6000 away.
    const double reach = std::max(p.probe_min_reach, b.speed * p.probe_seconds);
    for (double d : k_distances) {
        if (d > reach) break;
        const double h = ground_at(b, g, b.pos.x + sx * d, b.pos.y + sy * d);
        out.push_back({d, h + p.min_clearance});
    }
    return out;
}

/// One step of flight at a desired heading, speed and absolute altitude, with
/// the terrain clearance (a speed cap when the climb could not keep up, a
/// raised target altitude, and a hard floor under the body).
void fly(Body& b, const Params& p, const Ground& g, double h, double desired_yaw, double max_rate,
         double speed_target, double alt_target, double feed_forward = 0) {
    steer(b, p, desired_yaw, max_rate, h, feed_forward);

    const auto probes = probe_ahead(b, p, g);
    double z_target = alt_target;
    double cap = speed_target;
    for (const Probe& pr : probes) {
        z_target = std::max(z_target, pr.floor);
        const double need = pr.floor - b.pos.z;
        if (pr.distance > 0 && need > 0) {
            cap = std::min(cap, std::max(k_min_cap_speed, 0.8 * p.max_climb * pr.distance / need));
        }
    }
    approach_speed(b, p, cap, h);

    const double vz_cmd = clampd(z_target - b.pos.z, -p.max_descent, p.max_climb);
    b.vz += clampd(vz_cmd - b.vz, -p.vertical_accel * h, p.vertical_accel * h);

    b.pos.x += std::sin(b.yaw) * b.speed * h;
    b.pos.y += std::cos(b.yaw) * b.speed * h;
    b.pos.z += b.vz * h;
    const double floor_here = ground_at(b, g, b.pos.x, b.pos.y) + p.min_clearance;
    if (b.pos.z < floor_here) {
        b.pos.z = floor_here;
        if (b.vz < 0) b.vz = 0;
    }
}

void update_attitude(Body& b, const Params& p, double h) {
    const double bank_target =
        clampd(-b.yaw_rate / p.max_turn_rate * p.max_bank * clampd(b.speed / 500.0, 0, 1), -p.max_bank, p.max_bank);
    const double k = 1 - std::exp(-h / p.bank_time_constant);
    b.bank += (bank_target - b.bank) * k;
    b.pitch = clampd(std::atan2(b.vz, std::max(b.speed, 200.0)), -p.max_pitch, p.max_pitch);
}

void apply_command(Body& b, const Command& c, const Params& p) {
    (void)p;
    const Mode m = b.mode;
    if (m == Mode::crashed || m == Mode::crash) return;
    if (c.kind == CommandKind::crash) {
        if (m == Mode::grounded) set_mode(b, Mode::crashed);
        else set_mode(b, Mode::crash);
        return;
    }
    if (m == Mode::takeoff || m == Mode::landing) return;
    if (m == Mode::grounded) {
        if (c.kind == CommandKind::takeoff) {
            set_mode(b, Mode::takeoff);
            b.anchor = b.pos;
            b.speed = 0;
        }
        return;
    }
    switch (c.kind) {
    case CommandKind::fly_to: set_mode(b, Mode::cruise); break;
    case CommandKind::orbit: set_mode(b, Mode::orbit); break;
    case CommandKind::hover: set_mode(b, Mode::hover); break;
    case CommandKind::pass:
        if (m != Mode::pass) {
            set_mode(b, Mode::pass);
            b.pass_heading = b.yaw;
            b.pass_phase = 0;
            b.pass_armed = false;
        }
        break;
    case CommandKind::land:
        if (m != Mode::approach) set_mode(b, Mode::approach);
        break;
    default: break;
    }
}

void substep(Body& b, const Command& c, const Params& p, const Ground& g, double h, Events& ev) {
    ground_at(b, g, b.pos.x, b.pos.y);
    b.last_ground = ground_at(b, g, b.pos.x, b.pos.y);
    apply_command(b, c, p);
    const double gh = b.last_ground;

    switch (b.mode) {
    case Mode::grounded:
    case Mode::crashed:
        b.speed = 0;
        b.vz = 0;
        b.yaw_rate = 0;
        b.bank = 0;
        b.pitch = 0;
        break;

    case Mode::takeoff: {
        settle_yaw(b, p, h);
        approach_speed(b, p, 0, h);
        const double remaining = std::max(p.takeoff_time - b.mode_time, h);
        b.vz = (b.anchor.z + p.takeoff_height - b.pos.z) / remaining;
        b.pos.z += b.vz * h;
        b.pos.x += std::sin(b.yaw) * b.speed * h;
        b.pos.y += std::cos(b.yaw) * b.speed * h;
        if (b.mode_time + h >= p.takeoff_time) {
            ev.took_off = true;
            set_mode(b, Mode::cruise);
            b.vz = 0;
            b.mode_time = -h; // the step below adds h
        }
        break;
    }

    case Mode::cruise: {
        const double dist = distance_xy(b.pos, c.point);
        double spd = dist > p.transit_distance ? p.transit_speed : p.cruise_speed;
        const double height = c.height > 0 ? c.height : p.cruise_height;
        if (dist < 1) spd = b.speed;
        fly(b, p, g, h, dist < 1 ? b.yaw : heading_to(b.pos, c.point), p.max_turn_rate, spd, gh + height);
        break;
    }

    case Mode::orbit: {
        const double radius = c.radius > 0 ? c.radius : p.orbit_radius;
        const double height = c.height > 0 ? c.height : p.orbit_height;
        const double gc = ground_at(b, g, c.point.x, c.point.y);
        const double r = distance_xy(b.pos, c.point);
        const double theta = r > 1 ? std::atan2(b.pos.x - c.point.x, b.pos.y - c.point.y) : b.yaw - k_pi / 2;
        const double radial = clampd((r - radius) / (p.orbit_approach_band * radius), -1, 1);
        const double desired = theta + k_pi / 2 + radial * p.orbit_approach_angle;
        const double lap_rate = b.speed / std::max(r, 0.5 * radius); // the tangent turns this fast
        // Slower while the heading is far from the wanted one (the entry, a turn back): the turn
        // radius shrinks with the speed, so the dragon closes on the circle instead of spiralling out.
        const double err = std::fabs(wrap_pi(desired - b.yaw));
        const double spd = p.cruise_speed * clampd(std::cos(std::min(err, k_pi / 2)) + p.orbit_entry_speed, p.orbit_entry_speed, 1);
        fly(b, p, g, h, desired, p.max_turn_rate, spd, gc + height, lap_rate);
        break;
    }

    case Mode::hover: {
        const double height = c.height > 0 ? c.height : p.hover_height;
        const double dist = distance_xy(b.pos, c.point);
        const double err = dist > 1 ? wrap_pi(heading_to(b.pos, c.point) - b.yaw) : 0;
        const double stop = dist - p.hover_distance;
        double spd = 0;
        if (stop > 0) {
            spd = std::min(p.cruise_speed, std::sqrt(2 * p.deceleration * 0.8 * stop));
            spd *= std::max(0.25, std::cos(std::min(std::fabs(err), k_pi / 2)));
        }
        const double rate = b.speed < 150 ? p.hover_turn_rate : p.max_turn_rate;
        const bool moving_out = b.speed > 100 && std::fabs(wrap_pi(heading_to(b.pos, c.point) - b.yaw)) > k_pi / 2;
        if (dist < p.hover_distance * p.hover_retreat_ratio || (moving_out && dist < p.hover_distance - 50)) {
            // Too close (it took off beside the target): it flies away, stopping at the hover distance.
            const double away = dist > 1 ? heading_to(c.point, b.pos) : b.yaw;
            const double out = std::sqrt(2 * p.deceleration * 0.8 * std::max(p.hover_distance - dist, 0.0));
            fly(b, p, g, h, away, p.max_turn_rate, std::min(p.pass_speed, std::max(out, 100.0)), gh + height);
            break;
        }
        fly(b, p, g, h, dist > 1 ? heading_to(b.pos, c.point) : b.yaw, rate, spd, gh + height);
        break;
    }

    case Mode::pass: {
        const double height = c.height > 0 ? c.height : (c.dive ? p.dive_height : p.pass_height);
        const double dist = distance_xy(b.pos, c.point);
        // Pure pursuit of the point over the target while inbound (a straight line fixed at the start
        // cannot be joined from an orbit's tangent within the turn radius), then straight on.
        const double ahead = (c.point.x - b.pos.x) * std::sin(b.yaw) + (c.point.y - b.pos.y) * std::cos(b.yaw);
        if (b.pass_phase == 0) {
            if (!b.pass_armed && ahead > p.pass_arm_ahead) b.pass_armed = true;
            if (b.pass_armed && (ahead <= 0 || dist < p.pass_close)) {
                b.pass_phase = 1;
                b.pass_heading = b.yaw;
                ev.passed_over = true;
            }
        }
        if (b.pass_phase == 1 && dist >= p.pass_overshoot) b.pass_phase = 2;
        const double desired = b.pass_phase == 0 ? (dist > 1 ? heading_to(b.pos, c.point) : b.yaw) : b.pass_heading;
        const double gt = ground_at(b, g, c.point.x, c.point.y);
        const double alt = b.pass_phase == 0 ? gt + height : std::max(gt + height, gh + p.orbit_height);
        const double spd = b.pass_phase < 2 ? p.pass_speed : p.cruise_speed;
        fly(b, p, g, h, desired, p.max_turn_rate, spd, alt);
        break;
    }

    case Mode::approach: {
        const double dist = distance_xy(b.pos, c.point);
        const double gs = ground_at(b, g, c.point.x, c.point.y);
        const double alt = gs + p.approach_height;
        const double err = dist > 1 ? wrap_pi(heading_to(b.pos, c.point) - b.yaw) : 0;
        double spd = std::min(p.cruise_speed, std::max(40.0, dist * p.cruise_speed / p.approach_slow_distance * 0.5));
        if (std::fabs(err) > k_pi / 2) spd = std::min(spd, 300.0);
        const double rate = b.speed < 150 ? p.hover_turn_rate * 2 : p.max_turn_rate;
        fly(b, p, g, h, dist > 1 ? heading_to(b.pos, c.point) : b.yaw, rate, spd, alt);
        if ((dist < 150 && std::fabs(b.pos.z - alt) < 150) || b.mode_time > 60) {
            set_mode(b, Mode::landing);
            b.mode_time = -h;
        }
        break;
    }

    case Mode::landing: {
        settle_yaw(b, p, h);
        approach_speed(b, p, 0, h);
        const double remaining = std::max(p.landing_time - b.mode_time, h);
        b.vz = (gh - b.pos.z) / remaining;
        b.pos.z += b.vz * h;
        b.pos.x += std::sin(b.yaw) * b.speed * h;
        b.pos.y += std::cos(b.yaw) * b.speed * h;
        if (b.mode_time + h >= p.landing_time) {
            b.pos.z = ground_at(b, g, b.pos.x, b.pos.y);
            ev.landed = true;
            set_mode(b, Mode::grounded);
            b.speed = 0;
            b.vz = 0;
            b.yaw_rate = 0;
            b.bank = 0;
            b.pitch = 0;
        }
        break;
    }

    case Mode::crash: {
        settle_yaw(b, p, h);
        const double above = b.pos.z - gh;
        if (above > 1) {
            approach_speed(b, p, p.crash_speed, h);
            b.vz += clampd(-600 - b.vz, -p.vertical_accel * h, p.vertical_accel * h);
        } else {
            const double decel = p.crash_speed * p.crash_speed / (2 * p.crash_slide);
            b.speed = std::max(0.0, b.speed - decel * h);
            b.vz = 0;
        }
        b.pos.x += std::sin(b.yaw) * b.speed * h;
        b.pos.y += std::cos(b.yaw) * b.speed * h;
        b.pos.z += b.vz * h;
        const double g2 = ground_at(b, g, b.pos.x, b.pos.y);
        if (b.pos.z <= g2 + 1) {
            b.pos.z = g2;
            if (b.speed <= 0) {
                ev.crashed = true;
                set_mode(b, Mode::crashed);
                b.vz = 0;
                b.yaw_rate = 0;
            }
        }
        break;
    }
    }

    if (airborne(b.mode)) update_attitude(b, p, h);
    b.mode_time += h;
}

} // namespace

double wrap_pi(double a) {
    a = std::fmod(a + k_pi, 2 * k_pi);
    if (a < 0) a += 2 * k_pi;
    return a - k_pi;
}

Params Params::from_race(double max_turn_rate, double angular_acceleration, double angular_tolerance_deg,
                         double flight_radius, double cruise_speed, double run_speed) {
    Params p;
    p.max_turn_rate = max_turn_rate;
    p.angular_acceleration = angular_acceleration;
    p.angular_tolerance = angular_tolerance_deg * k_pi / 180.0;
    p.min_clearance = std::max(flight_radius, 400.0);
    p.cruise_speed = cruise_speed;
    p.transit_speed = run_speed;
    return p;
}

Events step(Body& b, const Command& c, const Params& p, const Ground& g, double dt) {
    Events ev;
    if (dt <= 0) return ev;
    const int n = std::max(1, static_cast<int>(std::ceil(dt / k_max_substep)));
    const double h = dt / n;
    for (int i = 0; i < n; ++i) substep(b, c, p, g, h, ev);
    return ev;
}

int flying_state(Mode m) {
    switch (m) {
    case Mode::grounded:
    case Mode::crashed: return 0;
    case Mode::takeoff: return 1;
    case Mode::cruise:
    case Mode::orbit:
    case Mode::pass: return 2;
    case Mode::hover: return 3;
    case Mode::approach:
    case Mode::landing:
    case Mode::crash: return 4;
    }
    return 0;
}

std::string clip_for(const Body& b, const Params& p, bool hurt) {
    switch (b.mode) {
    case Mode::grounded: return {};
    case Mode::takeoff: return "mttakeoffvertical";
    case Mode::approach: return "mtlandverticalapproach";
    case Mode::landing: return "mtlandvertical";
    case Mode::crash: return b.mode_time < 1.0 ? "mtland_crashlongapproach" : "mtland_crashlong";
    case Mode::crashed: return "mtland_crashlong";
    case Mode::hover:
        if (std::fabs(b.yaw_rate) > 0.3) return b.yaw_rate > 0 ? "mtflighthoverturnright90" : "mtflighthoverturnleft90";
        return "mtidle_hover";
    default: break;
    }
    const char* side = "";
    if (b.yaw_rate < -p.banked_rate) side = "left";
    else if (b.yaw_rate > p.banked_rate) side = "right";
    const bool banked = *side != 0;
    const std::string s = side;
    if (b.vz > 250) return "mtascend_climb" + (banked ? "bank" + s : std::string());
    if (b.vz < -300) return "mtdescend_dive" + (banked ? "bank" + s : std::string());
    if (b.speed < 600) return "mtfeatherslow" + (banked ? "_bank" + s : std::string());
    if (b.speed > p.cruise_speed * 1.05) {
        if (hurt) return banked ? "mtfastforward_flap_hurtbank" + s : "mtfastforward_flap_hurt";
        return "mtfastforward_flap";
    }
    if (hurt) return banked ? "mtforward_glidehurtbank" + s : "mtforward_glide_hurt";
    return banked ? "mtforward_glidebank" + s : "mtforward_glide";
}

std::optional<Vec3> find_landing(const Vec3& around, const Vec3& from, const Ground& ground, const Params& p) {
    struct Cand {
        Vec3 pos;
        double d;
    };
    std::vector<Cand> cands;
    for (double ring : p.landing_rings) {
        for (int i = 0; i < 8; ++i) {
            const double a = i * k_pi / 4;
            Vec3 c{around.x + std::sin(a) * ring, around.y + std::cos(a) * ring, 0};
            cands.push_back({c, distance_xy(c, from)});
        }
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    for (const Cand& c : cands) {
        double lo = 1e18;
        double hi = -1e18;
        bool ok = true;
        for (int i = 0; i < 7 && ok; ++i) {
            double x = c.pos.x;
            double y = c.pos.y;
            if (i > 0) {
                const double a = (i - 1) * k_pi / 3;
                x += std::sin(a) * p.landing_sample_radius;
                y += std::cos(a) * p.landing_sample_radius;
            }
            const double h = ground(x, y);
            if (std::isnan(h)) {
                ok = false;
                break;
            }
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        if (ok && hi - lo < p.landing_flatness) return Vec3{c.pos.x, c.pos.y, ground(c.pos.x, c.pos.y)};
    }
    return std::nullopt;
}

namespace {

void enter(Tactics& t, Maneuver m) {
    t.maneuver = m;
    t.timer = 0;
    t.elapsed = 0;
}

/// An attack is over: count it and choose between the ground and another orbit.
void attack_done(Tactics& t, const TacticsInput& in, const Params& p, const std::function<double()>& roll) {
    ++t.attacks;
    if (t.attacks >= p.min_air_attacks && roll() < in.style.ground_attack_chance) {
        t.landing_spot.reset();
        enter(t, Maneuver::land);
    } else {
        enter(t, Maneuver::orbit);
    }
}

Command orbit_cmd(const TacticsInput& in) {
    Command c;
    c.kind = CommandKind::orbit;
    c.point = in.target;
    return c;
}

} // namespace

Command decide(Tactics& t, const TacticsInput& in, const Body& b, const Params& p, const Ground& ground, double dt,
               const std::function<double()>& roll) {
    if (in.forced_landing) {
        t.forced_spot = in.forced_landing;
        if (!t.forced_seen) t.forced_pending = true;
        t.forced_seen = true;
    } else {
        t.forced_seen = false;
        t.forced_pending = false;
    }

    // Injured: down until it has healed well past the threshold (it regenerates while it waits).
    if (in.health_fraction < in.injured_fraction) {
        t.injured = true;
    } else if (in.health_fraction >= in.injured_fraction + p.injured_recovery) {
        t.injured = false;
    }

    if (!in.in_combat) {
        enter(t, b.mode == Mode::grounded ? Maneuver::ground : Maneuver::orbit);
        t.attacks = 0;
        t.forced_pending = false;
        return {};
    }

    t.timer += dt;
    t.elapsed += dt;
    const bool down = !in.allowed_to_fly || t.injured;
    const bool flying = b.mode != Mode::grounded && b.mode != Mode::crashed;

    // A flyer unknown to the tactics (loaded in the air, or it landed by
    // itself) is put into the picture.
    if (t.maneuver == Maneuver::ground && flying && b.mode != Mode::takeoff) enter(t, Maneuver::orbit);
    if (t.maneuver != Maneuver::ground && !flying && t.maneuver != Maneuver::takeoff) {
        t.attacks = 0;
        t.landing_spot.reset();
        enter(t, Maneuver::ground);
    }

    bool land_now = false;
    if (flying && b.mode != Mode::takeoff && t.maneuver != Maneuver::land) {
        if (down) land_now = true;
        if (t.forced_pending) land_now = true;
    }
    if (!flying || t.maneuver == Maneuver::land) t.forced_pending = false;
    if (land_now) {
        t.forced_pending = false;
        t.landing_spot.reset();
        enter(t, Maneuver::land);
    }

    switch (t.maneuver) {
    case Maneuver::ground: {
        if (!down && t.timer >= in.style.ground_attack_time * p.ground_seconds_scale) {
            enter(t, Maneuver::takeoff);
            Command c;
            c.kind = CommandKind::takeoff;
            return c;
        }
        return {};
    }
    case Maneuver::takeoff: {
        if (b.mode == Mode::grounded || b.mode == Mode::takeoff) {
            Command c;
            c.kind = CommandKind::takeoff;
            return c;
        }
        enter(t, down ? Maneuver::land : Maneuver::orbit);
        if (t.maneuver == Maneuver::orbit) return orbit_cmd(in);
        break;
    }
    case Maneuver::orbit: {
        // A lap counts once the circle is flown (entering it, or coming back from a pass, does not).
        if (std::fabs(distance_xy(in.self, in.target) - p.orbit_radius) > p.orbit_band * p.orbit_radius &&
            t.elapsed < p.orbit_settle_limit) {
            t.timer = 0;
        }
        if (t.timer >= p.orbit_seconds) {
            t.timer = 0;
            if (roll() < in.style.flying_attack_chance) {
                if (roll() < in.style.hover_chance) {
                    enter(t, Maneuver::hover);
                } else {
                    t.dive = roll() < in.style.dive_bomb_chance;
                    enter(t, Maneuver::pass);
                }
            }
        }
        if (t.maneuver == Maneuver::orbit) return orbit_cmd(in);
        break;
    }
    case Maneuver::hover: {
        if (b.speed < 150) {
            // timer already counts the call; only count while stopped.
        } else {
            t.timer -= dt;
        }
        if (t.timer >= in.style.hover_time * p.ground_seconds_scale ||
            t.elapsed > in.style.hover_time * p.ground_seconds_scale + 15) {
            attack_done(t, in, p, roll);
            break;
        }
        Command c;
        c.kind = CommandKind::hover;
        c.point = in.target;
        return c;
    }
    case Maneuver::pass: {
        if ((b.mode == Mode::pass && b.pass_phase >= 2) || t.elapsed > 30) {
            attack_done(t, in, p, roll);
            break;
        }
        Command c;
        c.kind = CommandKind::pass;
        c.point = in.target;
        c.dive = t.dive;
        return c;
    }
    case Maneuver::land:
        break;
    }

    if (t.maneuver == Maneuver::orbit) return orbit_cmd(in);
    if (t.maneuver == Maneuver::land) {
        if (t.forced_spot) t.landing_spot = t.forced_spot;
        else if (!t.landing_spot) t.landing_spot = find_landing(in.target, in.self, ground, p);
        if (!t.landing_spot) {
            // Nowhere flat: keep orbiting and try again after a lap.
            if (t.elapsed > p.orbit_seconds) enter(t, Maneuver::orbit);
            return orbit_cmd(in);
        }
        Command c;
        c.kind = CommandKind::land;
        c.point = *t.landing_spot;
        return c;
    }
    if (t.maneuver == Maneuver::hover) {
        Command c;
        c.kind = CommandKind::hover;
        c.point = in.target;
        return c;
    }
    return {};
}

bool breath_allowed(const Tactics& t, const Body& b, const Vec3& target, const Params& p) {
    const double err = std::fabs(wrap_pi(heading_to(b.pos, target) - b.yaw));
    if (t.maneuver == Maneuver::hover && b.mode == Mode::hover) return err <= p.aim_tolerance;
    if (t.maneuver == Maneuver::pass && b.mode == Mode::pass && b.pass_phase == 0)
        return err <= p.aim_tolerance && distance(b.pos, target) <= p.breath_range;
    return false;
}

} // namespace skydot::flight
