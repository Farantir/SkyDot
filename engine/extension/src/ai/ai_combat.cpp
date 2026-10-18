// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotAi's combat (GAMEPLAY-PLAN.md, 6h): a mind that sees an enemy (by its
// aggression and how it sees the other, combat_rules.hpp) sets its package aside
// and fights: it approaches, circles or backs off and asks the game layer to
// attack (`combat_attack`); with a bow it keeps its distance and shoots. It
// flees below its confidence's health fraction, searches the place it last saw
// the target for a while and then goes back to its package. The rules are
// combat_rules.hpp; this gathers the facts and steers. Log lines `combat: 0xA
// enters combat with 0xT`, `searches for`, `leaves combat`, `flees from`.
#include "ai/ai.hpp"

#include "actors/actor.hpp"
#include "data/actors.hpp"
#include "data/fb_search.hpp"
#include "physics/collision.hpp"
#include "vm/papyrus.hpp"

#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {
namespace {

constexpr std::uint32_t k_player_npc = 7;
constexpr std::uint32_t k_player_faction = 0xDB1;
/// Seconds between an actor's looks for an enemy, and between its combat decisions.
constexpr double k_detect_interval = 0.4;
constexpr double k_think_interval = 0.25;
/// PLACEHOLDER (A8, detection): the cone an actor sees in, degrees, and how high the heads are.
constexpr double k_view_cone_degrees = 190.0;
constexpr double k_player_head = 1.7;
constexpr double k_head_fraction = 0.93;
/// PLACEHOLDER: seconds out of sight before it searches, and how long it searches.
constexpr double k_lost_grace = 3.0;
constexpr double k_search_seconds = 10.0;
/// Allies this close to the victim of a hit (game units) may join.
constexpr double k_alarm_radius = 1024.0;
/// PLACEHOLDERS: the seconds between a melee swing and the next, and between arrows.
constexpr double k_attack_period = 1.4;
constexpr double k_shoot_period = 3.0;
/// PLACEHOLDER: the radius of a body in game units, taken off distances.
constexpr double k_body_radius = 21.0;
/// PLACEHOLDER: how far it backs off or circles, as game units.
constexpr double k_fallback_distance = 300.0;
constexpr double k_keep_distance = 700.0;
constexpr double k_circle_angle = 0.7;
/// Fallbacks where a GMST is missing.
constexpr double k_combat_distance = 141.0;
constexpr double k_unarmed_reach = 96.0;
constexpr double k_sneak_distance = 2500.0;
constexpr double k_exterior_mult = 2.1;
constexpr double k_flee_interior = 3000.0;
constexpr double k_flee_exterior = 5000.0;
constexpr double k_regard = 0.8;
/// PLACEHOLDER: how close the player must come (game units) to wake a sleeping actor.
constexpr double k_wake_distance = 400.0;

double flat(const Vector3& a, const Vector3& b) {
    const double dx = static_cast<double>(a.x - b.x);
    const double dy = static_cast<double>(a.y - b.y);
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

std::uint32_t SkydotAi::npc_of(std::uint32_t ref) const {
    if (ref == k_player_ref) {
        return k_player_npc;
    }
    const auto it = minds_.find(ref);
    if (it == minds_.end() || root() == nullptr) {
        return 0;
    }
    const std::uint32_t npc = pick_actor_form(*root(), it->second.npc, ref);
    return npc != 0 ? npc : it->second.npc;
}

std::vector<combat_ai::Membership> SkydotAi::memberships(std::uint32_t ref) const {
    std::vector<combat_ai::Membership> out;
    // SNAM comes from the template chain when the NPC uses its template's factions.
    const auto* npc = root() != nullptr ? resolve_npc(*root(), npc_of(ref), wfb::NpcTemplateFlags::use_factions, ref)
                                        : nullptr;
    if (npc != nullptr && npc->factions() != nullptr) {
        for (const auto* f : *npc->factions()) {
            out.push_back({f->faction(), f->rank()});
        }
    }
    if (ref == k_player_ref) {
        const bool listed = std::ranges::any_of(out, [](const auto& m) { return m.faction == k_player_faction; });
        if (!listed) {
            out.push_back({k_player_faction, 0});
        }
    }
    return out;
}

combat_ai::Reaction SkydotAi::reaction_of(std::uint32_t a, std::uint32_t b) {
    if (!relationships_built_) {
        relationships_built_ = true;
        const auto* list = root() != nullptr ? root()->relationships() : nullptr;
        if (list != nullptr) {
            for (const auto* r : *list) {
                relationships_.try_emplace(static_cast<std::uint64_t>(r->parent()) << 32 | r->child(), r->rank());
                relationships_.try_emplace(static_cast<std::uint64_t>(r->child()) << 32 | r->parent(), r->rank());
            }
        }
    }
    std::optional<std::uint32_t> rank;
    if (const auto it = relationships_.find(static_cast<std::uint64_t>(npc_of(a)) << 32 | npc_of(b));
        it != relationships_.end()) {
        rank = it->second;
    }
    const auto relations = [this](std::uint32_t faction) {
        std::vector<combat_ai::FactionRelation> out;
        const auto* f = root() != nullptr ? lookup(root()->factions(), faction) : nullptr;
        if (f != nullptr && f->relations() != nullptr) {
            for (const auto* r : *f->relations()) {
                out.push_back({r->faction(), r->modifier(), r->reaction()});
            }
        }
        return out;
    };
    return combat_ai::reaction(memberships(a), memberships(b), rank, relations);
}

std::uint8_t SkydotAi::value_of(std::uint32_t ref, const char* actor_value) const {
    if (vm_ == nullptr) {
        return 0;
    }
    const double v = vm_->get_actor_value(ref, godot::String(actor_value));
    return static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0));
}

bool SkydotAi::alive(std::uint32_t ref) const {
    if (ref == k_player_ref) {
        return player_known_ && player_alive_;
    }
    return vm_ == nullptr || !vm_->is_actor_dead(ref);
}

std::optional<Vector3> SkydotAi::position_of(std::uint32_t ref) const {
    if (ref == k_player_ref) {
        return player_known_ ? std::optional<Vector3>(player_position_) : std::nullopt;
    }
    const auto it = minds_.find(ref);
    if (it == minds_.end() || it->second.node == 0) {
        return std::nullopt;
    }
    return it->second.last;
}

void SkydotAi::set_player(const Vector3& position, bool alive_now) {
    player_position_ = position;
    player_alive_ = alive_now;
    player_known_ = true;
}

double SkydotAi::aggro_radius(const Mind& m) const {
    const auto* npc = resolve_npc(*root(), npc_of(m.ref), wfb::NpcTemplateFlags::use_ai_data, m.ref);
    if (npc == nullptr || npc->aggro_radius_behaviour() == 0) {
        return 0.0;
    }
    return static_cast<double>(npc->aggro_attack() != 0 ? npc->aggro_attack() : npc->aggro_warn_attack());
}

bool SkydotAi::sees(const Mind& m, const SkydotActor& actor, std::uint32_t target, bool cone, double aggro) const {
    const auto at = position_of(target);
    if (!at || !actor.is_inside_tree()) {
        return false;
    }
    const double scale = SkydotWorld::unit_scale();
    const bool interior = world_->data().cell_ptr(static_cast<std::uint32_t>(space_)) != nullptr;
    double range = k_sneak_distance;
    if (vm_ != nullptr) {
        range = static_cast<double>(world_->data().gmst_float("fSneakMaxDistance", static_cast<float>(range)));
        if (!interior) {
            range *= static_cast<double>(world_->data().gmst_float("fSneakExteriorDistanceMult",
                                                                    static_cast<float>(k_exterior_mult)));
        }
    }
    if (aggro > 0.0) {
        range = aggro;
    }
    const Vector3 from = actor.get_global_position();
    const Vector3 to = SkydotWorld::skyrim_position(*at);
    const double distance = static_cast<double>(from.distance_to(to));
    if (distance > range * scale) {
        return false;
    }
    // The cone, about the facing (-Z).
    Vector3 flat_to = to - from;
    flat_to.y = 0;
    const Vector3 forward = -actor.get_global_transform().basis.get_column(2);
    if (cone && flat_to.length() > 0.01F) {
        const double angle = static_cast<double>(forward.angle_to(flat_to.normalized()));
        if (angle > k_view_cone_degrees * 0.5 * std::numbers::pi / 180.0) {
            return false;
        }
    }
    // Head to head against the world; actors' own cylinders are not in the mask.
    auto* space = actor.get_world_3d()->get_direct_space_state();
    if (space == nullptr) {
        return true;
    }
    const Vector3 eye = from + Vector3(0, static_cast<float>(actor.get_height() * k_head_fraction), 0);
    const double target_height = target == k_player_ref ? k_player_head : 1.6;
    const Vector3 head = to + Vector3(0, static_cast<float>(target_height), 0);
    const auto query = godot::PhysicsRayQueryParameters3D::create(
        eye, head, physics_layer::world | physics_layer::terrain);
    (void)m;
    return space->intersect_ray(query).is_empty();
}

// ---- gear ---------------------------------------------------------------------

void SkydotAi::read_gear(Mind& m) {
    CombatMind& c = m.combat;
    c.gear_age = 0.0;
    c.has_melee = vm_->melee_weapon(m.ref) != 0;
    c.has_bow = !vm_->ranged_weapon(m.ref).is_empty();
    const double combat_distance =
        static_cast<double>(world_->data().gmst_float("fCombatDistance", static_cast<float>(k_combat_distance)));
    double reach = k_unarmed_reach;
    // The weapon in hand, or the best melee weapon it would draw.
    std::int64_t form = static_cast<std::int64_t>(vm_->equipment(m.ref).get("right", 0));
    if (form == 0 || c.ranged) {
        form = vm_->melee_weapon(m.ref);
    }
    if (form != 0) {
        const godot::Dictionary weapon = world_->get_weapon(form);
        if (!weapon.is_empty()) {
            reach = static_cast<double>(weapon.get("reach", 1.0)) * combat_distance;
        }
    } else {
        const godot::Dictionary plan = world_->get_actor_plan(m.ref);
        const godot::Dictionary race = world_->get_race(static_cast<std::int64_t>(plan.get("race", 0)));
        reach = static_cast<double>(race.get("unarmed_reach", k_unarmed_reach));
    }
    c.reach = reach;
    // ZNAM follows the AI data of the template chain (assumed: no template flag of its own).
    const auto* npc = resolve_npc(*root(), npc_of(m.ref), wfb::NpcTemplateFlags::use_ai_data, m.ref);
    const auto* style = npc != nullptr && npc->combat_style() != 0 ? lookup(root()->combat_styles(), npc->combat_style())
                                                                    : nullptr;
    c.style = {};
    if (style != nullptr) {
        c.style = {style->offensive_mult(), style->circle_mult(), style->fallback_mult(),
                   style->special_attack_mult()};
    }
    // Only a bow (no melee weapon to fight with) means ranged from the start.
    if (c.has_bow && !c.has_melee) {
        c.ranged = true;
    }
}

// ---- starting and ending ---------------------------------------------------------

void SkydotAi::enter_combat(Mind& m, SkydotActor& actor, std::uint32_t target) {
    CombatMind& c = m.combat;
    const bool was = c.state != 0;
    c.target = target;
    c.state = 1;
    c.since_seen = 0.0;
    c.searching = 0.0;
    c.cooldown = 0.5;
    c.think = 0.0;
    c.fleeing = false;
    if (const auto at = position_of(target)) {
        c.last_seen = *at;
    }
    if (was) {
        return;
    }
    c.was_wander = actor.get_wander();
    c.ranged = false;
    c.gear_age = 1e9;
    actor.set_wander(false);
    actor.stop();
    read_gear(m);
    std::printf("combat: 0x%X enters combat with 0x%X\n", m.ref, target);
    emit_signal("combat_state_changed", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(target),
                static_cast<std::int64_t>(1));
}

void SkydotAi::leave_combat(Mind& m, SkydotActor* actor) {
    CombatMind& c = m.combat;
    if (c.state == 0) {
        return;
    }
    const std::uint32_t target = c.target;
    c.state = 0;
    c.target = 0;
    c.fleeing = false;
    if (actor != nullptr) {
        actor->clear_look_target();
        actor->stop();
        actor->set_wander(c.was_wander);
    }
    // The package it set aside is chosen again.
    m.package = 0;
    m.think = 0.0;
    std::printf("combat: 0x%X leaves combat\n", m.ref);
    emit_signal("combat_state_changed", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(target),
                static_cast<std::int64_t>(0));
}

bool SkydotAi::start_combat(std::int64_t ref, std::int64_t target) {
    Mind* m = mind(static_cast<std::uint32_t>(ref));
    SkydotActor* actor = m != nullptr ? node_of(*m) : nullptr;
    if (actor == nullptr || !alive(static_cast<std::uint32_t>(ref)) || ref == target) {
        return false;
    }
    enter_combat(*m, *actor, static_cast<std::uint32_t>(target));
    return true;
}

void SkydotAi::stop_combat(std::int64_t ref) {
    if (Mind* m = mind(static_cast<std::uint32_t>(ref))) {
        leave_combat(*m, node_of(*m));
    }
}

void SkydotAi::update_fights(double seconds) {
    const std::vector<std::uint32_t> attached = attached_;
    for (const std::uint32_t ref : attached) {
        Mind* m = mind(ref);
        SkydotActor* actor = m != nullptr ? node_of(*m) : nullptr;
        if (actor == nullptr || m->combat.state == 0 || !actor->is_inside_tree()) {
            continue;
        }
        m->last = SkydotWorld::godot_to_skyrim(actor->get_global_position());
        combat_tick(*m, *actor, seconds);
    }
}

std::int64_t SkydotAi::get_reaction(std::int64_t a, std::int64_t b) {
    const auto known = [this](std::int64_t ref) {
        return ref == 0x14 || mind(static_cast<std::uint32_t>(ref)) != nullptr;
    };
    if (!known(a) || !known(b)) {
        return -1;
    }
    return static_cast<std::int64_t>(reaction_of(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)));
}

std::int64_t SkydotAi::get_combat_target(std::int64_t ref) {
    const auto it = minds_.find(static_cast<std::uint32_t>(ref));
    return it == minds_.end() ? 0 : it->second.combat.target;
}

std::int64_t SkydotAi::get_combat_state(std::int64_t ref) {
    const auto it = minds_.find(static_cast<std::uint32_t>(ref));
    return it == minds_.end() ? 0 : it->second.combat.state;
}

void SkydotAi::on_hit(std::int64_t attacker_id, std::int64_t target_id) {
    const auto attacker = static_cast<std::uint32_t>(attacker_id);
    const auto victim = static_cast<std::uint32_t>(target_id);
    if (!combat_enabled_ || attacker == victim || !alive(attacker)) {
        return;
    }
    Mind* v = mind(victim);
    SkydotActor* actor = v != nullptr ? node_of(*v) : nullptr;
    if (actor == nullptr || !alive(victim)) {
        return;
    }
    if (v->combat.state == 0) {
        enter_combat(*v, *actor, attacker);
    }
    const auto victim_at = position_of(victim);
    for (const std::uint32_t ref : attached_) {
        Mind* a = mind(ref);
        if (a == nullptr || ref == attacker || ref == victim || a->combat.state != 0 || !alive(ref)) {
            continue;
        }
        SkydotActor* node = node_of(*a);
        const auto at = position_of(ref);
        if (node == nullptr || !at || !victim_at || flat(*at, *victim_at) > k_alarm_radius) {
            continue;
        }
        if (!combat_ai::assists(value_of(ref, "Assistance"), reaction_of(ref, victim)) ||
            combat_ai::friendly(reaction_of(ref, attacker))) {
            continue;
        }
        if (sees(*a, *node, attacker)) {
            enter_combat(*a, *node, attacker);
        }
    }
}

// ---- the step --------------------------------------------------------------------

void SkydotAi::combat_walk(Mind& m, SkydotActor& actor, const Vector3& skyrim_point, bool run) {
    CombatMind& c = m.combat;
    if (c.repath > 0.0 && actor.is_walking()) {
        return;
    }
    c.repath = 0.6;
    const Vector3 at = SkydotWorld::skyrim_position(skyrim_point);
    if (!actor.walk_to(at, run)) {
        actor.walk_straight(at, run); // off the navmesh (a big creature): straight at it
    }
}

void SkydotAi::melee_tick(Mind& m, SkydotActor& actor, double distance, const Vector3& to) {
    CombatMind& c = m.combat;
    combat_ai::MeleeInputs in;
    in.distance = distance - k_body_radius;
    in.reach = c.reach;
    in.attack_ready = c.cooldown <= 0.0;
    in.style = c.style;
    in.roll = roll(m);
    const Vector3 here = m.last;
    Vector3 away = here - to;
    away.z = 0;
    const double length = static_cast<double>(away.length());
    const Vector3 dir = length > 1.0 ? away / static_cast<float>(length) : Vector3(1, 0, 0);
    switch (combat_ai::melee_step(in)) {
    case combat_ai::MeleeStep::approach:
        combat_walk(m, actor, to, true);
        break;
    case combat_ai::MeleeStep::attack:
    case combat_ai::MeleeStep::power_attack: {
        const bool power = combat_ai::melee_step(in) == combat_ai::MeleeStep::power_attack;
        actor.stop();
        c.cooldown = k_attack_period;
        emit_signal("combat_attack", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target),
                    power ? "power" : "light");
        break;
    }
    case combat_ai::MeleeStep::circle: {
        // A point on the circle round the target, a step further along it.
        const double radius = std::max(c.reach * 0.9, 60.0);
        const double angle = std::atan2(static_cast<double>(dir.y), static_cast<double>(dir.x)) + c.circle * k_circle_angle;
        combat_walk(m, actor,
                    to + Vector3(static_cast<float>(std::cos(angle) * radius), static_cast<float>(std::sin(angle) * radius), 0),
                    false);
        break;
    }
    case combat_ai::MeleeStep::fallback:
        combat_walk(m, actor, here + dir * static_cast<float>(k_fallback_distance), false);
        break;
    case combat_ai::MeleeStep::hold:
        actor.stop();
        break;
    }
}

void SkydotAi::ranged_tick(Mind& m, SkydotActor& actor, double distance, const Vector3& to, bool los) {
    CombatMind& c = m.combat;
    combat_ai::RangedInputs in;
    in.distance = distance;
    in.line_of_sight = los;
    in.has_melee = c.has_melee;
    const Vector3 here = m.last;
    Vector3 away = here - to;
    away.z = 0;
    const double length = static_cast<double>(away.length());
    const Vector3 dir = length > 1.0 ? away / static_cast<float>(length) : Vector3(1, 0, 0);
    switch (combat_ai::ranged_step(in)) {
    case combat_ai::RangedStep::switch_to_melee:
        c.ranged = false;
        c.gear_age = 1e9;
        break;
    case combat_ai::RangedStep::keep_distance:
        combat_walk(m, actor, here + dir * static_cast<float>(k_keep_distance), true);
        break;
    case combat_ai::RangedStep::approach:
        combat_walk(m, actor, to, true);
        break;
    case combat_ai::RangedStep::shoot:
        actor.stop();
        if (c.cooldown <= 0.0) {
            c.cooldown = k_shoot_period;
            emit_signal("combat_attack", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target), "shoot");
        }
        break;
    }
}

void SkydotAi::flee_tick(Mind& m, SkydotActor& actor, const Vector3& from) {
    const bool interior = world_->data().cell_ptr(static_cast<std::uint32_t>(space_)) != nullptr;
    const double far = static_cast<double>(world_->data().gmst_float(
        interior ? "fFleeDistanceInterior" : "fFleeDistanceExterior",
        static_cast<float>(interior ? k_flee_interior : k_flee_exterior)));
    const double distance = flat(m.last, from);
    if (distance >= far * k_regard) {
        leave_combat(m, &actor);
        return;
    }
    Vector3 away = m.last - from;
    away.z = 0;
    const double length = static_cast<double>(away.length());
    const Vector3 dir = length > 1.0 ? away / static_cast<float>(length) : Vector3(1, 0, 0);
    combat_walk(m, actor, m.last + dir * static_cast<float>(std::min(far - distance, 1500.0)), true);
}

bool SkydotAi::combat_tick(Mind& m, SkydotActor& actor, double seconds) {
    CombatMind& c = m.combat;
    if (!combat_enabled_ || vm_ == nullptr) {
        return false;
    }
    c.cooldown -= seconds;
    c.repath -= seconds;
    c.detect -= seconds;
    c.think -= seconds;
    c.gear_age += seconds;
    if (!alive(m.ref)) {
        leave_combat(m, nullptr);
        return false;
    }
    if (c.state == 0) {
        if (c.detect > 0.0 || !player_known_) {
            return false;
        }
        c.detect = k_detect_interval;
        // A sleeper wakes when the player comes close (and it could see him): its scripts hear
        // OnGetUp, which for an ambush draugr (masterAmbushScript) sets Aggression 2.
        if (!m.woken && m.step < m.steps.size() && m.steps[m.step].procedure == ai::Procedure::sleep &&
            alive(k_player_ref) && sees(m, actor, k_player_ref, false, k_wake_distance)) {
            m.woken = true;
            if (vm_->send_get_up(m.ref) > 0) {
                std::printf("combat: 0x%X wakes as 0x14 comes close\n", m.ref);
            }
        }
        // The nearest one it sees and would fight.
        std::uint32_t best = 0;
        double best_distance = 1e18;
        const auto consider = [&](std::uint32_t other) {
            if (other == m.ref || !alive(other)) {
                return;
            }
            const auto at = position_of(other);
            if (!at) {
                return;
            }
            const double d = flat(m.last, *at);
            if (d >= best_distance) {
                return;
            }
            const auto how = reaction_of(m.ref, other);
            // An aggro radius attacks whatever comes within it and is not a friend (skeevers,
            // wolves, ambush draugr); otherwise aggression and the reaction decide.
            const double radius = aggro_radius(m);
            const bool by_radius = radius > 0.0 && !combat_ai::friendly(how) && sees(m, actor, other, false, radius);
            if (!by_radius &&
                (!combat_ai::starts_combat(value_of(m.ref, "Aggression"), how) || !sees(m, actor, other))) {
                return;
            }
            best = other;
            best_distance = d;
        };
        consider(k_player_ref);
        for (const std::uint32_t other : attached_) {
            consider(other);
        }
        if (best == 0) {
            return false;
        }
        {
            const auto why = reaction_of(m.ref, best);
            const char* names[] = {"enemy", "neutral", "ally", "friend"};
            std::printf("combat: 0x%X sees 0x%X: %s, aggression %d\n", m.ref, best,
                        names[static_cast<int>(why)], value_of(m.ref, "Aggression"));
        }
        enter_combat(m, actor, best);
        return true;
    }
    // In combat (1) or searching (2).
    if (!alive(c.target)) {
        leave_combat(m, &actor);
        return false;
    }
    if (c.think > 0.0) {
        return true;
    }
    c.think = k_think_interval;
    if (c.gear_age > 2.0) {
        read_gear(m);
    }
    const auto at = position_of(c.target);
    if (!at) {
        leave_combat(m, &actor);
        return false;
    }
    const bool in_sight = sees(m, actor, c.target);
    if (in_sight) {
        c.last_seen = *at;
        c.since_seen = 0.0;
        if (c.state == 2) {
            c.state = 1;
            c.searching = 0.0;
            emit_signal("combat_state_changed", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target),
                        static_cast<std::int64_t>(1));
        }
    } else {
        c.since_seen += k_think_interval;
    }
    if (c.state == 1 && !in_sight && c.since_seen > k_lost_grace) {
        c.state = 2;
        c.searching = 0.0;
        std::printf("combat: 0x%X searches for 0x%X\n", m.ref, c.target);
        emit_signal("combat_state_changed", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target),
                    static_cast<std::int64_t>(2));
    }
    if (c.state == 2) {
        c.searching += k_think_interval;
        actor.clear_look_target();
        if (c.searching > k_search_seconds) {
            leave_combat(m, &actor);
            return false;
        }
        combat_walk(m, actor, c.last_seen, false);
        return true;
    }
    actor.set_look_target(SkydotWorld::skyrim_position(*at));
    // Flee below the health fraction of its confidence.
    if (!c.fleeing) {
        const double max = vm_->get_actor_value_max(m.ref, "Health");
        const double fraction = max > 0.0 ? vm_->get_actor_value(m.ref, "Health") / max : 1.0;
        combat_ai::FleeSettings s;
        s.cautious = world_->data().gmst_float("fConfidenceCautious", s.cautious);
        s.average = world_->data().gmst_float("fConfidenceAverage", s.average);
        s.brave = world_->data().gmst_float("fConfidenceBrave", s.brave);
        if (fraction < static_cast<double>(combat_ai::flee_fraction(value_of(m.ref, "Confidence"), s))) {
            c.fleeing = true;
            std::printf("combat: 0x%X flees from 0x%X\n", m.ref, c.target);
        }
    }
    if (c.fleeing) {
        actor.clear_look_target();
        flee_tick(m, actor, *at);
        return c.state != 0;
    }
    const double distance = flat(m.last, *at);
    if (c.ranged || (c.has_bow && !c.has_melee)) {
        ranged_tick(m, actor, distance, *at, in_sight);
    } else {
        melee_tick(m, actor, distance, *at);
    }
    return true;
}

} // namespace skydot
