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
#include "magic/magic_rules.hpp"
#include "physics/collision.hpp"
#include "vm/papyrus.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/object.hpp>
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
constexpr double k_player_view_degrees = 120.0;
// PLACEHOLDER: how long a shouting NPC inhales before the words (the clip's own length is the game
// layer's; this is the AI's wait).
constexpr double k_shout_inhale = 0.8;
constexpr double k_head_fraction = 0.93;
/// PLACEHOLDER: seconds out of sight before it searches, and how long it searches.
constexpr double k_lost_grace = 3.0;
constexpr double k_search_seconds = 10.0;
/// Allies this close to the victim of a hit (game units) may join.
constexpr double k_alarm_radius = 1024.0;
/// PLACEHOLDERS: the seconds between a melee swing and the next, and between arrows.
constexpr double k_attack_period = 1.4;
constexpr double k_shoot_period = 3.0;
/// PLACEHOLDER: seconds from a spell's release to the next cast, and the slack added to a spell's
/// casting time before the release (the game layer's hand has charged by then).
constexpr double k_cast_period = 1.5;
constexpr double k_release_slack = 0.1;
/// PLACEHOLDERS: reach of a touch spell, of an aimed spell whose projectile gives none, and the most
/// an aimed spell is cast from (game units).
constexpr double k_touch_reach = 128.0;
constexpr double k_aimed_reach = 1500.0;
constexpr double k_max_reach = 2500.0;
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
    // What scripts did to the actor's factions (SetFactionRank, RemoveFromFaction) wins over the NPC_.
    if (vm_ != nullptr) {
        const vm::ActorStates& states = vm_->actor_states();
        combat_ai::apply_scripted_memberships(
            out, [&](std::uint32_t faction) { return states.faction_override(ref, faction); }, states.added_factions(ref));
    }
    if (ref == k_player_ref) {
        const bool listed = std::ranges::any_of(out, [](const auto& m) { return m.faction == k_player_faction; });
        if (!listed) {
            out.push_back({k_player_faction, 0});
        }
    }
    return out;
}

const magic::Overrides& SkydotAi::overrides_of(std::uint32_t ref) const {
    const auto it = overrides_cache_.find(ref);
    if (it != overrides_cache_.end()) {
        return it->second;
    }
    return overrides_cache_.emplace(ref, vm_ != nullptr ? vm_->magic_overrides_of(ref) : magic::Overrides{})
        .first->second;
}

combat_ai::MagicState SkydotAi::magic_state(std::uint32_t ref) const {
    const magic::Overrides& o = overrides_of(ref);
    combat_ai::MagicState s;
    s.calm = o.calm;
    s.frenzy = o.frenzy;
    s.paralyzed = o.paralyzed;
    s.invisible = o.invisible;
    s.fear_from = o.fear_from;
    s.commanded_by = o.commanded_by;
    return s;
}

std::uint32_t SkydotAi::commander_of(std::uint32_t ref) const {
    return ref == k_player_ref ? 0 : overrides_of(ref).commanded_by;
}

godot::Dictionary SkydotAi::plan_of(std::uint32_t ref) const {
    godot::Dictionary plan = world_->get_actor_plan(ref);
    return plan.is_empty() ? world_->get_npc_plan(npc_of(ref), ref) : plan;
}

combat_ai::Reaction SkydotAi::reaction_of(std::uint32_t a, std::uint32_t b) {
    return combat_ai::commanded_reaction(
        a, b, [this](std::uint32_t ref) { return commander_of(ref); },
        [this](std::uint32_t x, std::uint32_t y) { return faction_reaction(x, y); });
}

combat_ai::Reaction SkydotAi::faction_reaction(std::uint32_t a, std::uint32_t b) {
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
    // A relationship a script set (Papyrus scale 4 .. -4) is the RELA rank 4 - r.
    if (vm_ != nullptr) {
        if (const auto set = vm_->actor_states().relationship_rank(a, b)) {
            rank = combat_ai::rela_rank_of(*set);
        }
    }
    const auto relations = [this](std::uint32_t faction) {
        std::vector<combat_ai::FactionRelation> out;
        const auto* f = root() != nullptr ? lookup(root()->factions(), faction) : nullptr;
        if (f != nullptr && f->relations() != nullptr) {
            for (const auto* r : *f->relations()) {
                out.push_back({r->faction(), r->modifier(), r->reaction()});
            }
        }
        // What SetEnemy / SetAlly said replaces the data's entry for that faction.
        if (vm_ != nullptr && vm_->actor_states().has_faction_reactions()) {
            out = combat_ai::with_scripted_relations(std::move(out), vm_->actor_states().faction_reactions_from(faction));
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

void SkydotAi::set_player_view(godot::Node3D* camera) {
    player_view_ = camera != nullptr ? static_cast<std::uint64_t>(camera->get_instance_id()) : 0;
}

bool SkydotAi::has_los(std::uint32_t ref, std::uint32_t target) const {
    const auto at = ref_spot(target);
    if (!at) {
        return false;
    }
    Vector3 eye;
    Vector3 forward;
    bool cone = false;
    godot::Node3D* node = nullptr;
    if (ref == k_player_ref) {
        node = godot::Object::cast_to<godot::Node3D>(godot::ObjectDB::get_instance(player_view_));
        if (node == nullptr || !node->is_inside_tree()) {
            return false;
        }
        eye = node->get_global_position();
        forward = -node->get_global_transform().basis.get_column(2);
        cone = true;
    } else {
        const auto it = minds_.find(ref);
        SkydotActor* actor = it != minds_.end() ? node_of(it->second) : nullptr;
        if (actor == nullptr || !actor->is_inside_tree()) {
            return false;
        }
        node = actor;
        eye = node->get_global_position() + Vector3(0, static_cast<float>(actor->get_height() * k_head_fraction), 0);
    }
    // An actor is looked at in the chest, anything else (a marker) where it stands.
    const bool actor_target = target == k_player_ref || minds_.contains(target);
    const Vector3 to = SkydotWorld::skyrim_position(at->position) +
                       Vector3(0, actor_target ? 1.4F : 0.0F, 0);
    // A (guess) 120 degrees of view for the player: the game's own cone is not in the data.
    if (cone && static_cast<double>(forward.angle_to((to - eye).normalized())) > k_player_view_degrees * 0.5 * std::numbers::pi / 180.0) {
        return false;
    }
    auto* space = node->get_world_3d()->get_direct_space_state();
    if (space == nullptr) {
        return true;
    }
    const auto query = godot::PhysicsRayQueryParameters3D::create(
        eye, to, physics_layer::world | physics_layer::terrain);
    return space->intersect_ray(query).is_empty();
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
    // An invisible target (the player too) is not seen at all.
    if (target != m.ref && overrides_of(target).invisible) {
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
        const godot::Dictionary plan = plan_of(m.ref);
        const godot::Dictionary race = world_->get_race(static_cast<std::int64_t>(plan.get("race", 0)));
        reach = static_cast<double>(race.get("unarmed_reach", k_unarmed_reach));
    }
    c.reach = reach;
    // A race with attacks in all directions (the dragon's bite, wings and tail) fights what is in its
    // reach before it shouts.
    {
        const godot::Dictionary plan = plan_of(m.ref);
        const godot::Dictionary race = world_->get_race(static_cast<std::int64_t>(plan.get("race", 0)));
        int directional = 0;
        for (const godot::Variant& v : static_cast<godot::Array>(race.get("attacks", godot::Array()))) {
            const godot::Dictionary a = v;
            directional += static_cast<double>(a.get("attack_angle", 0.0)) != 0.0 ? 1 : 0;
        }
        c.directional_melee = directional >= 4;
    }
    // ZNAM follows the AI data of the template chain (assumed: no template flag of its own).
    const auto* npc = resolve_npc(*root(), npc_of(m.ref), wfb::NpcTemplateFlags::use_ai_data, m.ref);
    const auto* style = npc != nullptr && npc->combat_style() != 0 ? lookup(root()->combat_styles(), npc->combat_style())
                                                                    : nullptr;
    c.style = {};
    c.weapon_style = {};
    if (style != nullptr) {
        c.style = {style->offensive_mult(), style->circle_mult(), style->fallback_mult(),
                   style->special_attack_mult()};
        c.weapon_style = {static_cast<double>(style->magic_score_mult()), static_cast<double>(style->melee_score_mult()),
                          static_cast<double>(style->ranged_score_mult()), static_cast<double>(style->unarmed_score_mult()),
                          static_cast<double>(style->shout_score_mult())};
    }
    if (style != nullptr) {
        c.flight_style = {static_cast<double>(style->hover_chance()),
                          static_cast<double>(style->dive_bomb_chance()),
                          static_cast<double>(style->ground_attack_chance()),
                          static_cast<double>(style->hover_time()),
                          static_cast<double>(style->ground_attack_time()),
                          static_cast<double>(style->flying_attack_chance())};
    }
    {
        const godot::Dictionary plan = plan_of(m.ref);
        const godot::Dictionary race = world_->get_race(static_cast<std::int64_t>(plan.get("race", 0)));
        const double injured = static_cast<double>(race.get("injured_health", 0.0));
        c.injured = injured > 0.0 ? injured : 0.35;
    }
    read_spells(m);
    // Only a bow (no melee weapon to fight with) means ranged from the start.
    if (c.has_bow && !c.has_melee) {
        c.ranged = true;
    }
}


// ---- magic ------------------------------------------------------------------------

void SkydotAi::read_spells(Mind& m) {
    CombatMind& c = m.combat;
    c.spells.clear();
    c.spells_read = true;
    const godot::PackedInt64Array known = vm_->spells_of(m.ref);
    for (const std::int64_t form : known) {
        const auto key = static_cast<std::uint32_t>(form);
        auto cached = spell_facts_.find(key);
        if (cached == spell_facts_.end()) {
            const godot::Dictionary info = vm_->cast_info(form);
            combat_ai::SpellFacts f;
            f.form = key;
            if (info.get("found", false)) {
                f.plain = static_cast<std::int64_t>(info.get("type", 0)) == 0 && !static_cast<bool>(info.get("is_scroll", false));
                f.concentration = static_cast<std::int64_t>(info.get("casting_type", 1)) == magic::casting::concentration;
                f.delivery = static_cast<std::uint32_t>(static_cast<std::int64_t>(info.get("delivery", 0)));
                f.hostile = info.get("hostile", false);
                f.recover = info.get("recover", false);
                f.archetype = static_cast<std::uint32_t>(static_cast<std::int64_t>(info.get("archetype", 0)));
                f.actor_value = static_cast<int>(static_cast<std::int64_t>(info.get("actor_value", -1)));
                double reach = 0.0;
                if (f.delivery == 1) {
                    reach = k_touch_reach;
                } else if (f.delivery == 2) {
                    const godot::Dictionary proj = world_->get_projectile(static_cast<std::int64_t>(info.get("projectile", 0)));
                    reach = proj.is_empty() ? 0.0 : static_cast<double>(proj.get("range", 0.0));
                    reach = reach > 0.0 ? std::min(reach, k_max_reach) : k_aimed_reach;
                } else if (f.delivery == 3) {
                    const double range = static_cast<double>(info.get("range", 0.0));
                    reach = range > 0.0 ? range : k_max_reach;
                }
                f.reach = reach;
            } else {
                f.plain = false;
            }
            cached = spell_facts_.emplace(key, f).first;
        }
        if (combat_ai::classify(cached->second) != combat_ai::SpellRole::other) {
            c.spells.push_back(cached->second);
        }
    }
    // The shouts: an NPC says all three words, so the spell of the last one and the recovery of the shout.
    c.shouts.clear();
    for (const std::int64_t shout : vm_->shouts_of(m.ref)) {
        const godot::Dictionary info = vm_->shout_info(m.ref, shout);
        const godot::Array words = info.get("words", godot::Array());
        if (words.is_empty()) {
            continue;
        }
        const godot::Dictionary last = words[words.size() - 1];
        const auto spell = static_cast<std::int64_t>(last.get("spell", 0));
        const godot::Dictionary cast = vm_->cast_info(spell);
        if (!cast.get("found", false)) {
            continue;
        }
        combat_ai::ShoutFacts f;
        f.form = static_cast<std::uint32_t>(shout);
        f.spell = static_cast<std::uint32_t>(spell);
        f.hostile = cast.get("hostile", false);
        f.recovery = static_cast<double>(last.get("recovery", 0.0));
        const auto delivery = static_cast<std::int64_t>(cast.get("delivery", 0));
        if (delivery == 2) {
            const godot::Dictionary proj = world_->get_projectile(static_cast<std::int64_t>(cast.get("projectile", 0)));
            const double range = proj.is_empty() ? 0.0 : static_cast<double>(proj.get("range", 0.0));
            f.reach = range > 0.0 ? std::min(range, k_max_reach) : k_aimed_reach;
        } else if (delivery == 3) {
            const double range = static_cast<double>(cast.get("range", 0.0));
            f.reach = range > 0.0 ? range : k_max_reach;
        }
        c.shouts.push_back(f);
    }
}

void SkydotAi::end_cast(Mind& m) {
    CombatMind& c = m.combat;
    if (!c.cast.active) {
        return;
    }
    c.cast.active = false;
    emit_signal("combat_cast", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target),
                static_cast<std::int64_t>(c.cast.spell), static_cast<std::int64_t>(1), "stop");
}

bool SkydotAi::cast_tick(Mind& m, double seconds) {
    CombatMind& c = m.combat;
    if (!c.cast.active) {
        return false;
    }
    auto& cast = c.cast;
    cast.elapsed += seconds;
    if (!cast.concentration) {
        if (cast.elapsed >= cast.charge + k_release_slack) {
            cast.active = false;
            c.cooldown = k_cast_period;
            std::printf("combat: 0x%X casts 0x%X\n", m.ref, cast.spell);
            emit_signal("combat_cast", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target),
                        static_cast<std::int64_t>(cast.spell), static_cast<std::int64_t>(1), "release");
        }
        return true;
    }
    const double magicka = vm_->get_actor_value(m.ref, "Magicka");
    bool done = cast.elapsed >= cast.hold || magicka < cast.cost * 0.1;
    if (cast.role == combat_ai::SpellRole::heal) {
        const double max = vm_->get_actor_value_max(m.ref, "Health");
        done = done || (max > 0.0 && vm_->get_actor_value(m.ref, "Health") / max >= combat_ai::k_healed_above);
    }
    if (done) {
        cast.active = false;
        c.cooldown = k_cast_period;
        std::printf("combat: 0x%X stops casting 0x%X\n", m.ref, cast.spell);
        emit_signal("combat_cast", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target),
                    static_cast<std::int64_t>(cast.spell), static_cast<std::int64_t>(1), "stop");
    }
    return true;
}

bool SkydotAi::shout_tick(Mind& m, double seconds) {
    CombatMind& c = m.combat;
    if (!c.shout.active) {
        return false;
    }
    c.shout.elapsed += seconds;
    if (c.shout.elapsed >= k_shout_inhale) {
        c.shout.active = false;
        c.cooldown = k_cast_period;
        c.aimed = 0.0;
        std::printf("combat: 0x%X says 0x%X\n", m.ref, c.shout.shout);
        emit_signal("combat_shout", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target),
                    static_cast<std::int64_t>(c.shout.shout), "release");
    }
    return true;
}

bool SkydotAi::shout_decide(Mind& m, SkydotActor& actor, double distance, const Vector3& to, bool los,
                            bool airborne) {
    CombatMind& c = m.combat;
    if (c.shouts.empty() || c.weapon_style.shout <= 0.0 || c.cast.active) {
        c.aimed = 0.0;
        c.facing = false;
        return false;
    }
    if (!airborne && c.directional_melee && distance - k_body_radius <= c.reach) {
        c.aimed = 0.0;
        c.facing = false;
        return false; // in reach of its bite, wings and tail
    }
    combat_ai::ShoutInputs in;
    in.shouts = c.shouts;
    in.recovering = vm_->voice_recovery(m.ref) > 0.0;
    in.line_of_sight = los;
    in.distance = distance;
    in.shout_mult = c.weapon_style.shout;
    in.roll = roll(m);
    // Would it shout if it were aimed? Then it stands and faces the target until it is.
    in.aimed = true;
    const std::uint32_t wanted = combat_ai::decide_shout(in);
    if (wanted == 0) {
        c.aimed = 0.0;
        c.facing = false;
        return false;
    }
    if (airborne) {
        // In the air the mover has aimed it (flight::breath_allowed) and it does not stand.
        c.shout = {true, wanted, 0.0};
        std::printf("combat: 0x%X shouts 0x%X at 0x%X (airborne)\n", m.ref, wanted, c.target);
        emit_signal("combat_shout", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target),
                    static_cast<std::int64_t>(wanted), "start");
        return true;
    }
    const auto& data = world_->data();
    const double tolerance = static_cast<double>(data.gmst_float("fAIShoutToleranceDegrees", 5.0F));
    const double min_aim = static_cast<double>(data.gmst_float("fAIShoutMinAimSeconds", 2.0F));
    Vector3 flat_to = SkydotWorld::skyrim_position(to) - actor.get_global_position();
    flat_to.y = 0;
    const Vector3 forward = -actor.get_global_transform().basis.get_column(2);
    const double angle = flat_to.length() > 0.01F
                             ? static_cast<double>(forward.angle_to(flat_to.normalized())) * 180.0 / std::numbers::pi
                             : 0.0;
    // The seconds count from the decision that found it facing the target, not from the one before.
    c.aimed = angle <= tolerance && c.facing ? c.aimed + k_think_interval : 0.0;
    c.facing = angle <= tolerance;
    in.aimed = combat_ai::shout_aimed(c.aimed, min_aim);
    const std::uint32_t shout = combat_ai::decide_shout(in);
    actor.stop();
    if (shout == 0) {
        return true; // faces the target until it has aimed long enough
    }
    c.shout = {true, shout, 0.0};
    std::printf("combat: 0x%X shouts 0x%X at 0x%X (aimed %.1f s)\n", m.ref, shout, c.target, c.aimed);
    emit_signal("combat_shout", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target),
                static_cast<std::int64_t>(shout), "start");
    return true;
}

bool SkydotAi::mage_tick(Mind& m, SkydotActor& actor, double distance, const Vector3& to, bool los) {
    CombatMind& c = m.combat;
    if (c.spells.empty()) {
        return false;
    }
    // What runs on it now decides which buffs it still needs.
    std::vector<std::uint32_t> running;
    for (const godot::Variant& v : vm_->active_effects(m.ref)) {
        const godot::Dictionary e = v;
        if (static_cast<std::int64_t>(e.get("caster", 0)) == static_cast<std::int64_t>(m.ref)) {
            running.push_back(static_cast<std::uint32_t>(static_cast<std::int64_t>(e.get("source", 0))));
        }
    }
    combat_ai::MageInputs in;
    in.spells = c.spells;
    for (auto& s : in.spells) {
        s.cost = vm_->spell_cost(m.ref, s.form);
        s.active = std::ranges::find(running, s.form) != running.end();
    }
    in.magicka = vm_->get_actor_value(m.ref, "Magicka");
    in.magicka_max = vm_->get_actor_value_max(m.ref, "Magicka");
    const double max_health = vm_->get_actor_value_max(m.ref, "Health");
    in.health_fraction = max_health > 0.0 ? vm_->get_actor_value(m.ref, "Health") / max_health : 1.0;
    in.distance = distance;
    in.line_of_sight = los;
    in.ready = c.cooldown <= 0.0;
    in.has_melee = c.has_melee;
    in.has_bow = c.has_bow;
    in.style = c.weapon_style;
    in.roll = roll(m);
    const combat_ai::MageDecision d = combat_ai::decide(in);
    const Vector3 here = m.last;
    Vector3 away = here - to;
    away.z = 0;
    const double length = static_cast<double>(away.length());
    const Vector3 dir = length > 1.0 ? away / static_cast<float>(length) : Vector3(1, 0, 0);
    switch (d.action) {
    case combat_ai::MageAction::weapons:
        return false;
    case combat_ai::MageAction::wait:
        actor.stop();
        return true;
    case combat_ai::MageAction::keep_distance:
        combat_walk(m, actor, here + dir * static_cast<float>(k_keep_distance), true);
        return true;
    case combat_ai::MageAction::approach: {
        const auto it = std::ranges::find_if(in.spells, [&](const auto& s) { return s.form == d.spell; });
        const double reach = it != in.spells.end() ? it->reach * combat_ai::k_reach_fraction : 0.0;
        // Walk to a point at that reach from the target, on this side of it.
        combat_walk(m, actor, to + dir * static_cast<float>(std::max(reach, 60.0)), true);
        return true;
    }
    case combat_ai::MageAction::cast: {
        const auto it = std::ranges::find_if(in.spells, [&](const auto& s) { return s.form == d.spell; });
        if (it == in.spells.end()) {
            return false;
        }
        actor.stop();
        auto& cast = c.cast;
        cast = {};
        cast.active = true;
        cast.spell = d.spell;
        cast.role = d.role;
        cast.concentration = it->concentration;
        cast.hold = d.hold;
        cast.cost = it->cost;
        const godot::Dictionary info = vm_->cast_info(static_cast<std::int64_t>(d.spell));
        cast.charge = it->concentration ? 0.0 : static_cast<double>(info.get("charge_time", 0.0));
        std::printf("combat: 0x%X %s 0x%X\n", m.ref, it->concentration ? "holds" : "charges", d.spell);
        emit_signal("combat_cast", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(c.target),
                    static_cast<std::int64_t>(d.spell), static_cast<std::int64_t>(1), "start");
        return true;
    }
    }
    return false;
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
    c.flight = {};
    c.flight_logged = -1;
    c.flight_log = 0.0;
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
    end_cast(m);
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
    overrides_cache_.clear();
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
    overrides_cache_.clear();
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
        if (attacker != k_player_ref && combat_ai::friendly(reaction_of(victim, attacker))) {
            std::printf("combat: 0x%X ignores a hit from its ally 0x%X\n", victim, attacker);
            return;
        }
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
    // The actors a commander commands fight whom it hits, and whoever hits it.
    for (const std::uint32_t ref : attached_) {
        Mind* a = mind(ref);
        const std::uint32_t master = commander_of(ref);
        SkydotActor* node = a != nullptr && master != 0 ? node_of(*a) : nullptr;
        if (node == nullptr || a->combat.state != 0 || !alive(ref) || overrides_of(ref).calm) {
            continue;
        }
        const std::uint32_t foe = master == attacker ? victim : master == victim ? attacker : 0;
        if (foe != 0 && foe != ref && commander_of(foe) != master && alive(foe) &&
            !combat_ai::friendly(reaction_of(ref, foe))) {
            enter_combat(*a, *node, foe);
        }
    }
}

double SkydotAi::stagger(std::int64_t ref_id, double magnitude) {
    const auto ref = static_cast<std::uint32_t>(ref_id);
    Mind* m = mind(ref);
    SkydotActor* actor = m != nullptr ? node_of(*m) : nullptr;
    if (actor == nullptr || !alive(ref)) {
        return 0.0;
    }
    const godot::Dictionary plan = plan_of(ref);
    const godot::Dictionary loco = world_->get_locomotion(plan.get("behaviour", godot::String()));
    const godot::Dictionary clips = loco.get("clips", godot::Dictionary());
    std::vector<std::string> names;
    for (const godot::Variant& key : clips.keys()) {
        names.emplace_back(godot::String(key).utf8().get_data());
    }
    const std::string name = combat_ai::stagger_clip(names, magnitude >= combat_ai::k_large_stagger);
    double length = 0.0;
    if (!name.empty()) {
        const godot::Dictionary clip = clips.get(godot::String::utf8(name.c_str()), godot::Dictionary());
        length = actor->play_action(clip.get("file", godot::String()), 1.0);
    }
    actor->stop();
    m->combat.stagger = length > 0.0 ? length : 1.0;
    std::printf("combat: 0x%X staggers (%g)\n", ref, magnitude);
    return m->combat.stagger;
}

bool SkydotAi::hold(std::int64_t ref_id, double seconds) {
    const auto ref = static_cast<std::uint32_t>(ref_id);
    Mind* m = mind(ref);
    SkydotActor* actor = m != nullptr ? node_of(*m) : nullptr;
    if (actor == nullptr || !alive(ref)) {
        return false;
    }
    actor->stop();
    m->combat.stagger = std::max(seconds, 0.0);
    return true;
}

bool SkydotAi::revive(std::int64_t ref_id) {
    const auto ref = static_cast<std::uint32_t>(ref_id);
    Mind* m = mind(ref);
    if (m == nullptr) {
        return false;
    }
    m->combat = CombatMind{};
    m->package = 0;
    m->steps.clear();
    m->step = 0;
    m->step_started = false;
    m->target.reset();
    m->door = 0;
    m->think = 0.0;
    m->idle = 0.0;
    SkydotActor* actor = node_of(*m);
    if (actor == nullptr) {
        return false;
    }
    actor->set_dead(false);
    actor->set_frozen(false);
    actor->set_wander(false);
    actor->clear_look_target();
    actor->stop();
    if (std::find(attached_.begin(), attached_.end(), ref) == attached_.end()) {
        attached_.push_back(ref);
    }
    std::printf("combat: 0x%X lives again\n", ref);
    return true;
}

// ---- magic ---------------------------------------------------------------------------

bool SkydotAi::follow_commander(Mind& m, SkydotActor& actor, std::uint32_t commander) {
    CombatMind& c = m.combat;
    const auto at = commander != 0 && alive(commander) ? position_of(commander) : std::nullopt;
    if (!at) {
        return false;
    }
    if (!c.following) {
        c.following = true;
        actor.set_wander(false);
    }
    const auto& data = world_->data();
    const double near = static_cast<double>(
        data.gmst_float("fSummonedCreatureMinFollowDist", static_cast<float>(combat_ai::k_follow_near)));
    const double far = static_cast<double>(
        data.gmst_float("fSummonedCreatureMaxFollowDist", static_cast<float>(combat_ai::k_follow_far)));
    switch (combat_ai::follow_step(flat(m.last, *at), near, far)) {
    case combat_ai::FollowStep::stay:
        if (actor.is_walking()) {
            actor.stop();
        }
        break;
    case combat_ai::FollowStep::walk:
        combat_walk(m, actor, *at, false);
        break;
    case combat_ai::FollowStep::run:
        combat_walk(m, actor, *at, true);
        break;
    }
    return true;
}

bool SkydotAi::magic_step(Mind& m, SkydotActor& actor, const combat_ai::MagicState& state, double seconds) {
    CombatMind& c = m.combat;
    const auto mode = combat_ai::magic_mode(state);
    // Paralysis: nothing is done, the combat state is kept.
    if (mode == combat_ai::MagicMode::frozen) {
        if (!c.paralyzed) {
            c.paralyzed = true;
            actor.stop();
            actor.set_frozen(true);
            std::printf("combat: 0x%X paralyzed\n", m.ref);
        }
        return true;
    }
    if (c.paralyzed) {
        c.paralyzed = false;
        actor.set_frozen(false);
        std::printf("combat: 0x%X moves again\n", m.ref);
    }
    // A stagger pauses the rest.
    if (c.stagger > 0.0) {
        c.stagger -= seconds;
        return true; // the clip plays, the AI waits
    }
    // Fear: it flees from the caster while the fear lasts.
    if (mode == combat_ai::MagicMode::afraid) {
        const auto from = alive(state.fear_from) ? position_of(state.fear_from) : std::nullopt;
        if (from) {
            if (c.afraid_of != state.fear_from) {
                c.afraid_of = state.fear_from;
                std::printf("combat: 0x%X flees from 0x%X (fear)\n", m.ref, state.fear_from);
                if (c.state == 0) {
                    enter_combat(m, actor, state.fear_from);
                    c.fear_entered = true;
                }
            }
            if (c.state != 0) {
                actor.clear_look_target();
                if (c.state == 2) {
                    c.state = 1;
                }
                flee_tick(m, actor, *from);
                if (c.state == 0) {
                    c.afraid_of = state.fear_from; // far enough: it waits, still afraid
                }
            }
            return true;
        }
    }
    if (c.afraid_of != 0) {
        // The fear is over: it fights what it would fight, else leaves.
        const std::uint32_t caster = c.afraid_of;
        c.afraid_of = 0;
        const bool entered = c.fear_entered;
        c.fear_entered = false;
        if (c.state != 0 && entered && c.target == caster &&
            !combat_ai::starts_combat(value_of(m.ref, "Aggression"), reaction_of(m.ref, caster))) {
            leave_combat(m, &actor);
        }
    }
    // Calm: no fight, none started.
    if (mode == combat_ai::MagicMode::calmed) {
        if (!c.calmed) {
            c.calmed = true;
            std::printf("combat: 0x%X calmed\n", m.ref);
        }
        if (c.state != 0) {
            leave_combat(m, &actor);
        }
        return false;
    }
    c.calmed = false;
    return false;
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

void SkydotAi::ranged_tick(Mind& m, SkydotActor& actor, double distance, const Vector3& to, bool los, bool airborne) {
    CombatMind& c = m.combat;
    combat_ai::RangedInputs in;
    in.distance = distance;
    in.line_of_sight = los;
    in.has_melee = c.has_melee;
    in.target_airborne = airborne;
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
    const combat_ai::MagicState magic = magic_state(m.ref);
    if (magic_step(m, actor, magic, seconds)) {
        end_cast(m);
        return true;
    }
    if (c.following && magic.commanded_by == 0) {
        // No longer commanded: its package again.
        c.following = false;
        m.package = 0;
        m.think = 0.0;
        actor.stop();
    }
    if (magic.frenzy) {
        c.frenzied = true;
    }
    if (c.frenzied && !magic.frenzy) {
        c.frenzied = false;
        if (c.state != 0 && !combat_ai::fights_after_frenzy(value_of(m.ref, "Aggression"),
                                                            reaction_of(m.ref, c.target))) {
            leave_combat(m, &actor);
        }
    }
    if (c.state == 0) {
        if (magic.calm) {
            return false;
        }
        if (c.detect > 0.0 || !player_known_) {
            return follow_commander(m, actor, magic.commanded_by);
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
            const auto how = combat_ai::reaction_under(magic, reaction_of(m.ref, other));
            // An aggro radius attacks whatever comes within it and is not a friend (skeevers,
            // wolves, ambush draugr); otherwise aggression and the reaction decide.
            const double radius = aggro_radius(m);
            const bool by_radius = radius > 0.0 && !combat_ai::friendly(how) && sees(m, actor, other, false, radius);
            if (!by_radius &&
                (!combat_ai::starts_combat(combat_ai::aggression_under(value_of(m.ref, "Aggression"), magic), how) ||
                 !sees(m, actor, other))) {
                return;
            }
            best = other;
            best_distance = d;
        };
        consider(k_player_ref);
        for (const std::uint32_t other : attached_) {
            consider(other);
        }
        // A commanded actor also fights whoever fights its commander.
        if (magic.commanded_by != 0 && !magic.frenzy) {
            const std::uint32_t master = magic.commanded_by;
            const auto help = [&](std::uint32_t other) {
                if (other == m.ref || other == master || !alive(other) || commander_of(other) == master ||
                    combat_ai::friendly(reaction_of(m.ref, other))) {
                    return;
                }
                if (const auto at = position_of(other); at && flat(m.last, *at) < best_distance) {
                    best = other;
                    best_distance = flat(m.last, *at);
                }
            };
            for (const std::uint32_t other : attached_) {
                const Mind* om = mind(other);
                if (om != nullptr && om->combat.state == 1 && om->combat.target == master) {
                    help(other);
                }
            }
            if (const Mind* cm = master == k_player_ref ? nullptr : mind(master);
                cm != nullptr && cm->combat.state == 1) {
                help(cm->combat.target);
            }
        }
        if (best == 0) {
            return follow_commander(m, actor, magic.commanded_by);
        }
        c.following = false;
        if (magic.frenzy) {
            std::printf("combat: 0x%X frenzied attacks 0x%X\n", m.ref, best);
        } else {
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
    if (c.state == 1 && actor.can_fly() && flight_combat(m, actor, seconds)) {
        return true; // the mover has the body
    }
    if (shout_tick(m, seconds)) {
        if (c.state != 1 || !position_of(c.target)) {
            c.shout.active = false;
        }
        return true; // it stands and shouts
    }
    if (cast_tick(m, seconds)) {
        if (c.state != 1 || !position_of(c.target)) {
            end_cast(m);
        }
        return true; // it stands and casts
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
        end_cast(m);
        actor.clear_look_target();
        flee_tick(m, actor, *at);
        return c.state != 0;
    }
    double distance = flat(m.last, *at);
    // A target in the air: a bow shoots it, everyone else without wings waits under it (a swing at
    // a dragon overhead only hits friends).
    bool airborne = false;
    if (!actor.can_fly()) {
        const Mind* foe_mind = mind(c.target);
        const SkydotActor* foe = foe_mind != nullptr ? node_of(*foe_mind) : nullptr;
        airborne = foe != nullptr && foe->is_flying();
    }
    if (airborne) {
        if (c.has_bow) {
            c.ranged = true;
            distance = static_cast<double>((*at - m.last).length());
        } else {
            constexpr double k_wait_under = 1024.0; // PLACEHOLDER: a melee fighter stops this far (flat) from a flier
            actor.set_look_target(SkydotWorld::skyrim_position(*at));
            if (distance > k_wait_under) {
                combat_walk(m, actor, *at, true);
            } else {
                actor.stop();
            }
            return true;
        }
    }
    if (shout_decide(m, actor, distance, *at, in_sight)) {
        return true;
    }
    if (mage_tick(m, actor, distance, *at, in_sight)) {
        return true;
    }
    if (c.ranged || (c.has_bow && !c.has_melee)) {
        ranged_tick(m, actor, distance, *at, in_sight, airborne);
    } else {
        melee_tick(m, actor, distance, *at);
    }
    return true;
}

} // namespace skydot
