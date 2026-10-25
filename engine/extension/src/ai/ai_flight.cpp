// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AI of a flying race (the dragon, GAMEPLAY-PLAN.md step 7b-6): in combat flight::decide chooses
// the maneuver (take off, orbit, hover, pass, land) from the combat style and the actor's state, and
// the orders go to SkydotActor::fly; the breath is started through the shout path once the mover
// aims it (flight::breath_allowed); out of combat a flying actor follows its patrol markers or
// orbits.
//
// Log lines (the sse tests parse them; keep them stable). "height" is the clearance over the ground
// under the actor, "ground" the surface height there, both game units; positions are Skyrim's.
//   flight: 0x<ref> <maneuver> (height <h> over ground <g>)
//       on every change of the maneuver: ground, takeoff, orbit, hover, pass, dive (a pass that is a
//       dive bomb) or land.
//   flight: 0x<ref> <mode> at (<x>,<y>,<z>) ground <g> clearance <c> dist <d>
//       every 2 s while the mover has the body; <mode> is the mover's (takeoff, cruise, orbit, hover,
//       pass, approach, landing, crash), <d> the horizontal distance to the target (in combat) or to
//       the goal (patrol marker, orbit centre).
#include "ai/ai.hpp"

#include "actors/actor.hpp"
#include "vm/papyrus.hpp"

#include <cmath>
#include <cstdio>

using godot::Vector3;

namespace skydot {
namespace {

constexpr double k_flight_log_interval = 2.0;
constexpr double k_breath_interval = 0.25;   ///< Seconds between the looks for a breath.
constexpr double k_patrol_reach = 1500.0;    ///< Units: a marker is reached within this.

flight::Vec3 vec(const Vector3& v) {
    return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

const char* maneuver_name(const flight::Tactics& t) {
    switch (t.maneuver) {
    case flight::Maneuver::ground: return "ground";
    case flight::Maneuver::takeoff: return "takeoff";
    case flight::Maneuver::orbit: return "orbit";
    case flight::Maneuver::hover: return "hover";
    case flight::Maneuver::pass: return t.dive ? "dive" : "pass";
    case flight::Maneuver::land: return "land";
    }
    return "ground";
}

} // namespace

void SkydotAi::flight_log_tick(Mind& m, SkydotActor& actor, double distance, double seconds) {
    CombatMind& c = m.combat;
    c.flight_log -= seconds;
    if (c.flight_log > 0.0 || !actor.flight_active()) {
        return;
    }
    c.flight_log = k_flight_log_interval;
    const flight::Body& b = actor.flight_body();
    const double ground = actor.flight_ground(b.pos.x, b.pos.y);
    std::printf("flight: 0x%X %s at (%.0f,%.0f,%.0f) ground %.0f clearance %.0f dist %.0f\n", m.ref,
                actor.get_flight_mode().utf8().get_data(), b.pos.x, b.pos.y, b.pos.z, ground, b.pos.z - ground,
                distance);
}

bool SkydotAi::flight_combat(Mind& m, SkydotActor& actor, double seconds) {
    CombatMind& c = m.combat;
    const auto at = position_of(c.target);
    if (!at) {
        return false;
    }
    if (c.gear_age > 2.0) {
        read_gear(m);
    }
    const flight::Params& params = actor.flight_params();
    const auto ground = [&actor](double x, double y) { return actor.flight_ground(x, y); };

    flight::TacticsInput in;
    in.in_combat = true;
    in.allowed_to_fly = vm_->allowed_to_fly(m.ref) && world_->data().world_ptr(m.space) != nullptr;
    const double max = vm_->get_actor_value_max(m.ref, "Health");
    in.health_fraction = max > 0.0 ? vm_->get_actor_value(m.ref, "Health") / max : 1.0;
    in.injured_fraction = c.injured;
    if (const std::uint32_t marker = vm_->forced_landing_marker(m.ref); marker != 0) {
        if (const auto spot = ref_spot(marker)) {
            in.forced_landing = vec(spot->position);
        }
    }
    in.target = vec(*at);
    in.self = vec(m.last);
    in.style = c.flight_style;
    const std::function<double()> dice = [this, &m] { return roll(m); };
    const flight::Command command = flight::decide(c.flight, in, actor.flight_body(), params, ground, seconds, dice);
    actor.set_flight_hurt(in.health_fraction < c.injured);
    if (command.kind != flight::CommandKind::keep) {
        actor.fly(command);
    }

    const int maneuver = static_cast<int>(c.flight.maneuver) + (c.flight.dive ? 100 : 0);
    if (maneuver != c.flight_logged) {
        c.flight_logged = maneuver;
        const double g = actor.flight_ground(static_cast<double>(m.last.x), static_cast<double>(m.last.y));
        std::printf("flight: 0x%X %s (height %.0f over ground %.0f)\n", m.ref, maneuver_name(c.flight),
                    static_cast<double>(m.last.z) - g, g);
    }
    if (!actor.flight_active()) {
        return false; // on the ground: the ground fight
    }
    const double distance = std::hypot(static_cast<double>(m.last.x - at->x), static_cast<double>(m.last.y - at->y));
    flight_log_tick(m, actor, distance, seconds);

    // The breath: said once the mover has aimed it; the body keeps flying while it inhales.
    if (shout_tick(m, seconds)) {
        return true;
    }
    if (c.think <= 0.0) {
        c.think = k_breath_interval;
        if (!c.cast.active && flight::breath_allowed(c.flight, actor.flight_body(), vec(*at), params)) {
            shout_decide(m, actor, distance, *at, sees(m, actor, c.target), true);
        }
    }
    return true;
}

void SkydotAi::flight_idle(Mind& m, SkydotActor& actor, double seconds) {
    CombatMind& c = m.combat;
    const flight::Params& params = actor.flight_params();
    const flight::Body& body = actor.flight_body();
    flight::Command command;

    if (!vm_->allowed_to_fly(m.ref) || world_->data().world_ptr(m.space) == nullptr) {
        // May not fly (any more): down at a flat spot near where it is.
        if (!c.idle_land) {
            const auto ground = [&actor](double x, double y) { return actor.flight_ground(x, y); };
            c.idle_land = flight::find_landing(body.pos, body.pos, ground, params);
        }
        if (c.idle_land) {
            command.kind = flight::CommandKind::land;
            command.point = *c.idle_land;
            actor.fly(command);
            flight_log_tick(m, actor, flight::distance_xy(body.pos, *c.idle_land), seconds);
            return;
        }
    } else {
        c.idle_land.reset();
    }

    // The package decides where: a patrol's markers, else the orbit marker or the editor spot.
    m.think -= seconds;
    if (m.think <= 0.0) {
        m.think = 1.5;
        choose(m);
    }
    if (m.package != 0 && m.step < m.steps.size() && m.steps[m.step].procedure == ai::Procedure::patrol) {
        if (!m.step_started) {
            m.step_started = true;
            m.step_time = 0.0;
            m.target = step_spot(m, m.steps[m.step]);
            load_patrol(m);
        }
    }
    if (!m.patrol.empty() && m.package != 0 && m.step < m.steps.size() &&
        m.steps[m.step].procedure == ai::Procedure::patrol) {
        if (m.patrol_next >= m.patrol.size()) {
            m.patrol_next = 0;
        }
        flight::Vec3 goal = vec(m.patrol[m.patrol_next]);
        if (flight::distance_xy(body.pos, goal) < k_patrol_reach) {
            m.patrol_next = (m.patrol_next + 1) % m.patrol.size();
            goal = vec(m.patrol[m.patrol_next]);
        }
        command.kind = flight::CommandKind::fly_to;
        command.point = goal;
        actor.fly(command);
        flight_log_tick(m, actor, flight::distance_xy(body.pos, goal), seconds);
        return;
    }
    if (!orbit_keyword_known_) {
        orbit_keyword_known_ = true;
        orbit_keyword_ = world_->data().keyword_by_editor_id("LinkDragonOrbitMarker");
    }
    Vector3 centre = editor_spot(m).position;
    if (orbit_keyword_ != 0) {
        if (const auto marker = ref_spot(linked_ref(m.ref, orbit_keyword_))) {
            centre = marker->position;
        }
    }
    command.kind = flight::CommandKind::orbit;
    command.point = vec(centre);
    actor.fly(command);
    flight_log_tick(m, actor, flight::distance_xy(body.pos, command.point), seconds);
}

} // namespace skydot
