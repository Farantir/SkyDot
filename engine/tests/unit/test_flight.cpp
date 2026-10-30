// SPDX-License-Identifier: GPL-3.0-or-later
//
// The flight mover (ai/flight.hpp): turn and climb limits, orbit, hover, pass,
// takeoff, landing, crash, the landing-spot search, clip choice and the combat
// flight choice, all on fake terrain.
#include "ai/flight.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <numbers>

using namespace skydot::flight;

namespace {

const Ground k_flat = [](double, double) { return 100.0; };

/// A ridge rising 3000 units over the 2000 units from x = 6000 to 8000.
const Ground k_ridge = [](double x, double) {
    const double t = std::clamp((x - 6000) / 2000.0, 0.0, 1.0);
    return 3000.0 * t;
};

Body airborne_body(double x, double y, double z, double yaw, double speed) {
    Body b;
    b.pos = {x, y, z};
    b.yaw = yaw;
    b.speed = speed;
    b.mode = Mode::cruise;
    return b;
}

Command orbit_at(Vec3 c) {
    Command cmd;
    cmd.kind = CommandKind::orbit;
    cmd.point = c;
    return cmd;
}

Events run(Body& b, const Command& c, const Params& p, const Ground& g, double seconds, double dt = 0.05) {
    Events all;
    for (int i = 0, n = static_cast<int>(std::lround(seconds / dt)); i < n; ++i) {
        const Events e = step(b, c, p, g, dt);
        all.took_off |= e.took_off;
        all.landed |= e.landed;
        all.crashed |= e.crashed;
        all.passed_over |= e.passed_over;
    }
    return all;
}

} // namespace

TEST_CASE("flight: yaw acceleration limits the change of the turn rate, the turn rate its maximum") {
    const Params p;
    Body b = airborne_body(0, 0, 1600, 0, 2000);
    Command c;
    c.kind = CommandKind::fly_to;
    c.point = {0, -30000, 0}; // behind: a U-turn
    const double dt = 0.02;
    double prev = b.yaw_rate;
    double peak = 0;
    double best = 1e9;
    for (int i = 0; i < 1000; ++i) {
        step(b, c, p, k_flat, dt);
        CHECK(std::fabs(b.yaw_rate - prev) <= p.angular_acceleration * p.angular_accel_scale * dt + 1e-9);
        CHECK(std::fabs(b.yaw_rate) <= p.max_turn_rate + 1e-9);
        peak = std::max(peak, std::fabs(b.yaw_rate));
        best = std::min(best, std::fabs(wrap_pi(heading_to(b.pos, c.point) - b.yaw)));
        prev = b.yaw_rate;
    }
    CHECK(peak > 0.8 * p.max_turn_rate);
    CHECK(best < p.angular_tolerance); // it did come round to the goal
}

TEST_CASE("flight: a large dt gives the same flight as small ones") {
    const Params p;
    Body a = airborne_body(0, 0, 1600, 0, 2000);
    Body b = a;
    const Command c = orbit_at({3000, 0, 100});
    run(a, c, p, k_flat, 10, 0.1);
    run(b, c, p, k_flat, 10, 0.05); // the same substeps
    CHECK(distance(a.pos, b.pos) < 1e-6);
    Body e = airborne_body(0, 0, 1600, 0, 2000);
    run(e, c, p, k_flat, 10, 0.02);
    CHECK(distance_xy(a.pos, e.pos) < 1500); // finer steps: the same kind of flight
    CHECK(distance_xy(e.pos, {3000, 0, 0}) < 4500);
    Body d = airborne_body(0, 0, 1600, 0, 2000);
    step(d, c, p, k_flat, 1.0);
    CHECK(std::isfinite(d.pos.x));
}

TEST_CASE("flight: the heading convention is Skyrim's (0 faces +y, clockwise)") {
    const Params p;
    Body b = airborne_body(0, 0, 1600, std::numbers::pi / 2, 2000);
    Command c;
    c.kind = CommandKind::fly_to;
    c.point = {50000, 0, 0};
    run(b, c, p, k_flat, 1);
    CHECK(b.pos.x > 1500);
    CHECK(std::fabs(b.pos.y) < 50);
}

TEST_CASE("flight: orbit converges to the radius and the height") {
    const Params p;
    const Vec3 centre{0, 0, 100};
    for (double start : {0.0, 1500.0, 5000.0}) {
        Body b = airborne_body(start, -3000, 1600, 0, 2000);
        const Command c = orbit_at(centre);
        run(b, c, p, k_flat, 60); // well over two laps
        for (int i = 0; i < 400; ++i) {
            step(b, c, p, k_flat, 0.05);
            const double r = distance_xy(b.pos, centre);
            CHECK(std::fabs(r - p.orbit_radius) < 0.15 * p.orbit_radius);
            CHECK(std::fabs(b.pos.z - (100 + p.orbit_height)) < 100);
        }
    }
}

TEST_CASE("flight: bank follows the turn direction and stays within the maximum") {
    const Params p;
    Body right = airborne_body(0, 0, 1600, 0, 2000);
    Body left = right;
    Command cr;
    cr.kind = CommandKind::fly_to;
    cr.point = {20000, 0, 0};
    Command cl = cr;
    cl.point = {-20000, 0, 0};
    for (int i = 0; i < 60; ++i) {
        step(right, cr, p, k_flat, 0.05);
        step(left, cl, p, k_flat, 0.05);
        CHECK(std::fabs(right.bank) <= p.max_bank + 1e-9);
        CHECK(std::fabs(left.bank) <= p.max_bank + 1e-9);
    }
    CHECK(right.yaw_rate > 0);
    CHECK(right.bank < -0.1);
    CHECK(left.yaw_rate < 0);
    CHECK(left.bank > 0.1);
}

TEST_CASE("flight: the dragon stays above the clearance over a ridge on its way") {
    const Params p;
    Body b = airborne_body(0, 0, 3100, std::numbers::pi / 2, 2000); // 1500 over the foot of the ground... flying east
    b.pos.z = 100 + p.cruise_height;
    Command c;
    c.kind = CommandKind::fly_to;
    c.point = {30000, 0, 0};
    double worst = 1e9;
    for (int i = 0; i < 2400 && b.pos.x < 14000; ++i) {
        step(b, c, p, k_ridge, 0.05);
        worst = std::min(worst, b.pos.z - k_ridge(b.pos.x, b.pos.y));
    }
    CHECK(worst >= p.min_clearance - 1e-6);
    CHECK(b.pos.x >= 14000); // it did get across
}

TEST_CASE("flight: a static spike in the path is cleared") {
    const Params p;
    const Ground tower = [](double x, double y) {
        return (std::fabs(x - 5000) < 200 && std::fabs(y) < 200) ? 4000.0 : 100.0;
    };
    Body b = airborne_body(0, 0, 1600, std::numbers::pi / 2, 2000);
    Command c;
    c.kind = CommandKind::fly_to;
    c.point = {12000, 0, 0};
    double worst = 1e9;
    for (int i = 0; i < 1200 && b.pos.x < 9000; ++i) {
        step(b, c, p, tower, 0.05);
        worst = std::min(worst, b.pos.z - tower(b.pos.x, b.pos.y));
    }
    CHECK(worst >= p.min_clearance - 1e-6);
    CHECK(b.pos.x >= 9000);
}

TEST_CASE("flight: it follows a slope down and up") {
    const Params p;
    const Ground slope = [](double x, double) { return 0.1 * x; };
    Body b = airborne_body(0, 0, 1500, std::numbers::pi / 2, 2000);
    Command c;
    c.kind = CommandKind::fly_to;
    c.point = {40000, 0, 0};
    run(b, c, p, slope, 20);
    CHECK(b.pos.z - slope(b.pos.x, 0) > p.min_clearance);
    CHECK(b.pos.z - slope(b.pos.x, 0) < p.cruise_height + 300);
}

TEST_CASE("flight: hover stops at its distance facing the target") {
    const Params p;
    const Vec3 target{0, 8000, 100};
    Body b = airborne_body(0, 0, 1600, 0, 2000);
    Command c;
    c.kind = CommandKind::hover;
    c.point = target;
    run(b, c, p, k_flat, 25);
    CHECK(b.mode == Mode::hover);
    CHECK(b.speed < 20);
    CHECK(std::fabs(distance_xy(b.pos, target) - p.hover_distance) < 300);
    CHECK(std::fabs(wrap_pi(heading_to(b.pos, target) - b.yaw)) < p.angular_tolerance);
    CHECK(std::fabs(b.pos.z - (100 + p.hover_height)) < 80);
    // and turns on the spot to a new target
    c.point = {-8000, b.pos.y, 100};
    run(b, c, p, k_flat, 15);
    CHECK(std::fabs(wrap_pi(heading_to(b.pos, c.point) - b.yaw)) < p.angular_tolerance);
}

TEST_CASE("flight: a pass flies over the target at pass height, then climbs") {
    const Params p;
    const Vec3 target{0, 6000, 100};
    Body b = airborne_body(0, 0, 1600, 0, 2000);
    Command c;
    c.kind = CommandKind::pass;
    c.point = target;
    bool passed = false;
    double z_over = 0;
    double xy_over = 1e9;
    for (int i = 0; i < 1200 && b.pass_phase < 2; ++i) {
        const Events e = step(b, c, p, k_flat, 0.05);
        if (e.passed_over) {
            passed = true;
            z_over = b.pos.z;
            xy_over = distance_xy(b.pos, target);
        }
    }
    CHECK(passed);
    CHECK(b.mode == Mode::pass);
    CHECK(xy_over < 200);
    CHECK(std::fabs(z_over - (100 + p.pass_height)) < 150);
    CHECK(b.pass_phase == 2);
    run(b, c, p, k_flat, 6);
    CHECK(b.pos.z > z_over + 300);
}

TEST_CASE("flight: from a standstill beside the centre it settles on the orbit circle within 10 s") {
    const Params p;
    Body b = airborne_body(1750, 0, 600, 0, 0);
    const Command c = orbit_at({0, 0, 0});
    double widest = 0;
    run(b, c, p, k_flat, 10);
    for (int i = 0; i < 400; ++i) {
        run(b, c, p, k_flat, 0.05);
        widest = std::max(widest, std::fabs(distance_xy(b.pos, c.point) - p.orbit_radius));
    }
    CHECK(widest < 0.1 * p.orbit_radius);
}

TEST_CASE("flight: a pass from an orbit's tangent flies over the target") {
    const Params p;
    for (double th : {0.0, 1.0, 2.5, 4.0}) {
        Body b = airborne_body(2500 * std::sin(th), 2500 * std::cos(th), 1600, th + 1.5708, 2000);
        b.yaw_rate = 0.8;
        Command c;
        c.kind = CommandKind::pass;
        c.point = {0, 0, 100};
        double closest = 1e9;
        for (int i = 0; i < 1200 && b.pass_phase < 2; ++i) {
            step(b, c, p, k_flat, 0.05);
            if (b.pass_phase == 0) closest = std::min(closest, distance_xy(b.pos, c.point));
        }
        CHECK(b.pass_phase == 2);
        CHECK(closest < 300);
    }
}

TEST_CASE("flight: a hover that starts beside the target flies out to the hover distance") {
    const Params p;
    Body b = airborne_body(450, 0, 1000, 0, 0);
    Command c;
    c.kind = CommandKind::hover;
    c.point = {0, 0, 0};
    run(b, c, p, k_flat, 40);
    CHECK(std::fabs(distance_xy(b.pos, c.point) - p.hover_distance) < 0.15 * p.hover_distance);
    CHECK(b.speed < 150);
}

TEST_CASE("flight: a dive bomb passes lower") {
    const Params p;
    Body b = airborne_body(0, 0, 1600, 0, 2000);
    Command c;
    c.kind = CommandKind::pass;
    c.point = {0, 6000, 100};
    c.dive = true;
    double z_over = 0;
    for (int i = 0; i < 1200 && b.pass_phase < 1; ++i) {
        step(b, c, p, k_flat, 0.05);
        z_over = b.pos.z;
    }
    CHECK(std::fabs(z_over - (100 + p.dive_height)) < 150);
}

TEST_CASE("flight: takeoff rises, ends with the event and flies on") {
    const Params p;
    Body b;
    b.pos = {0, 0, 100};
    Command c;
    c.kind = CommandKind::takeoff;
    Events all;
    double prev = b.pos.z;
    bool monotone = true;
    for (int i = 0; i < 100; ++i) { // 5 s
        const Events e = step(b, c, p, k_flat, 0.05);
        all.took_off |= e.took_off;
        if (b.pos.z < prev - 1e-9) monotone = false;
        prev = b.pos.z;
        if (i == 20) CHECK(b.mode == Mode::takeoff);
    }
    CHECK(monotone);
    CHECK(all.took_off);
    CHECK(b.pos.z >= 100 + p.takeoff_height - 5);
    CHECK((b.mode == Mode::cruise || b.mode == Mode::orbit));
    CHECK(flying_state(Mode::takeoff) == 1);
    c = orbit_at({0, 3000, 100});
    run(b, c, p, k_flat, 20);
    CHECK(b.mode == Mode::orbit);
    CHECK(b.pos.z > 1000);
}

TEST_CASE("flight: approach and landing end grounded on the ground") {
    const Ground slope = [](double, double y) { return 100.0 + 0.05 * y; };
    const Params p;
    Body b = airborne_body(0, -6000, 1600, 0, 2000);
    Command c;
    c.kind = CommandKind::land;
    c.point = {0, 0, 100};
    Events all;
    bool saw_approach = false;
    bool saw_landing = false;
    for (int i = 0; i < 2400 && b.mode != Mode::grounded; ++i) {
        const Events e = step(b, c, p, slope, 0.05);
        all.landed |= e.landed;
        saw_approach |= b.mode == Mode::approach;
        saw_landing |= b.mode == Mode::landing;
    }
    CHECK(saw_approach);
    CHECK(saw_landing);
    CHECK(all.landed);
    CHECK(b.mode == Mode::grounded);
    CHECK(std::fabs(b.pos.z - slope(b.pos.x, b.pos.y)) < 1e-6);
    CHECK(distance_xy(b.pos, c.point) < 250);
    CHECK(b.speed == 0);
    // Landing takes the clip's time.
    Body d = b;
    d.pos.z += 600;
    d.mode = Mode::landing;
    d.mode_time = 0;
    d.speed = 0;
    const Events e = run(d, c, p, slope, 1.2);
    CHECK_FALSE(e.landed);
    CHECK(run(d, c, p, slope, 0.2).landed);
}

TEST_CASE("flight: a crash glides down and ends crashed on the ground") {
    const Params p;
    Body b = airborne_body(0, 0, 1600, 0, 2000);
    Command c;
    c.kind = CommandKind::crash;
    const Events e = run(b, c, p, k_flat, 20);
    CHECK(e.crashed);
    CHECK(b.mode == Mode::crashed);
    CHECK(std::fabs(b.pos.z - 100) < 1e-6);
    CHECK(b.pos.y > 1000);
    CHECK(b.speed == 0);
    // Crashed stays down whatever it is told.
    c = orbit_at({0, 0, 100});
    run(b, c, p, k_flat, 2);
    CHECK(b.mode == Mode::crashed);
}

TEST_CASE("flight: find_landing rejects a bumpy or steep spot and finds a flat one") {
    const Params p;
    const Vec3 around{0, 0, 100};
    const Vec3 from{0, -5000, 1500};
    // Everything steep: nothing.
    const Ground steep = [](double x, double) { return 0.5 * x; };
    CHECK_FALSE(find_landing(around, from, steep, p).has_value());
    // Bumpy near the dragon (south), flat to the north.
    const Ground bumpy = [](double x, double y) {
        if (y < 0) return 100.0 + 300.0 * std::sin(x / 100.0) * std::cos(y / 100.0);
        return 100.0;
    };
    const auto spot = find_landing(around, from, bumpy, p);
    REQUIRE(spot.has_value());
    CHECK(spot->y > 0);
    CHECK(spot->z == 100.0);
    // Flat everywhere: the one nearest the dragon, on the outer ring.
    const auto near = find_landing(around, from, k_flat, p);
    REQUIRE(near.has_value());
    CHECK(near->y < 0);
    CHECK(std::fabs(distance_xy(*near, around) - p.landing_rings[1]) < 1e-6);
    // Unknown ground is no spot.
    const Ground unknown = [](double, double) { return std::nan(""); };
    CHECK_FALSE(find_landing(around, from, unknown, p).has_value());
}

TEST_CASE("flight: an unknown ground height keeps the last known one") {
    const Params p;
    const Ground g = [](double x, double) { return x > 3000 ? std::nan("") : 100.0; };
    Body b = airborne_body(0, 0, 1600, std::numbers::pi / 2, 2000);
    Command c;
    c.kind = CommandKind::fly_to;
    c.point = {20000, 0, 0};
    run(b, c, p, g, 5);
    CHECK(b.pos.x > 3000);
    CHECK(std::isfinite(b.pos.z));
    CHECK(b.pos.z > 100 + p.min_clearance);
}

TEST_CASE("flight: flying_state follows the CK values") {
    CHECK(flying_state(Mode::grounded) == 0);
    CHECK(flying_state(Mode::takeoff) == 1);
    CHECK(flying_state(Mode::cruise) == 2);
    CHECK(flying_state(Mode::orbit) == 2);
    CHECK(flying_state(Mode::pass) == 2);
    CHECK(flying_state(Mode::hover) == 3);
    CHECK(flying_state(Mode::approach) == 4);
    CHECK(flying_state(Mode::landing) == 4);
    CHECK(flying_state(Mode::crash) == 4);
    CHECK(flying_state(Mode::crashed) == 0);
}

TEST_CASE("flight: clip_for picks the variant of the mode, bank, climb and hurt") {
    const Params p;
    Body b = airborne_body(0, 0, 1600, 0, 2000);
    CHECK(clip_for(b, p, false) == "mtforward_glide");
    CHECK(clip_for(b, p, true) == "mtforward_glide_hurt");
    b.yaw_rate = -0.8;
    CHECK(clip_for(b, p, false) == "mtforward_glidebankleft");
    CHECK(clip_for(b, p, true) == "mtforward_glidehurtbankleft");
    b.yaw_rate = 0.8;
    CHECK(clip_for(b, p, false) == "mtforward_glidebankright");
    b.yaw_rate = 0;
    b.vz = 600;
    CHECK(clip_for(b, p, false) == "mtascend_climb");
    b.yaw_rate = 0.8;
    CHECK(clip_for(b, p, false) == "mtascend_climbbankright");
    b.yaw_rate = 0;
    b.vz = -600;
    CHECK(clip_for(b, p, false) == "mtdescend_dive");
    b.yaw_rate = -0.8;
    CHECK(clip_for(b, p, false) == "mtdescend_divebankleft");
    b.vz = 0;
    b.yaw_rate = 0;
    b.speed = 300;
    CHECK(clip_for(b, p, false) == "mtfeatherslow");
    b.yaw_rate = 0.5;
    CHECK(clip_for(b, p, false) == "mtfeatherslow_bankright");
    b.speed = 7400;
    b.yaw_rate = 0;
    CHECK(clip_for(b, p, false) == "mtfastforward_flap");
    CHECK(clip_for(b, p, true) == "mtfastforward_flap_hurt");
    b.mode = Mode::hover;
    b.speed = 0;
    CHECK(clip_for(b, p, false) == "mtidle_hover");
    b.yaw_rate = 0.6;
    CHECK(clip_for(b, p, false) == "mtflighthoverturnright90");
    b.mode = Mode::takeoff;
    CHECK(clip_for(b, p, false) == "mttakeoffvertical");
    b.mode = Mode::approach;
    CHECK(clip_for(b, p, false) == "mtlandverticalapproach");
    b.mode = Mode::landing;
    CHECK(clip_for(b, p, false) == "mtlandvertical");
    b.mode = Mode::crash;
    b.mode_time = 0.2;
    CHECK(clip_for(b, p, false) == "mtland_crashlongapproach");
    b.mode_time = 2;
    CHECK(clip_for(b, p, false) == "mtland_crashlong");
    b.mode = Mode::grounded;
    CHECK(clip_for(b, p, false).empty());
}

TEST_CASE("flight: Params::from_race takes the data") {
    const Params p = Params::from_race(1.5708, 0.25, 5, 400, 2000, 7400);
    CHECK(p.angular_tolerance == Catch::Approx(0.0872665).epsilon(1e-4));
    CHECK(p.min_clearance == 400);
    CHECK(Params::from_race(1, 1, 1, 900, 1, 1).min_clearance == 900);
    CHECK(Params::from_race(1, 1, 1, 100, 1, 1).min_clearance == 400);
}

namespace {

/// Dice read from a script; an empty script gives 0.99 (nothing succeeds).
struct Dice {
    std::deque<double> rolls;
    double operator()() {
        if (rolls.empty()) return 0.99;
        const double r = rolls.front();
        rolls.pop_front();
        return r;
    }
};

struct Rig {
    Params p;
    Tactics t;
    TacticsInput in;
    Body b;
    Dice dice;
    Events last;

    Rig() {
        in.in_combat = true;
        in.target = {0, 0, 100};
        in.self = {0, -3000, 100};
        in.style = {0.52, 0.36, 0.85, 0.42, 1.0, 0.75};
        b.pos = {0, -3000, 100};
    }

    Command tick(double dt = 0.05) {
        const std::function<double()> roll = [this] { return dice(); };
        const Command c = decide(t, in, b, p, k_flat, dt, roll);
        in.self = b.pos;
        last = step(b, c, p, k_flat, dt);
        return c;
    }

    /// Ticks until the maneuver is `m` (or the time is up); true when reached.
    bool until(Maneuver m, double max_seconds) {
        for (double s = 0; s < max_seconds; s += 0.05) {
            if (t.maneuver == m) return true;
            tick();
        }
        return t.maneuver == m;
    }

    void seconds(double s) {
        for (double x = 0; x < s; x += 0.05) tick();
    }
};

} // namespace

TEST_CASE("flight tactics: the whole combat cycle with scripted dice") {
    Rig r;
    // lap 1: attack, hover; after the hover (1 attack) orbit; lap 2: attack,
    // no hover, no dive: strafe; after it (2 attacks) the ground roll succeeds.
    r.dice.rolls = {0.0, 0.0,           // lap 1: attack, hover
                    0.0, 0.99, 0.99,    // lap 2: attack, no hover, no dive
                    0.0};               // ground attack
    CHECK(r.t.maneuver == Maneuver::ground);
    r.seconds(1);
    CHECK(r.b.mode == Mode::grounded); // waits ground_seconds = 20 s
    REQUIRE(r.until(Maneuver::takeoff, 25));
    REQUIRE(r.until(Maneuver::orbit, 10));
    CHECK(r.b.mode != Mode::grounded);
    REQUIRE(r.until(Maneuver::hover, 30));
    REQUIRE(r.until(Maneuver::orbit, 30));
    CHECK(r.t.attacks == 1);
    REQUIRE(r.until(Maneuver::pass, 30));
    CHECK_FALSE(r.t.dive);
    REQUIRE(r.until(Maneuver::land, 40));
    CHECK(r.t.attacks == 2);
    REQUIRE(r.until(Maneuver::ground, 40));
    CHECK(r.b.mode == Mode::grounded);
    CHECK(r.t.attacks == 0);
    REQUIRE(r.until(Maneuver::takeoff, 25));
}

TEST_CASE("flight tactics: the breath is allowed hovering at the target and in the pass") {
    Rig r;
    r.dice.rolls = {0.0, 0.0};
    r.b = airborne_body(0, -3000, 1600, 0, 2000);
    r.t.maneuver = Maneuver::orbit;
    bool in_hover = false;
    bool allowed = false;
    for (int i = 0; i < 20 * 40 && !in_hover; ++i) {
        r.tick();
        if (r.t.maneuver == Maneuver::hover && r.b.mode == Mode::hover) {
            allowed |= breath_allowed(r.t, r.b, r.in.target, r.p);
            if (r.b.speed < 5 && allowed) in_hover = true;
        }
    }
    CHECK(in_hover);
    CHECK(allowed);
    // Not facing: no breath.
    Body away = r.b;
    away.yaw += std::numbers::pi;
    CHECK_FALSE(breath_allowed(r.t, away, r.in.target, r.p));

    // A pass: inbound with the target ahead in range.
    Tactics tp;
    tp.maneuver = Maneuver::pass;
    Body b = airborne_body(0, -1500, 800, 0, 1200);
    b.mode = Mode::pass;
    CHECK(breath_allowed(tp, b, r.in.target, r.p));
    b.pos.y = -2600;
    CHECK_FALSE(breath_allowed(tp, b, r.in.target, r.p)); // out of range
    b.pos.y = -1500;
    b.pass_phase = 1;
    CHECK_FALSE(breath_allowed(tp, b, r.in.target, r.p)); // flown over
}

TEST_CASE("flight tactics: injured in the air lands and stays down") {
    Rig r;
    r.b = airborne_body(0, -3000, 1600, 0, 2000);
    r.t.maneuver = Maneuver::orbit;
    r.seconds(3);
    r.in.health_fraction = 0.2;
    REQUIRE(r.until(Maneuver::land, 2));
    REQUIRE(r.until(Maneuver::ground, 40));
    CHECK(r.b.mode == Mode::grounded);
    r.seconds(60);
    CHECK(r.b.mode == Mode::grounded);
    CHECK(r.t.maneuver == Maneuver::ground);
    r.in.health_fraction = 1.0;
    CHECK(r.until(Maneuver::takeoff, 30));
}

TEST_CASE("flight tactics: a dragon not allowed to fly lands and stays down") {
    Rig r;
    r.b = airborne_body(0, -3000, 1600, 0, 2000);
    r.t.maneuver = Maneuver::orbit;
    r.in.allowed_to_fly = false;
    REQUIRE(r.until(Maneuver::land, 2));
    REQUIRE(r.until(Maneuver::ground, 40));
    r.seconds(40);
    CHECK(r.b.mode == Mode::grounded);
}

TEST_CASE("flight tactics: a forced landing marker names where it lands next time, it does not land it at once") {
    Rig r;
    r.b = airborne_body(0, -3000, 1600, 0, 2000);
    r.t.maneuver = Maneuver::orbit;
    r.seconds(2);
    r.in.forced_landing = Vec3{6000, 6000, 100};
    r.in.style.flying_attack_chance = 0;
    r.seconds(20);
    CHECK(r.t.maneuver == Maneuver::orbit); // it keeps flying
    // Its attacks done and the decision to land taken, it lands at the marker.
    r.t.maneuver = Maneuver::orbit;
    r.t.attacks = 5;
    r.in.style.ground_attack_chance = 1;
    r.in.style.flying_attack_chance = 1;
    r.in.style.hover_chance = 0;
    REQUIRE(r.until(Maneuver::pass, 40));
    REQUIRE(r.until(Maneuver::land, 60));
    REQUIRE(r.until(Maneuver::ground, 80));
    CHECK(distance_xy(r.b.pos, {6000, 6000, 100}) < 300);
}

TEST_CASE("flight tactics: an injured dragon lands at the forced marker") {
    Rig r;
    r.b = airborne_body(0, -3000, 1600, 0, 2000);
    r.t.maneuver = Maneuver::orbit;
    r.in.forced_landing = Vec3{6000, 6000, 100};
    r.seconds(2);
    r.in.health_fraction = 0.2;
    REQUIRE(r.until(Maneuver::land, 2));
    REQUIRE(r.until(Maneuver::ground, 80));
    CHECK(distance_xy(r.b.pos, {6000, 6000, 100}) < 300);
}

TEST_CASE("flight tactics: out of combat it makes no demands") {
    Rig r;
    r.in.in_combat = false;
    const Command c = r.tick();
    CHECK(c.kind == CommandKind::keep);
    CHECK(r.t.maneuver == Maneuver::ground);
}

TEST_CASE("flight: the Godot facing is the Skyrim heading turned the other way") {
    // North (+y) is -Z in Godot: facing 0 and heading 0 agree; east (+x) is heading pi/2 and facing -pi/2.
    CHECK(facing_from_heading(0) == 0);
    CHECK(facing_from_heading(std::numbers::pi / 2) == Catch::Approx(-std::numbers::pi / 2));
    CHECK(heading_from_facing(facing_from_heading(1.0)) == Catch::Approx(1.0));
    // Facing f looks along (-sin f, -cos f) in Godot's (x, z); that is Skyrim (sin h, cos h) with z = -y.
    const double h = 0.7;
    const double f = facing_from_heading(h);
    CHECK(-std::sin(f) == Catch::Approx(std::sin(h)));
    CHECK(-std::cos(f) == Catch::Approx(-std::cos(h)));
}

TEST_CASE("flight tactics: an injured dragon stays down until it has healed well past the threshold") {
    Rig r;
    r.in.health_fraction = 0.3;
    r.seconds(25);
    CHECK(r.b.mode == Mode::grounded); // injured: it does not take off
    r.in.health_fraction = 0.4;       // regenerated a little, still above the threshold only
    r.seconds(25);
    CHECK(r.b.mode == Mode::grounded);
    r.in.health_fraction = 0.7;
    REQUIRE(r.until(Maneuver::takeoff, 25));
}

TEST_CASE("flight: a hovering dragon does not climb for a wall far ahead, a fast one does") {
    const Params p;
    const Ground wall = [](double x, double) { return x > 3500 ? 5000.0 : 0.0; };
    Body b = airborne_body(0, 0, 900, 1.5708, 0);
    Command c;
    c.kind = CommandKind::hover;
    c.point = {1800, 0, 0};
    run(b, c, p, wall, 15);
    CHECK(b.pos.z < 1200);
    Body fast = airborne_body(0, 0, 900, 1.5708, 2000);
    Command cruise;
    cruise.kind = CommandKind::fly_to;
    cruise.point = {20000, 0, 0};
    run(fast, cruise, p, wall, 3);
    CHECK(fast.pos.z > 1200); // it sees the wall 2 s ahead and climbs
}
