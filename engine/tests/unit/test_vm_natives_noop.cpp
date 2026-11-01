// SPDX-License-Identifier: GPL-3.0-or-later
//
// The natives that are meant to do nothing (vm/natives_noop.cpp).
#include "support/vm_rig.hpp"

#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using skydot::vm::Op;

TEST_CASE("an ignored native is silent and gives the default of its return type", "[vm][natives]") {
    Rig rig;
    rig.add({.name = "Keyword", .functions = {native_function("SendStoryEvent", {}, "Bool", true)}});
    skydot::vm::bind_noop_natives(rig.vm);

    auto f = global_function("run", {}, {{"r", "Bool"}}, "Bool");
    f.add(Op::callstatic, {ident("Keyword"), ident("SendStoryEvent"), ident("r")}).add(Op::return_, {ident("r")});
    rig.add(script("Caller", {f}));

    CHECK_FALSE(rig.call("caller", "run").b());
    CHECK(rig.vm.errors() == 0);
    CHECK(rig.log.empty());
}

TEST_CASE("the event registrations nothing raises are ignored on Form, Alias and ActiveMagicEffect", "[vm][natives]") {
    Rig rig;
    for (const char* cls : {"Form", "Alias", "ActiveMagicEffect"}) {
        rig.add({.name = cls, .functions = {native_function("RegisterForSleep", {}, "None", true)}});
    }
    skydot::vm::bind_noop_natives(rig.vm);

    auto f = global_function("run", {}, {}, "Int");
    for (const char* cls : {"Form", "Alias", "ActiveMagicEffect"}) {
        call_static(f, cls, "RegisterForSleep");
    }
    f.add(Op::return_, {integer(5)});
    rig.add(script("Caller", {f}));

    CHECK(rig.call("caller", "run").i() == 5);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("the logged natives are bound and return None", "[vm][natives]") {
    Rig rig;
    rig.add({.name = "ImageSpaceModifier",
             .functions = {native_function("Apply", {{"strength", "Float"}}, "None", false),
                           native_function("RemoveCrossFade", {}, "None", true)}});
    rig.add({.name = "ObjectReference", .functions = {native_function("AddToMap", {}, "None", false)}});
    skydot::vm::bind_noop_natives(rig.vm);

    auto f = global_function("run", {{"m", "ImageSpaceModifier"}, {"o", "ObjectReference"}}, {}, "Int");
    f.add(Op::callmethod, {ident("Apply"), ident("m"), ident("::NoneVar"), floating(1.0F)})
        .add(Op::callmethod, {ident("AddToMap"), ident("o"), ident("::NoneVar")});
    call_static(f, "ImageSpaceModifier", "RemoveCrossFade");
    f.add(Op::return_, {integer(1)});
    rig.add(script("Caller", {f}));

    CHECK(rig.call("caller", "run", {rig.vm.object(0x10, "ImageSpaceModifier"), rig.vm.object(0x11, "ObjectReference")})
              .i() == 1);
    CHECK(rig.vm.errors() == 0);
}
