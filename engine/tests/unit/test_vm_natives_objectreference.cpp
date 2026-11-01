// SPDX-License-Identifier: GPL-3.0-or-later
//
// ObjectReference natives (vm/natives_objectreference.cpp), over FakeWorld,
// FakeEvents and FakeRefs.
#include "support/fake_events.hpp"
#include "support/fake_hosts.hpp"
#include "support/fake_world.hpp"
#include "support/native_calls.hpp"
#include "support/vm_rig.hpp"

#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

#include <numbers>

using namespace skydot::testing;
using skydot::vm::Value;

namespace {

constexpr double k_pi = std::numbers::pi;

/// ObjectReference methods, each reachable as Caller.<lowercase name>(o, ...).
void add_reference_classes(Rig& rig) {
    const auto ref = [](const char* n) { return Param{n, "ObjectReference"}; };
    rig.add({.name = "Form"});
    rig.add({.name = "Key", .parent = "Form"});
    rig.add({.name = "ObjectReference",
             .functions = {native("Enable", {}), native("Disable"), native("DisableNoWait"),
                           native("Reset", {ref("t")}), native("IsDisabled", {}, "Bool"),
                           native("IsEnabled", {}, "Bool"), native("Is3DLoaded", {}, "Bool"),
                           native("GetLinkedRef", {{"k", "Form"}}, "ObjectReference"),
                           native("GetBaseObject", {}, "Form"), native("GetPositionX", {}, "Float"),
                           native("GetAngleZ", {}, "Float"), native("GetScale", {}, "Float"),
                           native("GetDistance", {ref("t")}, "Float"), native("PlayAnimation", {{"a", "String"}}, "Bool"),
                           native("PlayAnimationAndWait", {{"a", "String"}, {"e", "String"}}, "Bool"),
                           native("ApplyHavokImpulse", {{"x", "Float"}, {"y", "Float"}, {"z", "Float"}, {"m", "Float"}}),
                           native("PushActorAway", {ref("a"), {"f", "Float"}}),
                           native("KnockAreaEffect", {{"m", "Float"}, {"r", "Float"}}),
                           native("SetMotionType", {{"t", "Int"}}), native("BlockActivation", {}),
                           native("BlockActivationOff", {}), native("IsActivationBlocked", {}, "Bool"),
                           native("SetOpen", {}), native("GetOpenState", {}, "Int"), native("Lock", {}),
                           native("IsLocked", {}, "Bool"), native("GetLockLevel", {}, "Int"),
                           native("GetKey", {}, "Key"),
                           native("MoveTo", {ref("t"), {"x", "Float"}, {"y", "Float"}, {"z", "Float"}}),
                           native("TranslateTo", {{"x", "Float"}, {"y", "Float"}, {"z", "Float"}, {"ax", "Float"},
                                                  {"ay", "Float"}, {"az", "Float"}}),
                           native("GetItemCount", {{"i", "Form"}}, "Int"),
                           native("AddItem", {{"i", "Form"}, {"n", "Int"}}),
                           native("RemoveItem", {{"i", "Form"}, {"n", "Int"}, {"s", "Bool"}, ref("to")}),
                           native("GetTriggerObjectCount", {}, "Int")}});
    // BlockActivation() with no arguments needs a call without them.
    auto block_default = global_function("blockdefault", {{"o", "ObjectReference"}});
    block_default.add(skydot::vm::Op::callmethod, {ident("BlockActivation"), ident("o"), ident("::NoneVar")});
    auto lock_default = global_function("lockdefault", {{"o", "ObjectReference"}});
    lock_default.add(skydot::vm::Op::callmethod, {ident("Lock"), ident("o"), ident("::NoneVar")});
    auto open_default = global_function("opendefault", {{"o", "ObjectReference"}});
    open_default.add(skydot::vm::Op::callmethod, {ident("SetOpen"), ident("o"), ident("::NoneVar")});
    auto move_default = global_function("movedefault", {{"o", "ObjectReference"}, {"t", "ObjectReference"}});
    move_default.add(skydot::vm::Op::callmethod, {ident("MoveTo"), ident("o"), ident("::NoneVar"), ident("t")});

    const auto m = [](const char* name, const char* fn, std::vector<Param> extra = {}, const char* ret = "None") {
        return wrap(name, "ObjectReference", fn, std::move(extra), ret);
    };
    rig.add(script("Caller",
                   {m("enable", "Enable"), m("disable", "Disable"), m("disablenowait", "DisableNoWait"),
                    m("reset", "Reset", {{"t", "ObjectReference"}}), m("isdisabled", "IsDisabled", {}, "Bool"),
                    m("isenabled", "IsEnabled", {}, "Bool"), m("is3d", "Is3DLoaded", {}, "Bool"),
                    m("linked", "GetLinkedRef", {{"k", "Form"}}, "ObjectReference"),
                    m("base", "GetBaseObject", {}, "Form"), m("posx", "GetPositionX", {}, "Float"),
                    m("anglez", "GetAngleZ", {}, "Float"), m("scale", "GetScale", {}, "Float"),
                    m("distance", "GetDistance", {{"t", "ObjectReference"}}, "Float"),
                    m("anim", "PlayAnimation", {{"a", "String"}}, "Bool"),
                    m("animwait", "PlayAnimationAndWait", {{"a", "String"}, {"e", "String"}}, "Bool"),
                    m("impulse", "ApplyHavokImpulse", {{"x", "Float"}, {"y", "Float"}, {"z", "Float"}, {"m", "Float"}}),
                    m("push", "PushActorAway", {{"a", "ObjectReference"}, {"f", "Float"}}),
                    m("knock", "KnockAreaEffect", {{"m", "Float"}, {"r", "Float"}}),
                    m("motion", "SetMotionType", {{"t", "Int"}}), m("blocked", "IsActivationBlocked", {}, "Bool"),
                    m("openstate", "GetOpenState", {}, "Int"), m("islocked", "IsLocked", {}, "Bool"),
                    m("locklevel", "GetLockLevel", {}, "Int"), m("key", "GetKey", {}, "Key"),
                    m("moveto", "MoveTo", {{"t", "ObjectReference"}, {"x", "Float"}, {"y", "Float"}, {"z", "Float"}}),
                    m("translate", "TranslateTo",
                      {{"x", "Float"}, {"y", "Float"}, {"z", "Float"}, {"ax", "Float"}, {"ay", "Float"}, {"az", "Float"}}),
                    m("count", "GetItemCount", {{"i", "Form"}}, "Int"),
                    m("additem", "AddItem", {{"i", "Form"}, {"n", "Int"}}),
                    m("removeitem", "RemoveItem",
                      {{"i", "Form"}, {"n", "Int"}, {"s", "Bool"}, {"to", "ObjectReference"}}),
                    m("triggers", "GetTriggerObjectCount", {}, "Int"), block_default, lock_default, open_default,
                    move_default}));
}

struct Fixture {
    FakeWorld world;
    FakeEvents events;
    FakeRefs host;
    Rig rig;
    Fixture() {
        add_reference_classes(rig);
        skydot::vm::bind_objectreference_natives(rig.vm, world, events, host);
    }
    Value ref(std::uint32_t form) { return rig.vm.object(form, "ObjectReference"); }
    Value call(const char* fn, std::vector<Value> args) { return rig.call("caller", fn, std::move(args)); }
};

} // namespace

TEST_CASE("Enable, Disable and DisableNoWait ask the host; IsDisabled and IsEnabled read the world", "[vm][natives]") {
    Fixture f;
    f.call("disable", {f.ref(0x10)});
    f.call("enable", {f.ref(0x11)});
    f.call("disablenowait", {f.ref(0x12)});
    REQUIRE(f.host.disabled_calls.size() == 3);
    CHECK(f.host.disabled_calls[0] == std::pair<std::uint32_t, bool>{0x10, true});
    CHECK(f.host.disabled_calls[1] == std::pair<std::uint32_t, bool>{0x11, false});
    CHECK(f.host.disabled_calls[2] == std::pair<std::uint32_t, bool>{0x12, true});

    f.world.disabled = {0x20};
    CHECK(f.call("isdisabled", {f.ref(0x20)}).b());
    CHECK_FALSE(f.call("isenabled", {f.ref(0x20)}).b());
    CHECK(f.call("isenabled", {f.ref(0x21)}).b());
    f.world.loaded_3d = {0x21};
    CHECK(f.call("is3d", {f.ref(0x21)}).b());
    CHECK_FALSE(f.call("is3d", {f.ref(0x20)}).b());
}

TEST_CASE("Reset without a target goes back to the placement and forgets the move", "[vm][natives]") {
    Fixture f;
    f.world.placements[0x30] = {.position = {1.0, 2.0, 3.0}, .rotation = {0.5, 0.0, 0.25}, .disabled = true};
    f.host.states.moved[0x30] = {};
    f.host.states.disabled[0x30] = false;

    f.call("reset", {f.ref(0x30), Value()});
    CHECK_FALSE(f.host.states.moved.contains(0x30));
    CHECK_FALSE(f.host.states.disabled.contains(0x30));
    REQUIRE(f.events.moved.size() == 1);
    CHECK(f.events.moved[0].ref == 0x30);
    CHECK(f.events.moved[0].position == skydot::vm::Xyz{1.0, 2.0, 3.0});
    CHECK(f.events.moved[0].rotation == skydot::vm::Xyz{0.5, 0.0, 0.25});
    REQUIRE(f.host.destruction_cleared.size() == 1);
    CHECK(f.host.destruction_cleared[0] == 0x30);
    // Enabled as placed: the placement says disabled.
    REQUIRE(f.host.disabled_calls.size() == 1);
    CHECK(f.host.disabled_calls[0] == std::pair<std::uint32_t, bool>{0x30, true});
}

TEST_CASE("Reset to a target moves to where the target is now and remembers it", "[vm][natives]") {
    Fixture f;
    f.world.positions[0x41] = {10.0, 20.0, 30.0};
    f.world.rotations[0x41] = {0.0, 0.0, 1.5};

    f.call("reset", {f.ref(0x40), f.ref(0x41)});
    const auto it = f.host.states.moved.find(0x40);
    REQUIRE(it != f.host.states.moved.end());
    CHECK(it->second.position[1] == 20.0);
    CHECK(it->second.rotation[2] == 1.5);
    REQUIRE(f.events.moved.size() == 1);
    CHECK(f.events.moved[0].position == skydot::vm::Xyz{10.0, 20.0, 30.0});
    // A reference the data does not know is enabled after the reset.
    CHECK(f.host.disabled_calls.back() == std::pair<std::uint32_t, bool>{0x40, false});
}

TEST_CASE("position, angle in degrees, scale (default 1) and distance come from the world", "[vm][natives]") {
    Fixture f;
    f.world.positions[0x50] = {3.0, 0.0, 0.0};
    f.world.positions[0x51] = {0.0, 4.0, 0.0};
    f.world.rotations[0x50] = {0.0, 0.0, k_pi / 2.0};
    f.world.placements[0x50] = {.scale = 2.5};

    CHECK(f.call("posx", {f.ref(0x50)}).f() == 3.0F);
    CHECK(f.call("anglez", {f.ref(0x50)}).f() == 90.0F);
    CHECK(f.call("scale", {f.ref(0x50)}).f() == 2.5F);
    CHECK(f.call("scale", {f.ref(0x99)}).f() == 1.0F);
    CHECK(f.call("distance", {f.ref(0x50), f.ref(0x51)}).f() == 5.0F);
    CHECK(f.call("distance", {f.ref(0x50), Value()}).f() == 0.0F);
}

TEST_CASE("GetLinkedRef takes the keyword (None: the plain link); GetBaseObject is the placement's base", "[vm][natives]") {
    Fixture f;
    f.world.placements[0x60] = {.base = 0x700};
    CHECK(f.call("base", {f.ref(0x60)}).form() == 0x700);
    CHECK(f.call("linked", {f.ref(0x1), Value()}).form() == 2); // FakeWorld: 1 -> 2
    CHECK(f.call("linked", {f.ref(0x1), f.rig.vm.object(0x500, "Form")}).kind() == skydot::vm::Kind::none);
    CHECK(f.call("linked", {f.ref(0x9), Value()}).kind() == skydot::vm::Kind::none);
}

TEST_CASE("animation notices: PlayAnimation at once, PlayAnimationAndWait once it is waiting", "[vm][natives]") {
    Fixture f;
    CHECK(f.call("anim", {f.ref(0x70), String("Open")}).b());
    REQUIRE(f.events.animations.size() == 1);
    CHECK(f.events.animations[0] == std::pair<std::uint32_t, std::string>{0x70, "Open"});

    const auto thread = f.rig.vm.call_global("caller", "animwait", {f.ref(0x71), String("Close"), String("done")});
    f.rig.vm.run_ready();
    REQUIRE(f.events.animations.size() == 2);
    CHECK(f.events.animations[1] == std::pair<std::uint32_t, std::string>{0x71, "Close"});
    CHECK_FALSE(f.rig.vm.take_result(thread).has_value());
    f.rig.vm.notify("anim:" + std::to_string(0x71) + ":done");
    f.rig.vm.update(0.1);
    REQUIRE(f.rig.vm.take_result(thread).has_value());
}

TEST_CASE("physics notices: impulse, push (not on None), knock and motion type", "[vm][natives]") {
    Fixture f;
    f.call("impulse", {f.ref(0x80), Float(1.5F), Float(0.0F), Float(-2.0F), Float(40.0F)});
    REQUIRE(f.events.impulses.size() == 1);
    CHECK(f.events.impulses[0].ref == 0x80);
    CHECK(f.events.impulses[0].direction == skydot::vm::Xyz{1.5, 0.0, -2.0});
    CHECK(f.events.impulses[0].magnitude == 40.0);

    f.call("push", {f.ref(0x80), Value(), Float(5.0F)});
    CHECK(f.events.pushes.empty());
    f.call("push", {f.ref(0x80), f.ref(0x81), Float(5.0F)});
    REQUIRE(f.events.pushes.size() == 1);
    CHECK(f.events.pushes[0].source == 0x80);
    CHECK(f.events.pushes[0].target == 0x81);
    CHECK(f.events.pushes[0].force == 5.0);

    f.call("knock", {f.ref(0x80), Float(2.0F), Float(300.0F)});
    REQUIRE(f.events.knocks.size() == 1);
    CHECK(f.events.knocks[0].radius == 300.0);
    f.call("motion", {f.ref(0x80), Int(4)});
    REQUIRE(f.events.motion_types.size() == 1);
    CHECK(f.events.motion_types[0] == std::pair<std::uint32_t, std::int32_t>{0x80, 4});
}

TEST_CASE("BlockActivation blocks by default and unblocks when told false", "[vm][natives]") {
    Fixture f;
    f.call("blockdefault", {f.ref(0x90)});
    CHECK(f.host.states.blocked.contains(0x90));
    CHECK(f.call("blocked", {f.ref(0x90)}).b());
    CHECK_FALSE(f.call("blocked", {f.ref(0x91)}).b());
}

TEST_CASE("doors and locks: SetOpen opens by default and tells the game layer; lock level is never negative",
          "[vm][natives]") {
    Fixture f;
    f.call("opendefault", {f.ref(0xA0)});
    CHECK(f.host.open[0xA0] == skydot::natives::k_open);
    REQUIRE(f.events.opened.size() == 1);
    CHECK(f.events.opened[0] == std::pair<std::uint32_t, bool>{0xA0, true});
    CHECK(f.call("openstate", {f.ref(0xA0)}).i() == 1);

    f.call("lockdefault", {f.ref(0xA1)});
    CHECK(f.host.locked[0xA1]);
    CHECK(f.call("islocked", {f.ref(0xA1)}).b());
    CHECK(f.call("locklevel", {f.ref(0xA1)}).i() == 0); // the host has no level: negative
    f.host.lock_levels[0xA1] = 50;
    CHECK(f.call("locklevel", {f.ref(0xA1)}).i() == 50);

    f.host.keys[0xA1] = 0xB00;
    CHECK(f.call("key", {f.ref(0xA1)}).form() == 0xB00);
    CHECK(f.call("key", {f.ref(0xA2)}).kind() == skydot::vm::Kind::none);
}

TEST_CASE("MoveTo needs a target and matches its rotation by default; TranslateTo puts the reference there at once",
          "[vm][natives]") {
    Fixture f;
    f.call("moveto", {f.ref(0xC0), Value(), Float(1.0F), Float(2.0F), Float(3.0F)});
    CHECK(f.host.moves.empty());
    f.call("movedefault", {f.ref(0xC0), f.ref(0xC1)});
    REQUIRE(f.host.moves.size() == 1);
    CHECK(f.host.moves[0].target == 0xC1);
    CHECK(f.host.moves[0].offset == skydot::vm::Xyz{0.0, 0.0, 0.0});
    CHECK(f.host.moves[0].match_rotation);
    f.call("moveto", {f.ref(0xC0), f.ref(0xC1), Float(1.0F), Float(2.0F), Float(3.0F)});
    CHECK(f.host.moves[1].offset == skydot::vm::Xyz{1.0, 2.0, 3.0});

    f.call("translate", {f.ref(0xC2), Float(10.0F), Float(20.0F), Float(30.0F), Float(0.0F), Float(0.0F), Float(180.0F)});
    REQUIRE(f.host.relocations.size() == 1);
    CHECK(f.host.relocations[0].position == skydot::vm::Xyz{10.0, 20.0, 30.0});
    CHECK(f.host.relocations[0].rotation[2] == static_cast<double>(static_cast<float>(k_pi)));
    REQUIRE(f.host.sent.size() == 1);
    CHECK(f.host.sent[0] == std::pair<std::uint32_t, std::string>{0xC2, "OnTranslationComplete"});
}

TEST_CASE("inventory: the owner is resolved first, AddItem adds one by default, RemoveItem hands on what was taken",
          "[vm][natives]") {
    Fixture f;
    const auto gold = f.rig.vm.object(0xF, "Form");
    f.call("additem", {f.ref(0xD0), gold, Int(5)});
    CHECK(f.call("count", {f.ref(0xD0), gold}).i() == 5);
    CHECK(f.call("count", {f.ref(0xD0), Value()}).i() == 0);
    CHECK_FALSE(f.host.resolved.empty());
    CHECK(f.host.resolved.front() == 0xD0);

    f.host.to_take = {{.extra = 0, .count = 2}, {.extra = 7, .count = 1}};
    f.call("removeitem", {f.ref(0xD0), gold, Int(3), Bool(false), f.ref(0xD1)});
    CHECK(f.rig.vm.inventory().count(0xD1, 0xF) == 3);
    CHECK(f.rig.vm.inventory().count(0xD0, 0xF) == 5); // the fake host took nothing from it itself
}

TEST_CASE("GetTriggerObjectCount asks the host", "[vm][natives]") {
    Fixture f;
    f.host.triggers[0xE0] = 3;
    CHECK(f.call("triggers", {f.ref(0xE0)}).i() == 3);
    CHECK(f.call("triggers", {f.ref(0xE1)}).i() == 0);
}
