// SPDX-License-Identifier: GPL-3.0-or-later
//
// Weapon natives (vm/natives_weapon.cpp): Fire tells the game layer who fires what.
#include "support/fake_events.hpp"
#include "support/vm_rig.hpp"

#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using skydot::vm::Op;

namespace {

void add_weapon(Rig& rig) {
    rig.add({.name = "Form"});
    rig.add({.name = "Weapon",
             .functions = {native_function("Fire", {{"source", "ObjectReference"}, {"ammo", "Form"}}, "None", false)}});
    auto fire = global_function("fire", {{"w", "Weapon"}, {"s", "ObjectReference"}, {"a", "Form"}});
    fire.add(Op::callmethod, {ident("Fire"), ident("w"), ident("::NoneVar"), ident("s"), ident("a")});
    auto bare = global_function("bare", {{"w", "Weapon"}});
    bare.add(Op::callmethod, {ident("Fire"), ident("w"), ident("::NoneVar")});
    rig.add(script("Caller", {fire, bare}));
}

} // namespace

TEST_CASE("Weapon.Fire sends the weapon, the source and the ammo", "[vm][natives]") {
    FakeEvents events;
    Rig rig;
    add_weapon(rig);
    skydot::vm::bind_weapon_natives(rig.vm, events);

    rig.call("caller", "fire",
             {rig.vm.object(0x20, "Weapon"), rig.vm.object(0x14, "ObjectReference"), rig.vm.object(0x21, "Form")});
    REQUIRE(events.shots.size() == 1);
    CHECK(events.shots[0].weapon == 0x20);
    CHECK(events.shots[0].source == 0x14);
    CHECK(events.shots[0].ammo == 0x21);
}

TEST_CASE("Weapon.Fire with None for source and ammo sends 0 for both", "[vm][natives]") {
    FakeEvents events;
    Rig rig;
    add_weapon(rig);
    skydot::vm::bind_weapon_natives(rig.vm, events);

    rig.call("caller", "fire", {rig.vm.object(0x20, "Weapon"), skydot::vm::Value(), skydot::vm::Value()});
    REQUIRE(events.shots.size() == 1);
    CHECK(events.shots[0].source == 0);
    CHECK(events.shots[0].ammo == 0);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("Weapon.Fire called with fewer arguments than it takes still fires, with 0 for the rest", "[vm][natives]") {
    FakeEvents events;
    Rig rig;
    add_weapon(rig);
    skydot::vm::bind_weapon_natives(rig.vm, events);

    rig.call("caller", "bare", {rig.vm.object(0x22, "Weapon")});
    REQUIRE(events.shots.size() == 1);
    CHECK(events.shots[0].weapon == 0x22);
    CHECK(events.shots[0].source == 0);
    CHECK(events.shots[0].ammo == 0);
}
