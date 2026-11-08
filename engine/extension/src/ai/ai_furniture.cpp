// SPDX-License-Identifier: GPL-3.0-or-later
//
// Actors using furniture (SkydotAi's part; the Godot-free pieces are in furniture_use.hpp).
//
// A step that uses a furniture (Sleep, Sit, Eat, UseIdleMarker, Activate on a reference whose base is
// a FURN with markers) runs like this:
//   - Plan: pick a free marker of the step's kind, the entry (the allowed side nearest the actor), and
//     ask the IDLE tree what the actor plays: ActionActivate's tree, the root for the actor's behaviour
//     graph, with the facts the conditions give (GetSitting/GetSleeping 2, IsEnteringInteractionQuick,
//     IsFurnitureEntryType, the furniture's keywords). The graph, run from the origin, shows how far the
//     enter moves the body: the actor starts that far from the marker (the entry point).
//   - Placed on load (attached with the step already running): enter instantly (the IDLE tree's
//     quick idle), the body at the marker. Otherwise walk to the entry point, then enter.
//   - In (state 3): the graph's clip loops, the body is held at the marker.
//   - Out: when the step no longer uses the furniture, or combat starts, the exit idle is played (state
//     4); FallStart hands the body to the physics (state 0 from there; LandStart goes to the graph
//     when it lands), IdleBedGetUp and IdleChairGetUp are OnGetUp for the actor's scripts, and the use
//     ends when the graph is back in the state it left.
#include "actors/actor.hpp"
#include "ai/ai.hpp"
#include "ai/idle_choice.hpp"
#include "render/furniture_markers.hpp"
#include "vm/papyrus.hpp"
#include "data/fb_search.hpp"
#include "skydot_formats/flags.hpp"

#include <godot_cpp/core/memory.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>

using godot::Dictionary;
using godot::String;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {
namespace {

/// Game units within which the walk to the entry point is over; the body is placed on it from there.
/// PLACEHOLDER: the navmesh path ends within the actor's own tolerance of the point.
constexpr double k_entry_reach = 24.0;
/// A walk that gave up this far from the entry point is not an enter: the actor stays where it is.
constexpr double k_enter_snap = 96.0;

constexpr std::uint32_t fourcc(const char (&tag)[5]) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(tag[0])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(tag[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(tag[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(tag[3])) << 24;
}

bool same_event(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

double horizontal(const Vector3& a, const Vector3& b) {
    return static_cast<double>(godot::Vector2(a.x - b.x, a.y - b.y).length());
}

} // namespace

// ---- what the actor's step uses --------------------------------------------------------------

std::uint32_t SkydotAi::base_of_ref(std::uint32_t ref) const {
    const auto cell = static_cast<std::uint32_t>(world_->get_ref_cell(ref));
    const auto* c = world_->data().cell_ptr(cell);
    const auto* r = c != nullptr ? lookup(c->refs(), ref) : nullptr;
    if (r != nullptr) {
        return r->base();
    }
    return vm_ != nullptr ? vm_->base_form_of(ref) : 0;
}

const std::vector<FurnitureMarker>& SkydotAi::markers_of(std::uint32_t base_id) {
    if (const auto it = markers_.find(base_id); it != markers_.end()) {
        return it->second;
    }
    std::vector<FurnitureMarker> out;
    const auto* base = world_->data().base_ptr(base_id);
    if (base != nullptr && base->type() == fourcc("FURN") && base->model() != nullptr && base->model()->size() != 0) {
        if (godot::Node3D* model = world_->build_model(String::utf8(base->model()->c_str()))) {
            out = furniture_markers(model);
            godot::memdelete(model);
        }
    }
    return markers_.emplace(base_id, std::move(out)).first->second;
}

std::optional<SkydotAi::FurnitureTarget> SkydotAi::furniture_target(Mind& m, const ai::Step& s) {
    using ai::Procedure;
    switch (s.procedure) {
    case Procedure::sleep:
    case Procedure::sit:
    case Procedure::eat:
    case Procedure::use_idle_marker:
    case Procedure::activate:
        break;
    default:
        return std::nullopt;
    }
    const auto spot = step_spot(m, s);
    if (!spot || spot->ref == 0 || spot->ref == m.ref || spot->ref == m.furniture_skip || spot->space != m.space) {
        return std::nullopt;
    }
    const std::uint32_t base = base_of_ref(spot->ref);
    if (base == 0) {
        return std::nullopt;
    }
    const auto& markers = markers_of(base);
    if (markers.empty()) {
        return std::nullopt;
    }
    return FurnitureTarget{spot->ref, base,
                           ai::Pose{static_cast<double>(spot->position.x), static_cast<double>(spot->position.y),
                                    static_cast<double>(spot->position.z), static_cast<double>(spot->rotation_z)},
                           &markers};
}

bool SkydotAi::furniture_taken(std::uint32_t ref, std::uint32_t base, std::uint32_t except) {
    const std::size_t markers = markers_of(base).size();
    if (markers == 0) {
        return false;
    }
    std::size_t users = 0;
    for (const auto& [other, mind] : minds_) {
        if (other == except) {
            continue;
        }
        const bool found = std::ranges::any_of(mind.lists, [ref](const auto& entry) { return entry.second == ref; });
        if (found || (mind.furniture && mind.furniture->ref == ref)) {
            ++users;
        }
    }
    return users >= markers;
}

bool SkydotAi::furniture_claimed(std::uint32_t ref, int marker, std::uint32_t except) const {
    for (const auto& [other, mind] : minds_) {
        if (other != except && mind.furniture && mind.furniture->ref == ref && mind.furniture->marker == marker) {
            return true;
        }
    }
    return false;
}

bool SkydotAi::furniture_wanted(Mind& m, const FurnitureUse& u) {
    if (m.package == 0 || m.steps.empty()) {
        return false;
    }
    std::size_t step = m.step;
    if (step >= m.steps.size()) {
        // Done, or about to start over.
        if (!ai::repeats(ai::resolve_package(*root(), m.package))) {
            return false;
        }
        step = 0;
    }
    const auto target = furniture_target(m, m.steps[step]);
    return target && target->ref == u.ref;
}

// ---- planning and entering -------------------------------------------------------------------

std::shared_ptr<SkydotAi::FurnitureUse> SkydotAi::furniture_plan(Mind& m, const FurnitureTarget& t, const ai::Step& s,
                                                                 bool instant) {
    using ai::Procedure;
    const auto& markers = *t.markers;
    // The first free marker of the kind the step wants (sleep a bed's, sit and eat a seat's), else the first free.
    const int wants = s.procedure == Procedure::sleep ? 2 : (s.procedure == Procedure::sit || s.procedure == Procedure::eat) ? 1 : 0;
    int pick = -1;
    for (const bool strict : {true, false}) {
        for (std::size_t i = 0; i < markers.size() && pick < 0; ++i) {
            if (!furniture_claimed(t.ref, static_cast<int>(i), m.ref) &&
                (!strict || wants == 0 || markers[i].animation_type == wants)) {
                pick = static_cast<int>(i);
            }
        }
    }
    if (pick < 0) {
        return nullptr;
    }
    const Dictionary plan = plan_of(m.ref);
    const String behaviour = plan.get("behaviour", String());
    if (behaviour.is_empty()) {
        return nullptr;
    }
    auto u = std::make_shared<FurnitureUse>();
    u->def = world_->graph_library().graph(behaviour.utf8().get_data());
    if (u->def->empty()) {
        return nullptr;
    }
    u->scale = static_cast<double>(plan.get("scale", 1.0));
    u->ref = t.ref;
    u->marker = pick;
    const FurnitureMarker& marker = markers[static_cast<std::size_t>(pick)];
    u->anim_type = marker.animation_type;
    u->at = ai::marker_pose(t.pose, marker);
    u->entry = ai::choose_entry(marker, u->at, static_cast<double>(m.last.x), static_cast<double>(m.last.y));
    u->quick_enter = instant;
    u->state = ai::UseState::entering;
    m.furniture = u; // the conditions read the use from the mind
    u->enter_event = furniture_event(m);
    if (u->enter_event.empty()) {
        m.furniture.reset();
        return nullptr;
    }
    u->start = u->at;
    if (!instant) {
        // How far the enter moves the body, from the graph run alone: the entry point is that far back.
        const std::string key = u->def->behaviour + "|" + u->enter_event;
        auto it = enter_travels_.find(key);
        if (it == enter_travels_.end()) {
            ai::Pose travel;
            auto& library = world_->graph_library();
            if (!ai::enter_travel(u->def, [&library](const std::string& f) { return library.clip_duration(f); },
                                  u->enter_event, travel)) {
                m.furniture.reset();
                return nullptr;
            }
            it = enter_travels_.emplace(key, travel).first;
        }
        u->start = ai::enter_start(u->at, it->second, u->scale);
    }
    return u;
}

void SkydotAi::furniture_begin(Mind& m, SkydotActor& actor, bool instant) {
    FurnitureUse& u = *m.furniture;
    auto& library = world_->graph_library();
    u.run = std::make_unique<ai::GraphRun>(u.def, [&library](const std::string& f) { return library.clip_duration(f); }, u.scale);
    u.quick_enter = instant;
    const ai::Pose from = instant ? u.at : u.start;
    u.run->set_pose(from);
    actor.set_scripted(true);
    u.begun = true;
    u.reported = true;
    u.state = ai::UseState::entering;
    const std::vector<std::string> events = u.run->enter(u.enter_event);
    if (!u.run->in_furniture()) {
        furniture_end(m, &actor, true); // the graph has no such enter: the actor stays as it is
        return;
    }
    furniture_events(m, actor, events);
    if (instant || u.run->settled()) {
        u.state = ai::UseState::in;
    }
    furniture_apply(m, actor);
    std::printf("furniture: 0x%X %s 0x%X marker %d entry 0x%X event %s\n", m.ref, instant ? "placed in" : "enters",
                u.ref, u.marker, static_cast<unsigned>(u.entry), u.enter_event.c_str());
}

bool SkydotAi::furniture_step(Mind& m, SkydotActor& actor, const ai::Step& s, double seconds) {
    const auto target = furniture_target(m, s);
    if (!target) {
        return false;
    }
    if (m.furniture && m.furniture->begun) {
        return true; // in use: furniture_tick has the actor
    }
    if (!m.furniture || m.furniture->ref != target->ref) {
        if (!furniture_plan(m, *target, s, m.placed)) {
            m.furniture_skip = target->ref; // nothing to play there
            return false;
        }
    }
    if (m.placed) {
        furniture_begin(m, actor, true);
        return true;
    }
    const FurnitureUse& u = *m.furniture;
    const Spot entry{m.space, Vector3(static_cast<float>(u.start.x), static_cast<float>(u.start.y), static_cast<float>(u.start.z)),
                     static_cast<float>(u.start.yaw), 0.0, u.ref};
    if (go(m, actor, entry, k_entry_reach, seconds)) {
        if (horizontal(m.last, entry.position) > k_enter_snap) {
            m.furniture_skip = target->ref; // the walk ended far from it: no enter from there
            m.furniture.reset();
            return false;
        }
        furniture_begin(m, actor, false);
    }
    return true;
}

// ---- in the furniture ------------------------------------------------------------------------

void SkydotAi::furniture_apply(Mind& m, SkydotActor& actor) {
    FurnitureUse& u = *m.furniture;
    const ai::Pose& p = u.run->pose();
    if (!u.released) {
        actor.place_body(SkydotWorld::skyrim_position(Vector3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z))),
                         -p.yaw);
    }
    if (const auto now = u.run->graph().playing()) {
        actor.play_scripted(String::utf8(now->file.c_str()), static_cast<double>(std::fabs(now->speed)),
                            now->mode == 1 || now->mode == 3, static_cast<double>(now->time));
    }
}

void SkydotAi::furniture_events(Mind& m, SkydotActor& actor, const std::vector<std::string>& events) {
    FurnitureUse& u = *m.furniture;
    for (const std::string& e : events) {
        if (same_event(e, "FallStart") && !u.released) {
            u.released = true;
            u.since_release = 0.0;
            actor.release_body();
        } else if ((same_event(e, "IdleBedGetUp") || same_event(e, "IdleChairGetUp")) && u.state == ai::UseState::leaving &&
                   !u.get_up_sent) {
            u.get_up_sent = true;
            if (vm_ != nullptr) {
                vm_->send_get_up(m.ref);
            }
        }
    }
}

void SkydotAi::furniture_exit(Mind& m, SkydotActor& actor) {
    FurnitureUse& u = *m.furniture;
    u.state = ai::UseState::leaving;
    u.quick_enter = false;
    u.exit_sent = true;
    const std::string event = furniture_event(m);
    if (event.empty() || !u.run->graph().set().handles(event)) {
        furniture_end(m, &actor, true); // no way to play the exit: out at once
        return;
    }
    std::printf("furniture: 0x%X gets up from 0x%X: %s\n", m.ref, u.ref, event.c_str());
    furniture_events(m, actor, u.run->send(event));
}

bool SkydotAi::furniture_tick(Mind& m, SkydotActor& actor, double seconds) {
    if (!m.furniture || !m.furniture->begun) {
        return false;
    }
    FurnitureUse& u = *m.furniture;
    if (!furniture_wanted(m, u)) {
        u.exit_asked = true;
    }
    if (u.state == ai::UseState::entering && u.run->settled()) {
        u.state = ai::UseState::in;
    }
    if (u.exit_asked && u.state == ai::UseState::in && !u.exit_sent) {
        furniture_exit(m, actor);
        if (!m.furniture) {
            return false;
        }
    }
    furniture_events(m, actor, u.run->advance(seconds));
    if (u.state == ai::UseState::entering && u.run->settled()) {
        u.state = ai::UseState::in;
    }
    if (u.released) {
        u.since_release += seconds;
        // The engine tells the graph when the feet are down.
        if (!u.landed && u.since_release > 0.25 && actor.is_on_floor()) {
            u.landed = true;
            furniture_events(m, actor, u.run->send("LandStart"));
        }
    }
    if (!u.run->in_furniture()) {
        std::printf("furniture: 0x%X is out of 0x%X\n", m.ref, u.ref);
        furniture_end(m, &actor, false);
        return false;
    }
    furniture_apply(m, actor);
    return true;
}

void SkydotAi::furniture_end(Mind& m, SkydotActor* actor, bool failed) {
    if (m.furniture && failed) {
        m.furniture_skip = m.furniture->ref;
    }
    const bool begun = m.furniture && m.furniture->begun;
    m.furniture.reset();
    if (actor != nullptr && begun) {
        actor->set_scripted(false);
    }
    m.think = 0.0; // choose its package again
}

Dictionary SkydotAi::get_furniture_state(std::int64_t ref) {
    Dictionary out;
    Mind* m = mind(static_cast<std::uint32_t>(ref));
    if (m == nullptr || !m->furniture) {
        return out;
    }
    const FurnitureUse& u = *m->furniture;
    out["ref"] = static_cast<std::int64_t>(u.ref);
    out["marker"] = u.marker;
    out["entry"] = static_cast<std::int64_t>(u.entry);
    out["anim_type"] = u.anim_type;
    out["event"] = String::utf8(u.enter_event.c_str());
    out["begun"] = u.begun;
    out["released"] = u.released;
    out["state"] = static_cast<std::int64_t>(u.released || !u.reported ? 0 : static_cast<int>(u.state));
    out["start"] = Vector3(static_cast<float>(u.start.x), static_cast<float>(u.start.y), static_cast<float>(u.start.z));
    out["marker_position"] = Vector3(static_cast<float>(u.at.x), static_cast<float>(u.at.y), static_cast<float>(u.at.z));
    out["marker_yaw_degrees"] = u.at.yaw * 180.0 / std::numbers::pi;
    if (u.run) {
        const ai::Pose& p = u.run->pose();
        out["position"] = Vector3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
        out["yaw_degrees"] = p.yaw * 180.0 / std::numbers::pi;
        if (const auto now = u.run->graph().playing()) {
            out["clip"] = String::utf8(now->clip.c_str());
        }
    }
    return out;
}

} // namespace skydot
