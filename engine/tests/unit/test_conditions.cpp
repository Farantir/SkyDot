// SPDX-License-Identifier: GPL-3.0-or-later
//
// The condition evaluator (conditions/conditions.cpp) and its bindings
// (conditions/functions.cpp) against a fake world: how a list is combined,
// whom a condition runs on, what an unbound function means, and the
// functions dialogue and quests lean on. The conditions travel in a built
// world.fb (support/world_builder.hpp), as they do in the game.
#include "support/fake_condition_world.hpp"
#include "support/world_builder.hpp"

#include "ai/packages.hpp"
#include "conditions/functions.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <utility>
#include <vector>

using namespace skydot::testing;
namespace conditions = skydot::conditions;
namespace fn = skydot::conditions::fn;

namespace {

/// The conditions of a package built from `list`, evaluated.
bool pass(std::vector<ConditionSpec> list, FakeConditionWorld& world,
          conditions::ConditionContext ctx = {}, conditions::ConditionOverrides* overrides = nullptr) {
    const BuiltWorld built(WorldSpec{.packages = {{.id = 1, .conditions = std::move(list)}}});
    const auto* package = skydot::ai::find_package(*built, 1);
    return conditions::conditions_pass(package->conditions(), world, ctx, overrides);
}

/// `function` compared with `value`.
ConditionSpec cond(std::uint16_t function, float value = 1.0F, std::uint32_t param1 = 0,
                   Compare compare = eq, std::uint8_t flags = 0) {
    return condition(function, compare, value, flags, param1);
}

/// Answers function 9999 with 1 and records whom it ran on.
class Recorder final : public conditions::ConditionOverrides {
public:
    std::vector<std::uint32_t> subjects;
    std::optional<double> function_value(const wfb::Condition& c, std::uint32_t subject,
                                         const conditions::ConditionContext&) override {
        if (c.function() != 9999) {
            return std::nullopt;
        }
        subjects.push_back(subject);
        return 1.0;
    }
};

} // namespace

TEST_CASE("comparisons use their operator", "[conditions]") {
    using conditions::compare;
    CHECK(compare(5.0, eq, 5.0));
    CHECK(compare(5.0, ne, 4.0));
    CHECK(compare(5.0, gt, 4.0));
    CHECK_FALSE(compare(5.0, gt, 5.0));
    CHECK(compare(5.0, ge, 5.0));
    CHECK(compare(4.0, lt, 5.0));
    CHECK(compare(5.0, le, 5.0));
    CHECK_FALSE(compare(6.0, le, 5.0));
}

TEST_CASE("ORed runs are ANDed", "[conditions]") {
    FakeConditionWorld w;
    w.globals[1] = 1.0;
    w.globals[2] = 0.0;
    const auto yes = [&] { return cond(fn::get_global_value, 1.0F, 1); };
    const auto no = [&] { return cond(fn::get_global_value, 1.0F, 2); };
    const auto or_with_next = [](ConditionSpec c) {
        c.type = static_cast<std::uint8_t>(c.type | k_or);
        return c;
    };
    CHECK(pass({}, w));
    // (no or yes) and yes
    CHECK(pass({or_with_next(no()), yes(), yes()}, w));
    // (no or no) and yes
    CHECK_FALSE(pass({or_with_next(no()), no(), yes()}, w));
    // yes and (no or yes)
    CHECK(pass({yes(), or_with_next(no()), yes()}, w));
    // A trailing OR closes its run at the end of the list.
    CHECK(pass({yes(), or_with_next(yes())}, w));
    CHECK_FALSE(pass({yes(), or_with_next(no())}, w));
}

TEST_CASE("the comparison value can be a GLOB", "[conditions]") {
    FakeConditionWorld w;
    w.globals[0x500] = 3.0;
    w.quests[0x10].stage = 3;
    ConditionSpec c = cond(fn::get_stage, 0.0F, 0x10, eq, k_use_global);
    c.value_global = 0x500;
    CHECK(pass({c}, w));
    w.globals[0x500] = 4.0;
    CHECK_FALSE(pass({c}, w));
}

TEST_CASE("a condition runs on the reference its run-on names", "[conditions]") {
    FakeConditionWorld w;
    Recorder r;
    w.links[{0x14, 0}] = 0x333;
    w.combat[0x14] = 0x444;
    w.aliases[{0x77, 4}] = 0x555;
    const conditions::ConditionContext ctx{.subject = 0x14, .target = 0x15, .quest = 0x77};
    const auto on = [&](std::uint32_t run_on, std::uint32_t reference = 0, std::int32_t alias = -1,
                        std::uint8_t flags = 0) {
        ConditionSpec c = cond(9999, 1.0F, 0, eq, flags);
        c.run_on = run_on;
        c.reference = reference;
        c.param3 = alias;
        return c;
    };
    CHECK(pass({on(0), on(1), on(2, 0x99), on(3), on(4), on(5, 0, 4), on(5, 0, 9), on(6), on(7)}, w, ctx, &r));
    CHECK(r.subjects == std::vector<std::uint32_t>{0x14, 0x15, 0x99, 0x444, 0x333, 0x555, 0, 0, 0});

    // The swap flag exchanges subject and target before a run-on 0 or 1.
    Recorder swapped;
    CHECK(pass({on(0, 0, -1, k_swap), on(1, 0, -1, k_swap)}, w, ctx, &swapped));
    CHECK(swapped.subjects == std::vector<std::uint32_t>{0x15, 0x14});
}

TEST_CASE("an unbound function is false and counted, or 0 under the zero policy", "[conditions]") {
    FakeConditionWorld w;
    std::vector<std::uint16_t> logged;
    w.unknowns().set_sink([&](std::uint16_t f) { logged.push_back(f); });
    CHECK_FALSE(pass({cond(9998, 0.0F)}, w)); // would hold if 0 compared with 0
    CHECK_FALSE(pass({cond(9998, 0.0F)}, w));
    CHECK_FALSE(pass({cond(9997, 0.0F)}, w));
    CHECK(w.unknowns().counts().at(9998) == 2);
    CHECK(w.unknowns().counts().at(9997) == 1);
    CHECK(logged == std::vector<std::uint16_t>{9998, 9997}); // one warning per index

    conditions::ConditionContext zero;
    zero.unknown = conditions::UnknownPolicy::zero;
    CHECK(pass({cond(9998, 0.0F)}, w, zero));
    CHECK_FALSE(pass({cond(9998, 1.0F)}, w, zero));
    CHECK(w.unknowns().counts().at(9998) == 4);
}

TEST_CASE("overrides answer before the bindings do", "[conditions]") {
    FakeConditionWorld w;
    w.quests[0x10].stage = 5;
    struct Fixed final : conditions::ConditionOverrides {
        std::optional<double> function_value(const wfb::Condition& c, std::uint32_t,
                                             const conditions::ConditionContext&) override {
            return c.function() == fn::get_stage ? std::optional<double>(9.0) : std::nullopt;
        }
    } fixed;
    CHECK(pass({cond(fn::get_stage, 5.0F, 0x10)}, w));
    CHECK_FALSE(pass({cond(fn::get_stage, 5.0F, 0x10)}, w, {}, &fixed));
    CHECK(pass({cond(fn::get_stage, 9.0F, 0x10)}, w, {}, &fixed));
}

TEST_CASE("systems that do not exist yet answer their resting value", "[conditions]") {
    FakeConditionWorld w;
    for (const std::uint16_t f : conditions::placeholder_functions()) {
        CAPTURE(f);
        CHECK(pass({cond(f, 0.0F)}, w));
    }
    CHECK(w.unknowns().counts().empty());
}

TEST_CASE("GetEquippedShout compares the voice slot with its parameter", "[conditions]") {
    FakeConditionWorld w;
    w.shouts[0x14] = 0x13E07;
    CHECK(pass({cond(fn::get_equipped_shout, 1.0F, 0x13E07)}, w, {.subject = 0x14}));
    CHECK_FALSE(pass({cond(fn::get_equipped_shout, 1.0F, 0x13E08)}, w, {.subject = 0x14}));
    CHECK(pass({cond(fn::get_equipped_shout, 0.0F, 0x13E08)}, w, {.subject = 0x14}));
    CHECK_FALSE(pass({cond(fn::get_equipped_shout, 1.0F, 0x13E07)}, w, {.subject = 0x15}));
    CHECK(pass({cond(fn::get_equipped_shout, 0.0F, 0x13E07)}, w, {.subject = 0x15}));
    CHECK(w.unknowns().counts().empty());
}

TEST_CASE("IsChild and IsUndead, GetRestrained rests at 0", "[conditions]") {
    FakeConditionWorld w;
    w.children.insert(0x20);
    w.keywords.insert({0x21, fn::k_actor_type_undead});
    w.keywords.insert({0x22, 0x66});
    CHECK(pass({cond(fn::is_child, 1.0F)}, w, {.subject = 0x20}));
    CHECK_FALSE(pass({cond(fn::is_child, 1.0F)}, w, {.subject = 0x21}));
    CHECK(pass({cond(fn::is_undead, 1.0F)}, w, {.subject = 0x21}));
    CHECK(pass({cond(fn::is_undead, 0.0F)}, w, {.subject = 0x22}));
    CHECK(pass({cond(fn::is_undead, 0.0F)}, w, {.subject = 0x20}));
    CHECK(pass({cond(fn::get_restrained, 0.0F)}, w, {.subject = 0x21}));
    CHECK(w.unknowns().counts().empty());
}

TEST_CASE("GetIsVoiceType takes the NPC's voice, else its race's, or a form list", "[conditions]") {
    FakeConditionWorld w;
    constexpr std::uint32_t k_male = 0xA1, k_female = 0xA2, k_own = 0xA3, k_list = 0xB1;
    w.races[0x20] = 0xC1;
    w.race_voices[0xC1] = {k_male, k_female};
    w.races[0x21] = 0xC1;
    w.females.insert(0x21);
    w.races[0x22] = 0xC1;
    w.voices[0x22] = k_own;
    w.lists[k_list] = {k_female, k_own};
    const auto is = [&](std::uint32_t subject, std::uint32_t voice) {
        return pass({cond(fn::get_is_voice_type, 1.0F, voice)}, w, {.subject = subject});
    };
    CHECK(is(0x20, k_male));
    CHECK_FALSE(is(0x20, k_female));
    CHECK(is(0x21, k_female)); // the race's female voice
    CHECK(is(0x22, k_own));    // its own, not the race's
    CHECK_FALSE(is(0x22, k_male));
    CHECK(is(0x21, k_list));
    CHECK(is(0x22, k_list));
    CHECK_FALSE(is(0x20, k_list));
    CHECK_FALSE(is(0x99, k_male)); // no voice at all
}

TEST_CASE("GetInCurrentLoc walks the location's parents", "[conditions]") {
    FakeConditionWorld w;
    w.cells[0x14] = 0x100;
    w.cell_locations[0x100] = 0x1; // Riverwood Trader
    w.location_parents[0x1] = 0x2; // Riverwood
    w.location_parents[0x2] = 0x3; // Whiterun Hold
    w.location_keywords.insert({0x1, 0x77});
    const auto in = [&](std::uint32_t loc) {
        return pass({cond(fn::get_in_current_loc, 1.0F, loc)}, w, {.subject = 0x14});
    };
    CHECK(in(0x1));
    CHECK(in(0x2));
    CHECK(in(0x3));
    CHECK_FALSE(in(0x4));
    CHECK(pass({cond(fn::location_has_keyword, 1.0F, 0x77)}, w, {.subject = 0x14}));
    CHECK_FALSE(pass({cond(fn::location_has_keyword, 1.0F, 0x78)}, w, {.subject = 0x14}));
    // Nowhere in particular: not in any location.
    CHECK_FALSE(pass({cond(fn::get_in_current_loc, 1.0F, 0x1)}, w, {.subject = 0x15}));
}

TEST_CASE("GetRelationshipRank maps RELA ranks to -4..4", "[conditions]") {
    FakeConditionWorld w;
    w.npcs[0x14] = 0x7;
    w.npcs[0x20] = 0x900;
    // RELA ranks as stored: 0 lover ... 4 acquaintance ... 8 archnemesis.
    const std::pair<std::int32_t, float> expected[] = {{0, 4.0F}, {1, 3.0F}, {2, 2.0F}, {3, 1.0F}, {4, 0.0F},
                                                      {5, -1.0F}, {6, -2.0F}, {7, -3.0F}, {8, -4.0F}};
    for (const auto& [raw, rank] : expected) {
        CAPTURE(raw);
        w.relationships[{0x900, 0x7}] = raw;
        CHECK(pass({cond(fn::get_relationship_rank, rank, 0x14)}, w, {.subject = 0x20}));
        CHECK(pass({cond(fn::get_relationship_rank, rank, 0x20)}, w, {.subject = 0x14})); // either way round
    }
    w.relationships.clear();
    CHECK(pass({cond(fn::get_relationship_rank, 0.0F, 0x14)}, w, {.subject = 0x20}));
}

TEST_CASE("GetOffersServicesNow follows a vendor faction's hours", "[conditions]") {
    FakeConditionWorld w;
    w.factions[0x20] = {{0xF1, 0}, {0xF2, 0}};
    w.vendors[0xF1] = {8, 20};
    const auto offers = [&](double hour) {
        w.now_hour = hour;
        return pass({cond(fn::get_offers_services_now, 1.0F)}, w, {.subject = 0x20});
    };
    CHECK(offers(8.0));
    CHECK(offers(19.5));
    CHECK_FALSE(offers(20.0));
    CHECK_FALSE(offers(7.9));
    w.vendors[0xF1] = {22, 4}; // past midnight
    CHECK(offers(23.0));
    CHECK(offers(3.0));
    CHECK_FALSE(offers(12.0));
    w.vendors[0xF1] = {0, 0}; // start == end: always
    CHECK(offers(12.0));
    w.vendors.clear(); // no vendor faction
    CHECK_FALSE(offers(12.0));
    CHECK_FALSE(pass({cond(fn::get_offers_services_now, 1.0F)}, w, {.subject = 0x99}));
}

TEST_CASE("quest, alias and variable functions", "[conditions]") {
    FakeConditionWorld w;
    w.quests[0x10] = {.stage = 20, .done = {10, 20}, .running = true, .completed = false};
    w.aliases[{0x10, 3}] = 0x42;
    w.variables["::count_var"] = 7.0;
    const conditions::ConditionContext ctx{.subject = 0x42, .quest = 0x10};
    CHECK(pass({cond(fn::get_stage, 20.0F, 0x10)}, w, ctx));
    CHECK(pass({cond(fn::get_stage_done, 1.0F, 0x10)}, w, ctx) == false); // param2 is 0: not done
    ConditionSpec done = cond(fn::get_stage_done, 1.0F, 0x10);
    done.param2 = 10;
    CHECK(pass({done}, w, ctx));
    CHECK(pass({cond(fn::get_quest_running, 1.0F, 0x10)}, w, ctx));
    CHECK(pass({cond(fn::get_quest_completed, 0.0F, 0x10)}, w, ctx));
    CHECK(pass({cond(fn::get_is_alias_ref, 1.0F, 3)}, w, ctx));
    CHECK_FALSE(pass({cond(fn::get_is_alias_ref, 1.0F, 4)}, w, ctx));
    ConditionSpec var = cond(fn::get_vm_quest_variable, 7.0F, 0x10);
    var.string2 = "::count_var";
    CHECK(pass({var}, w, ctx));
    var.string2 = "::missing_var";
    CHECK(pass({var, cond(fn::get_vm_quest_variable, 0.0F, 0x10)}, w, ctx) == false); // no string: 0 != 7
    CHECK_FALSE(pass({var}, w, ctx)); // a missing variable is 0
}

TEST_CASE("identity, faction, item and distance functions", "[conditions]") {
    FakeConditionWorld w;
    w.npcs[0x20] = 0x900;
    w.factions[0x20] = {{0xF1, 2}};
    w.items[{0x20, 0xF}] = 120;
    w.items[{0x20, 0x44}] = 3;
    w.keywords.insert({0x20, 0x66});
    w.dead.insert(0x21);
    w.races[0x20] = 0xC1;
    w.females.insert(0x20);
    w.links[{0x20, 0x55}] = 0x30;
    w.distances[{0x20, 0x30}] = 250.0;
    const conditions::ConditionContext ctx{.subject = 0x20};
    CHECK(pass({cond(fn::get_is_id, 1.0F, 0x900)}, w, ctx));
    CHECK(pass({cond(fn::get_is_id, 1.0F, 0x20)}, w, ctx));
    CHECK(pass({cond(fn::get_in_faction, 1.0F, 0xF1)}, w, ctx));
    CHECK(pass({cond(fn::get_in_faction, 0.0F, 0xF2)}, w, ctx));
    CHECK(pass({cond(fn::get_faction_rank, 2.0F, 0xF1)}, w, ctx));
    CHECK(pass({cond(fn::get_faction_rank, -1.0F, 0xF2)}, w, ctx));
    CHECK(pass({cond(fn::get_gold, 120.0F)}, w, ctx));
    CHECK(pass({cond(fn::get_item_count, 3.0F, 0x44)}, w, ctx));
    CHECK(pass({cond(fn::has_keyword, 1.0F, 0x66)}, w, ctx));
    CHECK(pass({cond(fn::get_dead, 1.0F)}, w, {.subject = 0x21}));
    CHECK(pass({cond(fn::get_is_race, 1.0F, 0xC1)}, w, ctx));
    CHECK(pass({cond(fn::get_is_sex, 1.0F, 1)}, w, ctx));
    CHECK(pass({cond(fn::has_linked_ref, 1.0F, 0x55)}, w, ctx));
    CHECK(pass({cond(fn::is_linked_to, 1.0F, 0x30)}, w, ctx) == false); // keyword is param2
    ConditionSpec linked = cond(fn::is_linked_to, 1.0F, 0x30);
    linked.param2 = 0x55;
    CHECK(pass({linked}, w, ctx));
    CHECK(pass({cond(fn::get_distance, 250.0F, 0x30)}, w, ctx));
    CHECK(pass({cond(fn::get_distance, 1.0e9F, 0x31)}, w, ctx)); // not in the same place: far
    // The player's sex and race answer for GetPCIs...
    w.females.insert(0x14);
    w.races[0x14] = 0xC2;
    CHECK(pass({cond(fn::get_pc_is_sex, 1.0F, 1)}, w, ctx));
    CHECK(pass({cond(fn::get_pc_is_race, 1.0F, 0xC2)}, w, ctx));
    // ...and a reference parameter may name an alias.
    w.aliases[{0x10, 1}] = 0x30;
    CHECK(pass({cond(fn::get_distance, 250.0F, 1, eq, k_use_aliases)}, w, {.subject = 0x20, .quest = 0x10}));
}

TEST_CASE("places and dialogue state", "[conditions]") {
    FakeConditionWorld w;
    w.cells[0x20] = 0x100;
    w.cells[0x21] = 0x200;
    w.interiors.insert(0x100);
    w.cell_worlds[0x200] = 0x3C;
    w.talked.insert(0x20);
    w.in_dialogue.insert(0x21);
    CHECK(pass({cond(fn::get_in_cell, 1.0F, 0x100)}, w, {.subject = 0x20}));
    CHECK(pass({cond(fn::is_in_interior, 1.0F)}, w, {.subject = 0x20}));
    CHECK(pass({cond(fn::is_in_interior, 0.0F)}, w, {.subject = 0x21}));
    CHECK(pass({cond(fn::get_in_worldspace, 1.0F, 0x3C)}, w, {.subject = 0x21}));
    CHECK_FALSE(pass({cond(fn::get_in_worldspace, 1.0F, 0x3C)}, w, {.subject = 0x20}));
    CHECK(pass({cond(fn::get_talked_to_pc, 1.0F)}, w, {.subject = 0x20}));
    CHECK(pass({cond(fn::is_in_dialogue_with_player, 1.0F)}, w, {.subject = 0x21}));
    CHECK(pass({cond(fn::is_in_dialogue_with_player, 0.0F)}, w, {.subject = 0x20}));
    w.now_hour = 14.5;
    w.now_day = 3;
    w.percent = 41.0;
    CHECK(pass({cond(fn::get_current_time, 14.5F), cond(fn::get_day_of_week, 3.0F),
                cond(fn::get_random_percent, 41.0F)}, w));
}
