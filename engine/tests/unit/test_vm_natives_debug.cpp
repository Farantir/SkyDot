// SPDX-License-Identifier: GPL-3.0-or-later
//
// Debug natives and Message.Show (vm/natives_debug.cpp): what they ask of the
// world and what they send out, over FakeWorld and FakeEvents.
#include "support/fake_events.hpp"
#include "support/fake_world.hpp"
#include "support/vm_rig.hpp"

#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using skydot::vm::Op;

namespace {

void add_debug(Rig& rig) {
    rig.add({.name = "Debug",
             .functions = {native_function("Trace", {{"text", "String"}}),
                           native_function("Notification", {{"text", "String"}}),
                           native_function("MessageBox", {{"text", "String"}})}});
    rig.add({.name = "Message",
             .functions = {native_function("Show", {{"a", "Float"}, {"b", "Float"}}, "Int", false),
                           native_function("ShowAsHelpMessage", {}, "Int", false)}});
}

/// Caller.show(m, a, b) and Caller.help(m): m.Show(a, b) and m.ShowAsHelpMessage().
void add_callers(Rig& rig) {
    auto show = global_function("show", {{"m", "Message"}, {"a", "Float"}, {"b", "Float"}}, {{"r", "Int"}}, "Int");
    show.add(Op::callmethod, {ident("Show"), ident("m"), ident("r"), ident("a"), ident("b")})
        .add(Op::return_, {ident("r")});
    auto help = global_function("help", {{"m", "Message"}}, {{"r", "Int"}}, "Int");
    help.add(Op::callmethod, {ident("ShowAsHelpMessage"), ident("m"), ident("r")}).add(Op::return_, {ident("r")});
    rig.add(script("Caller", {show, help}));
}

skydot::vm::Value message(Rig& rig, std::uint32_t form) { return rig.vm.object(form, "Message"); }

} // namespace

TEST_CASE("Debug.Trace, Notification and MessageBox send their text, MessageBox as a box", "[vm][natives]") {
    FakeWorld world;
    FakeEvents events;
    Rig rig;
    add_debug(rig);
    skydot::vm::bind_debug_natives(rig.vm, world, events);

    auto f = global_function("run", {{"n", "Int"}}, {{"s", "String"}}, "None");
    call_static(f, "Debug", "Trace", {str("hello")});
    call_static(f, "Debug", "Notification", {str("note")});
    call_static(f, "Debug", "MessageBox", {str("box")});
    rig.add(script("Caller", {f}));
    rig.call("caller", "run", {Int(0)});

    REQUIRE(events.traces.size() == 1);
    CHECK(events.traces[0] == "hello");
    REQUIRE(events.messages.size() == 2);
    CHECK(events.messages[0].text == "note");
    CHECK_FALSE(events.messages[0].box);
    CHECK(events.messages[1].text == "box");
    CHECK(events.messages[1].box);
    CHECK_FALSE(events.messages[1].help);
}

TEST_CASE("Debug.Trace turns a non-string argument into text and none into an empty one", "[vm][natives]") {
    FakeWorld world;
    FakeEvents events;
    Rig rig;
    rig.add({.name = "Debug", .functions = {native_function("Trace", {{"n", "Int"}}), native_function("Notification")}});
    skydot::vm::bind_debug_natives(rig.vm, world, events);

    auto f = global_function("run");
    call_static(f, "Debug", "Trace", {integer(42)});
    call_static(f, "Debug", "Notification");
    rig.add(script("Caller", {f}));
    rig.call("caller", "run");

    REQUIRE(events.traces.size() == 1);
    CHECK(events.traces[0] == skydot::vm::Vm::to_string(Int(42)));
    REQUIRE(events.messages.size() == 1);
    CHECK(events.messages[0].text == skydot::vm::Vm::to_string(skydot::vm::Value()));
}

TEST_CASE("Message.Show fills the record's text, as a box only if the record is one", "[vm][natives]") {
    FakeWorld world;
    world.messages[0x50] = {.title = "Title", .text = "Gold: %.0f of %.0f", .box = true, .buttons = {"Ok"}};
    world.messages[0x51] = {.title = "Only a title", .text = "", .box = false, .buttons = {}};
    FakeEvents events;
    Rig rig;
    add_debug(rig);
    skydot::vm::bind_debug_natives(rig.vm, world, events);

    add_callers(rig);
    rig.call("caller", "show", {message(rig, 0x50), Float(3.0F), Float(10.0F)});
    rig.call("caller", "show", {message(rig, 0x51), Float(0.0F), Float(0.0F)});
    rig.call("caller", "help", {message(rig, 0x50)});

    REQUIRE(events.messages.size() == 3);
    CHECK(events.messages[0].text == "Gold: 3 of 10");
    CHECK(events.messages[0].box);
    CHECK(events.messages[1].text == "Only a title");
    CHECK_FALSE(events.messages[1].box);
    // Shown as help, a box message is a notification.
    CHECK_FALSE(events.messages[2].box);
    CHECK(events.messages[2].help);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("Message.Show on a form that is no message is the script's error and sends nothing", "[vm][natives]") {
    FakeWorld world;
    FakeEvents events;
    Rig rig;
    add_debug(rig);
    skydot::vm::bind_debug_natives(rig.vm, world, events);

    add_callers(rig);
    CHECK(rig.call("caller", "show", {message(rig, 0x99), Float(0.0F), Float(0.0F)}).i() == 0);
    CHECK(events.messages.empty());
    CHECK(rig.vm.errors() == 1);
}
