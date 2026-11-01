// SPDX-License-Identifier: GPL-3.0-or-later
//
// Form natives (vm/natives_form.cpp): update timers and animation event
// registrations, over FakeWorld and FakeEvents.
#include "support/fake_events.hpp"
#include "support/fake_world.hpp"
#include "support/vm_rig.hpp"

#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using skydot::vm::Op;

namespace {

/// Form natives, and a script "Ticker" extending Form: Start/Once/Stop set
/// and clear its update timer, OnUpdate notes "tick", Watch(o) registers for
/// o's "Hit" animation event.
void add_ticker(Rig& rig) {
    rig.add({.name = "Form",
             .functions = {native_function("RegisterForUpdate", {{"s", "Float"}}, "None", false),
                           native_function("RegisterForSingleUpdate", {{"s", "Float"}}, "None", false),
                           native_function("UnregisterForUpdate", {}, "None", false),
                           native_function("GetFormID", {}, "Int", false),
                           native_function("RegisterForAnimationEvent", {{"o", "ObjectReference"}, {"e", "String"}},
                                           "Bool", false),
                           native_function("UnregisterForAnimationEvent", {{"o", "ObjectReference"}, {"e", "String"}},
                                           "None", false)}});
    auto start = method("Start", {{"s", "Float"}});
    start.add(Op::callmethod, {ident("RegisterForUpdate"), ident("self"), ident("::NoneVar"), ident("s")});
    auto once = method("Once", {{"s", "Float"}});
    once.add(Op::callmethod, {ident("RegisterForSingleUpdate"), ident("self"), ident("::NoneVar"), ident("s")});
    auto stop = method("Stop");
    stop.add(Op::callmethod, {ident("UnregisterForUpdate"), ident("self"), ident("::NoneVar")});
    auto tick = method("OnUpdate");
    note(tick, "tick");
    auto watch = method("Watch", {{"o", "ObjectReference"}}, {{"r", "Bool"}}, "Bool");
    watch.add(Op::callmethod, {ident("RegisterForAnimationEvent"), ident("self"), ident("r"), ident("o"), str("Hit")})
        .add(Op::return_, {ident("r")});
    auto id = method("Id", {}, {{"r", "Int"}}, "Int");
    id.add(Op::callmethod, {ident("GetFormID"), ident("self"), ident("r")}).add(Op::return_, {ident("r")});
    auto ticker = script("Ticker", {start, once, stop, tick, watch, id});
    ticker.parent = "Form";
    rig.add(ticker);
}

} // namespace

TEST_CASE("RegisterForUpdate sends OnUpdate every interval, RegisterForSingleUpdate once", "[vm][natives]") {
    FakeWorld world;
    FakeEvents events;
    Rig rig;
    add_ticker(rig);
    skydot::vm::bind_form_natives(rig.vm, world, events);
    auto* repeating = rig.vm.attach(0x70, "ticker");
    REQUIRE(repeating != nullptr);

    rig.call(repeating, "Start", {Float(2.0F)});
    rig.vm.update(1.9);
    CHECK(rig.notes.empty());
    rig.vm.update(0.2);
    CHECK(rig.notes.size() == 1);
    rig.vm.update(2.0);
    CHECK(rig.notes.size() == 2);

    rig.call(repeating, "Stop");
    rig.vm.update(10.0);
    CHECK(rig.notes.size() == 2);

    rig.notes.clear();
    rig.call(repeating, "Once", {Float(1.0F)});
    rig.vm.update(1.1);
    rig.vm.update(5.0);
    CHECK(rig.notes.size() == 1);
}

TEST_CASE("a negative update interval is taken as 0", "[vm][natives]") {
    FakeWorld world;
    FakeEvents events;
    Rig rig;
    add_ticker(rig);
    skydot::vm::bind_form_natives(rig.vm, world, events);
    auto* ticker = rig.vm.attach(0x70, "ticker");
    REQUIRE(ticker != nullptr);

    rig.call(ticker, "Once", {Float(-4.0F)});
    rig.vm.update(0.0);
    CHECK(rig.notes.size() == 1);
}

TEST_CASE("GetFormID gives the form's id", "[vm][natives]") {
    FakeWorld world;
    FakeEvents events;
    Rig rig;
    add_ticker(rig);
    skydot::vm::bind_form_natives(rig.vm, world, events);
    auto* ticker = rig.vm.attach(0x7A, "ticker");
    REQUIRE(ticker != nullptr);
    CHECK(rig.call(ticker, "Id").i() == 0x7A);
}

TEST_CASE("RegisterForAnimationEvent needs the sender's 3D, registers once and tells the game layer",
          "[vm][natives]") {
    FakeWorld world;
    world.loaded_3d = {0x80};
    FakeEvents events;
    Rig rig;
    add_ticker(rig);
    skydot::vm::bind_form_natives(rig.vm, world, events);
    auto* ticker = rig.vm.attach(0x70, "ticker");
    REQUIRE(ticker != nullptr);

    // No 3D: refused, nothing sent.
    CHECK_FALSE(rig.call(ticker, "Watch", {rig.vm.object(0x81, "ObjectReference")}).b());
    CHECK(events.animation_senders.empty());
    // None: refused.
    CHECK_FALSE(rig.call(ticker, "Watch", {skydot::vm::Value()}).b());

    CHECK(rig.call(ticker, "Watch", {rig.vm.object(0x80, "ObjectReference")}).b());
    REQUIRE(events.animation_senders.size() == 1);
    CHECK(events.animation_senders[0] == 0x80);
    CHECK(rig.vm.has_animation_event(ticker, 0x80, "hit"));
    CHECK(rig.vm.errors() == 0);
}
