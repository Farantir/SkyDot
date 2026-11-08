// SPDX-License-Identifier: GPL-3.0-or-later
//
// Choosing an idle for an action from the IDLE tree (ai/idle_choice.cpp): the root of the actor's
// behaviour file, depth first in sibling order, groups left when none of their leaves passes.
#include "support/fake_condition_world.hpp"
#include "support/world_builder.hpp"

#include "ai/idle_choice.hpp"
#include "conditions/functions.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
namespace conditions = skydot::conditions;
namespace fn = skydot::conditions::fn;
using skydot::ai::choose_idle;

namespace {

constexpr std::uint32_t k_action = 0x13;
constexpr std::uint32_t k_actor = 0x100;
constexpr std::uint32_t k_furniture = 0x200;

ConditionSpec cond(std::uint16_t function, float value, std::uint32_t param1 = 0) {
    return condition(function, eq, value, 0, param1);
}

WorldSpec tree() {
    WorldSpec spec;
    spec.format_version = skydot::WorldData::FORMAT_VERSION;
    spec.idles = {
        // Two roots under the action, one per behaviour graph; listed out of order.
        {.id = 0x20, .editor_id = "RootB", .behaviour = "Actors\\Other\\B.hkx", .parent = k_action, .previous = 0x10},
        {.id = 0x10, .editor_id = "RootA", .behaviour = "Actors\\Character\\Behaviors\\0_Master.hkx", .parent = k_action},
        // Under RootA: the chair group, the bed group, a group that passes with no passing leaf, and a last group.
        {.id = 0x30, .editor_id = "Chair", .parent = 0x10, .conditions = {cond(fn::is_furniture_anim_type, 1.0F, 1)}},
        {.id = 0x31, .editor_id = "Bed", .parent = 0x10, .previous = 0x30,
         .conditions = {cond(fn::is_furniture_anim_type, 1.0F, 2)}},
        {.id = 0x32, .editor_id = "Empty", .parent = 0x10, .previous = 0x31},
        {.id = 0x33, .editor_id = "Fallback", .parent = 0x10, .previous = 0x32},
        {.id = 0x40, .editor_id = "EnterChair", .event = "IdleChairEnterStart", .parent = 0x30,
         .conditions = {cond(fn::is_entering_interaction_quick, 0.0F)}},
        {.id = 0x41, .editor_id = "EnterChairInstant", .event = "IdleChairEnterInstant", .parent = 0x30, .previous = 0x40,
         .conditions = {cond(fn::is_entering_interaction_quick, 1.0F)}},
        {.id = 0x42, .editor_id = "BedEnter", .event = "IdleBedEnterStart", .parent = 0x31},
        {.id = 0x43, .editor_id = "NeverPasses", .event = "Never", .parent = 0x32,
         .conditions = {cond(fn::get_sleeping, 99.0F)}},
        {.id = 0x44, .editor_id = "Anything", .event = "IdleAnything", .parent = 0x33},
        {.id = 0x50, .editor_id = "OtherLeaf", .event = "OtherEvent", .parent = 0x20},
    };
    return spec;
}

} // namespace

TEST_CASE("the idle of an action comes from the root of the actor's behaviour", "[idles]") {
    const OpenWorld world(tree());
    REQUIRE(world.result.status == skydot::WorldData::Status::ok);
    FakeConditionWorld facts;
    conditions::ConditionContext context;
    context.subject = k_actor;
    context.target = k_furniture;
    const std::string master = "actors/character/behaviors/0_master.hkx"; // case and slashes do not matter

    facts.furniture_types[k_actor] = 1;
    auto choice = choose_idle(world.data, k_action, master, facts, context);
    REQUIRE(choice.has_value());
    CHECK(choice->idle == 0x40);
    CHECK(choice->event == "IdleChairEnterStart");

    facts.entering_quick.insert(k_actor);
    choice = choose_idle(world.data, k_action, master, facts, context);
    REQUIRE(choice.has_value());
    CHECK(choice->event == "IdleChairEnterInstant");

    facts.furniture_types[k_actor] = 2;
    choice = choose_idle(world.data, k_action, master, facts, context);
    REQUIRE(choice.has_value());
    CHECK(choice->event == "IdleBedEnterStart");

    // A type nothing matches: the Empty group passes, none of its leaves does, the next sibling wins.
    facts.furniture_types[k_actor] = 3;
    choice = choose_idle(world.data, k_action, master, facts, context);
    REQUIRE(choice.has_value());
    CHECK(choice->event == "IdleAnything");

    // Another behaviour graph has its own root.
    choice = choose_idle(world.data, k_action, "Actors\\Other\\B.hkx", facts, context);
    REQUIRE(choice.has_value());
    CHECK(choice->event == "OtherEvent");
    CHECK_FALSE(choose_idle(world.data, k_action, "Actors\\None.hkx", facts, context).has_value());
    CHECK_FALSE(choose_idle(world.data, 0x99, master, facts, context).has_value());
}
