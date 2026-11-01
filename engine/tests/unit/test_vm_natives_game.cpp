// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game natives (vm/natives_game.cpp): GetPlayer and GetForm, over FakeQuests.
#include "support/fake_hosts.hpp"
#include "support/vm_rig.hpp"

#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using skydot::vm::Op;

namespace {

void add_game(Rig& rig) {
    rig.add({.name = "Game",
             .functions = {native_function("GetPlayer", {}, "Actor"),
                           native_function("GetForm", {{"id", "Int"}}, "Form")}});
    rig.add({.name = "Form", .functions = {native_function("GetFormID", {}, "Int", false)}});
    rig.add({.name = "Actor", .parent = "Form"});
    rig.vm.bind("Form", "GetFormID", [](skydot::vm::NativeCall& c) {
        return skydot::vm::NativeResult{.value = Int(static_cast<std::int32_t>(c.self.form()))};
    });
    auto player = global_function("player", {}, {{"p", "Actor"}, {"r", "Int"}}, "Int");
    player.add(Op::callstatic, {ident("Game"), ident("GetPlayer"), ident("p")})
        .add(Op::callmethod, {ident("GetFormID"), ident("p"), ident("r")})
        .add(Op::return_, {ident("r")});
    auto form = global_function("form", {{"id", "Int"}}, {{"f", "Form"}, {"r", "Int"}}, "Int");
    form.add(Op::callstatic, {ident("Game"), ident("GetForm"), ident("f"), ident("id")})
        .add(Op::callmethod, {ident("GetFormID"), ident("f"), ident("r")})
        .add(Op::return_, {ident("r")});
    rig.add(script("Caller", {player, form}));
}

} // namespace

TEST_CASE("Game.GetPlayer is the player reference, 0x14", "[vm][natives]") {
    FakeQuests quests;
    Rig rig;
    add_game(rig);
    skydot::vm::bind_game_natives(rig.vm, quests);

    CHECK(rig.call("caller", "player").i() == 0x14);
    CHECK(quests.attached.empty());
}

TEST_CASE("Game.GetForm gives the form seen as Form, after asking for a quest's scripts", "[vm][natives]") {
    FakeQuests quests;
    Rig rig;
    add_game(rig);
    skydot::vm::bind_game_natives(rig.vm, quests);

    CHECK(rig.call("caller", "form", {Int(0x1234)}).i() == 0x1234);
    REQUIRE(quests.attached.size() == 1);
    CHECK(quests.attached[0].quest == 0x1234);
    CHECK(quests.attached[0].on_init);
}

TEST_CASE("Game.GetForm(0) is None and asks nothing; the id is taken as unsigned", "[vm][natives]") {
    FakeQuests quests;
    Rig rig;
    add_game(rig);
    skydot::vm::bind_game_natives(rig.vm, quests);

    // None.GetFormID() is the script's error; no quest was asked for.
    rig.call("caller", "form", {Int(0)});
    CHECK(quests.attached.empty());
    CHECK(rig.vm.errors() == 1);

    rig.call("caller", "form", {Int(-1)}); // 0xFFFFFFFF
    REQUIRE(quests.attached.size() == 1);
    CHECK(quests.attached[0].quest == 0xFFFFFFFFU);
}
