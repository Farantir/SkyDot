// SPDX-License-Identifier: GPL-3.0-or-later
#include "conditions/functions.hpp"

#include "conditions/conditions.hpp"

#include "skydot_formats/flags.hpp"

#include <algorithm>
#include <array>

namespace skydot::conditions {
namespace {

constexpr std::uint32_t k_gold = 0xF;
constexpr double k_far = 1.0e9;
/// Locations nest a few levels; this only stops loops.
constexpr int k_max_location_depth = 32;

constexpr std::array<std::uint16_t, 14> k_placeholders{
    fn::get_equipped_shout,       fn::is_sneaking,          fn::is_in_combat,
    fn::is_weapon_out,            fn::is_weapon_magic_out,  fn::get_crime_gold,
    fn::is_trespassing,           fn::get_trespass_warning_level,
    fn::get_player_teammate,      fn::is_actor_a_victim,    fn::get_should_attack,
    fn::get_intimidate_success,   fn::get_bribe_success,    fn::get_detected,
};

double flag(bool value) { return value ? 1.0 : 0.0; }

/// Parameter 1 as a reference: with the "use aliases" flag it names an alias
/// of the context's quest.
std::uint32_t reference_param(const wfb::Condition& c, const ConditionContext& ctx,
                              const ConditionWorld& world) {
    if (formats::has_flag(static_cast<wfb::ConditionFlags>(c.type()), wfb::ConditionFlags::use_aliases)) {
        return world.alias_fill(ctx.quest, c.param1());
    }
    return c.param1();
}

bool in_location(const ConditionWorld& world, std::uint32_t location, std::uint32_t wanted) {
    for (int depth = 0; location != 0 && depth < k_max_location_depth; ++depth) {
        if (location == wanted) {
            return true;
        }
        location = world.location_parent(location);
    }
    return false;
}

/// The location of a subject: its cell's, or itself if it is one.
std::uint32_t location_of(const ConditionWorld& world, std::uint32_t subject) {
    const std::uint32_t cell = world.cell_of(subject);
    return cell != 0 ? world.cell_location(cell) : subject;
}

bool is_voice_type(ConditionWorld& world, std::uint32_t subject, std::uint32_t wanted) {
    std::uint32_t voice = world.voice_type_of(subject);
    if (voice == 0) {
        const auto [male, female] = world.race_voice_types(world.race_of(subject));
        voice = world.is_female(subject) ? female : male;
    }
    if (voice == 0) {
        return false;
    }
    if (voice == wanted) {
        return true;
    }
    std::vector<std::uint32_t> forms;
    return world.form_list(wanted, forms) && std::ranges::find(forms, voice) != forms.end();
}

bool offers_services(ConditionWorld& world, std::uint32_t subject) {
    const double hour = world.hour();
    for (const std::uint32_t faction : world.factions_of(subject)) {
        const auto hours = world.vendor_hours(faction);
        if (!hours) {
            continue;
        }
        const double start = hours->first;
        const double end = hours->second;
        if (start == end) { // Always open.
            return true;
        }
        const bool open = start < end ? hour >= start && hour < end : hour >= start || hour < end;
        if (open) {
            return true;
        }
    }
    return false;
}

/// RELA ranks run from 0 lover to 8 archnemesis; the condition's from 4 to -4.
double relationship_rank(ConditionWorld& world, std::uint32_t subject, std::uint32_t other) {
    const std::uint32_t a = world.npc_of(subject);
    const std::uint32_t b = world.npc_of(other);
    if (a == 0 || b == 0) {
        return 0.0;
    }
    const auto raw = world.relationship_rank(a, b);
    return raw ? 4.0 - static_cast<double>(*raw) : 0.0;
}

} // namespace

std::span<const std::uint16_t> placeholder_functions() { return k_placeholders; }

std::optional<double> function_value(const wfb::Condition& c, std::uint32_t subject,
                                     const ConditionContext& ctx, ConditionWorld& world) {
    const std::uint32_t p1 = c.param1();
    const std::uint32_t p2 = c.param2();
    switch (c.function()) {
    // ---- identity
    case fn::get_is_id:
        return flag(world.npc_of(subject) == p1 || subject == p1);
    case fn::get_is_reference:
        return flag(subject == reference_param(c, ctx, world));
    case fn::get_is_alias_ref:
        return flag(subject != 0 && subject == world.alias_fill(ctx.quest, p1));
    // ---- factions, race, sex, voice
    case fn::get_in_faction:
        return flag(world.faction_rank(subject, p1).has_value());
    case fn::get_faction_rank:
        return static_cast<double>(world.faction_rank(subject, p1).value_or(-1));
    case fn::get_is_race:
        return flag(world.race_of(subject) == p1);
    case fn::get_pc_is_race:
        return flag(world.race_of(world.player_ref()) == p1);
    case fn::get_is_sex:
        return flag((world.is_female(subject) ? 1U : 0U) == p1);
    case fn::get_pc_is_sex:
        return flag((world.is_female(world.player_ref()) ? 1U : 0U) == p1);
    case fn::get_is_voice_type:
        return flag(is_voice_type(world, subject, p1));
    case fn::has_keyword:
        return flag(world.has_keyword(subject, p1));
    case fn::get_level:
        return static_cast<double>(world.level(subject));
    // ---- quests and variables
    case fn::get_stage:
        return static_cast<double>(world.quest_stage(p1));
    case fn::get_stage_done:
        return flag(world.stage_done(p1, static_cast<std::int32_t>(p2)));
    case fn::get_quest_running:
        return flag(world.quest_running(p1));
    case fn::get_quest_completed:
        return flag(world.quest_completed(p1));
    case fn::get_global_value:
        return world.global_value(p1);
    case fn::get_vm_quest_variable:
        if (c.string2() == nullptr || c.string2()->size() == 0) {
            return 0.0;
        }
        return world.script_variable(p1, {}, c.string2()->str()).value_or(0.0);
    case fn::get_vm_script_variable: {
        if (c.string2() == nullptr || c.string2()->size() == 0) {
            return 0.0;
        }
        return world.script_variable(reference_param(c, ctx, world), {}, c.string2()->str()).value_or(0.0);
    }
    // ---- actor state
    case fn::get_dead:
        return flag(world.is_dead(subject));
    case fn::get_dead_count:
        return static_cast<double>(world.dead_count(p1));
    case fn::get_disabled:
        return flag(world.is_disabled(subject));
    case fn::get_actor_value:
        return world.actor_value(subject, p1);
    case fn::get_base_actor_value:
        return world.base_actor_value(subject, p1);
    case fn::get_actor_value_percent:
        return world.actor_value_percent(subject, p1);
    case fn::get_item_count:
        return static_cast<double>(world.item_count(subject, p1));
    case fn::get_gold:
        return static_cast<double>(world.item_count(subject, k_gold));
    // ---- places
    case fn::get_in_cell:
        return flag(world.cell_of(subject) == p1);
    case fn::is_in_interior: {
        const std::uint32_t cell = world.cell_of(subject);
        return flag(cell != 0 && world.cell_interior(cell));
    }
    case fn::get_in_worldspace: {
        const std::uint32_t cell = world.cell_of(subject);
        return flag(cell != 0 && !world.cell_interior(cell) && world.cell_world(cell) == p1);
    }
    case fn::get_distance:
        return world.distance(subject, reference_param(c, ctx, world)).value_or(k_far);
    case fn::get_in_current_loc:
        return flag(in_location(world, location_of(world, subject), p1));
    case fn::location_has_keyword: {
        const std::uint32_t location = location_of(world, subject);
        return flag(location != 0 && world.location_has_keyword(location, p1));
    }
    case fn::has_linked_ref:
        return flag(world.linked_ref(subject, p1) != 0);
    case fn::is_linked_to: {
        const std::uint32_t other = reference_param(c, ctx, world);
        return flag(other != 0 && world.linked_ref(subject, p2) == other);
    }
    // ---- relationships, vendors
    case fn::get_relationship_rank:
        return relationship_rank(world, subject, reference_param(c, ctx, world));
    case fn::get_offers_services_now:
        return flag(offers_services(world, subject));
    // ---- clock, chance
    case fn::get_current_time:
        return world.hour();
    case fn::get_day_of_week:
        return static_cast<double>(world.day_of_week());
    case fn::get_random_percent:
        return world.random_percent();
    // ---- dialogue
    case fn::get_talked_to_pc:
        return flag(world.talked_to_player(subject));
    case fn::is_in_dialogue_with_player:
        return flag(world.in_dialogue_with_player(subject));
    default:
        break;
    }
    // Systems that do not exist yet answer their resting value.
    if (std::ranges::find(k_placeholders, c.function()) != k_placeholders.end()) {
        return 0.0;
    }
    return std::nullopt;
}

} // namespace skydot::conditions
