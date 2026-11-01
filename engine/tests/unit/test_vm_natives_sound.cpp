// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sound natives (vm/natives_sound.cpp): playback ids and the notices sent.
#include "support/fake_events.hpp"
#include "support/fake_hosts.hpp"
#include "support/vm_rig.hpp"

#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using skydot::vm::Op;

namespace {

void add_sound(Rig& rig) {
    rig.add({.name = "Sound",
             .functions = {native_function("Play", {{"source", "ObjectReference"}}, "Int", false),
                           native_function("PlayAndWait", {{"source", "ObjectReference"}}, "Bool", false),
                           native_function("StopInstance", {{"instance", "Int"}}, "None", true),
                           native_function("SetInstanceVolume", {{"instance", "Int"}, {"volume", "Float"}}, "None", true)}});
    auto play = global_function("play", {{"s", "Sound"}, {"o", "ObjectReference"}}, {{"r", "Int"}}, "Int");
    play.add(Op::callmethod, {ident("Play"), ident("s"), ident("r"), ident("o")}).add(Op::return_, {ident("r")});
    auto wait = global_function("playwait", {{"s", "Sound"}, {"o", "ObjectReference"}}, {{"r", "Bool"}}, "Bool");
    wait.add(Op::callmethod, {ident("PlayAndWait"), ident("s"), ident("r"), ident("o")}).add(Op::return_, {ident("r")});
    auto stop = global_function("stop", {{"i", "Int"}});
    call_static(stop, "Sound", "StopInstance", {ident("i")});
    auto volume = global_function("volume", {{"i", "Int"}, {"v", "Float"}});
    call_static(volume, "Sound", "SetInstanceVolume", {ident("i"), ident("v")});
    rig.add(script("Caller", {play, wait, stop, volume}));
}

} // namespace

TEST_CASE("Sound.Play takes an instance id, sends it with sound and source and returns it", "[vm][natives]") {
    FakeEvents events;
    FakeSounds sounds;
    sounds.next = 7;
    Rig rig;
    add_sound(rig);
    skydot::vm::bind_sound_natives(rig.vm, sounds, events);

    const auto sound = rig.vm.object(0x60, "Sound");
    CHECK(rig.call("caller", "play", {sound, rig.vm.object(0x14, "ObjectReference")}).i() == 7);
    CHECK(rig.call("caller", "play", {sound, skydot::vm::Value()}).i() == 8);
    REQUIRE(events.played.size() == 2);
    CHECK(events.played[0].sound == 0x60);
    CHECK(events.played[0].source == 0x14);
    CHECK(events.played[0].instance == 7);
    CHECK(events.played[1].source == 0); // None plays non-positionally
    CHECK(events.played[1].instance == 8);
}

TEST_CASE("Sound.Play when the ids are used up returns 0 and plays nothing", "[vm][natives]") {
    FakeEvents events;
    FakeSounds sounds;
    sounds.exhausted = true;
    Rig rig;
    add_sound(rig);
    skydot::vm::bind_sound_natives(rig.vm, sounds, events);

    CHECK(rig.call("caller", "play", {rig.vm.object(0x60, "Sound"), skydot::vm::Value()}).i() == 0);
    CHECK(events.played.empty());

    // PlayAndWait does not wait either: it gives false.
    CHECK_FALSE(rig.call("caller", "playwait", {rig.vm.object(0x60, "Sound"), skydot::vm::Value()}).b());
    CHECK(events.played.empty());
}

TEST_CASE("Sound.PlayAndWait waits for the end notice, and sends play_sound once it is waiting", "[vm][natives]") {
    FakeEvents events;
    FakeSounds sounds;
    Rig rig;
    add_sound(rig);
    skydot::vm::bind_sound_natives(rig.vm, sounds, events);

    const auto thread = rig.vm.call_global(
        "caller", "playwait", {rig.vm.object(0x61, "Sound"), rig.vm.object(0x15, "ObjectReference")});
    rig.vm.run_ready();
    REQUIRE(events.played.size() == 1);
    CHECK(events.played[0].sound == 0x61);
    CHECK(events.played[0].source == 0x15);
    CHECK(events.played[0].instance == 1);
    CHECK_FALSE(rig.vm.take_result(thread).has_value());

    rig.vm.notify("sound:1");
    rig.vm.update(0.1);
    const auto done = rig.vm.take_result(thread);
    REQUIRE(done.has_value());
    CHECK(done->b());
}

TEST_CASE("Sound.PlayAndWait gives up when the clip never ends", "[vm][natives]") {
    FakeEvents events;
    FakeSounds sounds;
    Rig rig;
    add_sound(rig);
    skydot::vm::bind_sound_natives(rig.vm, sounds, events);

    const auto thread = rig.vm.call_global("caller", "playwait", {rig.vm.object(0x61, "Sound"), skydot::vm::Value()});
    rig.vm.run_ready();
    rig.vm.update(skydot::vm::k_sound_timeout - 1.0);
    CHECK_FALSE(rig.vm.take_result(thread).has_value());
    rig.vm.update(2.0);
    CHECK(rig.vm.take_result(thread).has_value());
}

TEST_CASE("StopInstance and SetInstanceVolume pass the instance and the volume on", "[vm][natives]") {
    FakeEvents events;
    FakeSounds sounds;
    Rig rig;
    add_sound(rig);
    skydot::vm::bind_sound_natives(rig.vm, sounds, events);

    rig.call("caller", "stop", {Int(4)});
    rig.call("caller", "volume", {Int(4), Float(0.5F)});
    REQUIRE(events.stopped.size() == 1);
    CHECK(events.stopped[0] == 4);
    REQUIRE(events.volumes.size() == 1);
    CHECK(events.volumes[0].first == 4);
    CHECK(events.volumes[0].second == 0.5);
}
