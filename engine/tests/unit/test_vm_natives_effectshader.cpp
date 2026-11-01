// SPDX-License-Identifier: GPL-3.0-or-later
//
// EffectShader natives (vm/natives_effectshader.cpp): what they tell the game layer.
#include "support/fake_events.hpp"
#include "support/vm_rig.hpp"

#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using skydot::vm::Op;

namespace {

void add_shader(Rig& rig) {
    rig.add({.name = "EffectShader",
             .functions = {native_function("Play", {{"target", "ObjectReference"}}, "None", false),
                           native_function("Stop", {{"target", "ObjectReference"}}, "None", false)}});
    auto play = global_function("play", {{"s", "EffectShader"}, {"t", "ObjectReference"}});
    play.add(Op::callmethod, {ident("Play"), ident("s"), ident("::NoneVar"), ident("t")});
    auto stop = global_function("stop", {{"s", "EffectShader"}, {"t", "ObjectReference"}});
    stop.add(Op::callmethod, {ident("Stop"), ident("s"), ident("::NoneVar"), ident("t")});
    rig.add(script("Caller", {play, stop}));
}

} // namespace

TEST_CASE("EffectShader.Play tells the game layer the shader and its target, as playing", "[vm][natives]") {
    FakeEvents events;
    Rig rig;
    add_shader(rig);
    skydot::vm::bind_effectshader_natives(rig.vm, events);

    rig.call("caller", "play", {rig.vm.object(0x30, "EffectShader"), rig.vm.object(0x40, "ObjectReference")});
    REQUIRE(events.shaders.size() == 1);
    CHECK(events.shaders[0].shader == 0x30);
    CHECK(events.shaders[0].target == 0x40);
    CHECK(events.shaders[0].play);
}

TEST_CASE("EffectShader.Stop sends the same notice as not playing", "[vm][natives]") {
    FakeEvents events;
    Rig rig;
    add_shader(rig);
    skydot::vm::bind_effectshader_natives(rig.vm, events);

    rig.call("caller", "stop", {rig.vm.object(0x30, "EffectShader"), rig.vm.object(0x40, "ObjectReference")});
    REQUIRE(events.shaders.size() == 1);
    CHECK(events.shaders[0].shader == 0x30);
    CHECK(events.shaders[0].target == 0x40);
    CHECK_FALSE(events.shaders[0].play);
}

TEST_CASE("an EffectShader played on None sends target 0", "[vm][natives]") {
    FakeEvents events;
    Rig rig;
    add_shader(rig);
    skydot::vm::bind_effectshader_natives(rig.vm, events);

    rig.call("caller", "play", {rig.vm.object(0x31, "EffectShader"), skydot::vm::Value()});
    REQUIRE(events.shaders.size() == 1);
    CHECK(events.shaders[0].target == 0);
    CHECK(rig.vm.errors() == 0);
}
