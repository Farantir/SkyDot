// SPDX-License-Identifier: GPL-3.0-or-later
//
// The behaviour state machine (actors/behavior_machine.hpp) on a cut of the
// swinging wall trap's graph: events take transitions, clip triggers raise
// events that feed back into the machine, playback speed and clip modes.
#include "actors/behavior_machine.hpp"

#include "animation_generated.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace skydot;
using Catch::Approx;

namespace {

bool has(const std::vector<std::string>& list, const std::string& what) {
    return std::ranges::find(list, what) != list.end();
}

/// States as in WallTraps' behavior00: Start (0), 180For (2), StopGapIdle
/// (4), 180Idle (9), 180Back (10), StopFor (8).
BehaviorDef wall_trap() {
    BehaviorDef def;
    def.start_state = 0;
    def.clips = {
        {"StartFor", "90For.hkx", 2, 1.0F, 0.0F, 0.0F, {}, 1.0F},
        {"180For", "180For.hkx", 0, 0.8F, 0.0F, 0.0F,
         {{0.0F, "SoundPlay", false}, {0.0F, "Done", true}, {0.0F, "Trans", true}, {0.5F, "SoundStop", false}}, 2.0F},
        {"180Idle", "180Back.hkx", 2, 1.0F, 0.0F, 0.0F, {}, 2.0F},
        {"180Back", "180Back.hkx", 0, 1.0F, 0.0F, 0.0F, {{0.0F, "TransStop", true}, {0.0F, "Done", true}}, 2.0F},
        {"StopGap", "StopGap.hkx", 0, 1.0F, 0.0F, 0.0F, {{0.0F, "TransStart", true}, {0.0F, "Done", true}}, 1.0F},
    };
    def.states = {
        {0, "Start", 0, {}, {}, {{"180For", 2}}},
        {2, "180For", 1, {}, {}, {{"Trans", 9}}},
        {4, "StopGapIdle", -1, {"TransStop"}, {}, {{"Rearm", 8}}},
        {8, "StopFor", 4, {}, {}, {{"TransStart", 0}}},
        {9, "180Idle", 2, {}, {}, {{"180Back", 10}}},
        {10, "180Back", 3, {}, {"Exit"}, {{"TransStop", 4}}},
    };
    def.wildcard = {{"180End", 9}, {"HardReset", 0}};
    return def;
}

} // namespace

TEST_CASE("the machine starts in its start state and holds a user-controlled clip", "[behavior]") {
    BehaviorMachine m(wall_trap());
    BehaviorOutput out;
    m.start(out);
    REQUIRE(m.state() != nullptr);
    CHECK(m.state()->name == "Start");
    REQUIRE(out.changes.size() == 1);
    CHECK(out.changes[0].from.empty());
    m.advance(5.0F, out);
    CHECK(m.state()->name == "Start");
    CHECK(m.clip() == 0);
    CHECK(m.clip_time() == 0.0F);
}

TEST_CASE("an unknown event changes nothing", "[behavior]") {
    BehaviorMachine m(wall_trap());
    BehaviorOutput out;
    m.start(out);
    out = {};
    m.send("Open", out);
    CHECK(out.changes.empty());
    CHECK(m.state()->name == "Start");
}

TEST_CASE("a swing plays at its speed, raises Done and Trans at the end and moves on", "[behavior]") {
    BehaviorMachine m(wall_trap());
    BehaviorOutput out;
    m.start(out);
    out = {};
    m.send("180For", out);
    REQUIRE(out.changes.size() == 1);
    CHECK(out.changes[0].from == "Start");
    CHECK(out.changes[0].to == "180For");
    CHECK(out.changes[0].event == "180For");
    CHECK_FALSE(has(out.events, "180For")); // The sent event is not echoed.

    out = {};
    m.advance(0.1F, out);
    CHECK(has(out.events, "SoundPlay"));
    CHECK(m.clip_time() == Approx(0.08F)); // Speed 0.8.
    CHECK_FALSE(has(out.events, "Done"));
    CHECK(m.state()->name == "180For");

    out = {};
    m.advance(0.6F, out); // Past 0.5 s of clip time.
    CHECK(has(out.events, "SoundStop"));
    CHECK(m.state()->name == "180For");

    out = {};
    m.advance(1.9F, out); // The 2 s clip at 0.8 ends 2.5 s in.
    CHECK(out.events == std::vector<std::string>{"Done", "Trans"});
    CHECK(m.state()->name == "180Idle");
    REQUIRE(out.changes.size() == 1);
    CHECK(out.changes[0].event == "Trans");
}

TEST_CASE("a single-play clip holds its last frame", "[behavior]") {
    BehaviorMachine m(wall_trap());
    BehaviorOutput out;
    m.start(out);
    m.send("180For", out);
    m.advance(1.0F, out);
    CHECK(m.state()->name == "180For");
    CHECK(m.clip_time() == Approx(0.8F));
}

TEST_CASE("the retract chain: Back, TransStop into the idle state, Rearm, TransStart into Start", "[behavior]") {
    BehaviorMachine m(wall_trap());
    BehaviorOutput out;
    m.start(out);
    m.send("180End", out); // Wildcard: start swung.
    CHECK(m.state()->name == "180Idle");
    out = {};
    m.send("180Back", out);
    CHECK(m.state()->name == "180Back");
    out = {};
    m.advance(3.0F, out);
    CHECK(m.state()->name == "StopGapIdle");
    CHECK(has(out.events, "TransStop"));
    CHECK(has(out.events, "Done"));
    CHECK(has(out.events, "Exit")); // Exit notify event of the state left.
    CHECK(has(out.events, "TransStop")); // Also StopGapIdle's enter event.
    out = {};
    m.send("Rearm", out);
    CHECK(m.state()->name == "StopFor");
    out = {};
    m.advance(1.5F, out);
    CHECK(has(out.events, "TransStart"));
    CHECK(m.state()->name == "Start");
}

TEST_CASE("a state's own transition wins over a wildcard; wildcards need no state", "[behavior]") {
    BehaviorDef def = wall_trap();
    def.states[0].transitions.push_back({"HardReset", 9});
    BehaviorMachine m(std::move(def));
    BehaviorOutput out;
    m.start(out);
    m.send("HardReset", out);
    CHECK(m.state()->name == "180Idle");
    m.send("HardReset", out);
    CHECK(m.state()->name == "Start"); // The wildcard.
}

TEST_CASE("looping and ping-pong clips keep their time inside the clip", "[behavior]") {
    BehaviorDef def;
    def.start_state = 1;
    def.clips = {{"loop", "a.hkx", 1, 1.0F, 0.0F, 0.0F, {{0.5F, "Half", false}}, 2.0F},
                 {"pong", "b.hkx", 3, 1.0F, 0.0F, 0.0F, {}, 2.0F}};
    def.states = {{1, "Loop", 0, {}, {}, {{"Go", 2}}}, {2, "Pong", 1, {}, {}, {}}};
    BehaviorMachine m(std::move(def));
    BehaviorOutput out;
    m.start(out);
    m.advance(0.6F, out);
    m.advance(1.9F, out);
    CHECK(m.clip_time() == Approx(0.5F));
    CHECK(std::ranges::count(out.events, "Half") == 2); // At 0.5 s and again after the wrap.
    m.send("Go", out);
    m.advance(2.5F, out);
    CHECK(m.clip_time() == Approx(1.5F)); // Turned at 2 s, 0.5 s back.
}

TEST_CASE("an animation asset's graph becomes a behaviour definition", "[behavior]") {
    namespace afb = bethconv::pack::afb;
    flatbuffers::FlatBufferBuilder b;
    const auto trigger = afb::CreateClipTrigger(b, 0.0F, 1, true);
    const auto triggers = b.CreateVector(std::vector{trigger});
    const auto clip_name = b.CreateString("180For");
    const auto clip_file = b.CreateString("Animations\\180For.hkx");
    afb::ClipGeneratorBuilder cg(b);
    cg.add_name(clip_name);
    cg.add_animation(clip_file);
    cg.add_mode(0);
    cg.add_playback_speed(0.8F);
    cg.add_triggers(triggers);
    const auto clip = cg.Finish();
    const auto clips = b.CreateVector(std::vector{clip});
    const auto transition = afb::CreateStateTransition(b, 0, 1, true, b.CreateString("!x"));
    const auto transitions = b.CreateVector(std::vector{transition});
    const auto s0 = afb::CreateBehaviorState(b, 0, b.CreateString("Start"), 0, 0, 0, transitions);
    const auto s1 = afb::CreateBehaviorState(b, 1, b.CreateString("End"), -1);
    const auto states = b.CreateVector(std::vector{s0, s1});
    const auto machine = afb::CreateStateMachine(b, b.CreateString("M"), 0, states);
    const auto machines = b.CreateVector(std::vector{machine});
    const auto events = b.CreateVectorOfStrings(std::vector<std::string>{"Go", "Done"});
    const auto graph = afb::CreateBehaviorGraph(b, events, 0, machines);
    const auto graphs = b.CreateVector(std::vector{graph});
    afb::AnimationBuilder ab(b);
    ab.add_format_version(2);
    ab.add_clip_generators(clips);
    ab.add_behavior_graphs(graphs);
    afb::FinishAnimationBuffer(b, ab.Finish());

    const BehaviorDef def = behavior_from_asset(*afb::GetAnimation(b.GetBufferPointer()));
    REQUIRE(def.states.size() == 2);
    CHECK(def.start_state == 0);
    CHECK(def.states[0].name == "Start");
    CHECK(def.states[0].clip == 0);
    CHECK(def.states[1].clip == -1);
    REQUIRE(def.states[0].transitions.size() == 1);
    CHECK(def.states[0].transitions[0].event == "Go");
    CHECK(def.states[0].transitions[0].to_state == 1);
    REQUIRE(def.clips.size() == 1);
    CHECK(def.clips[0].speed == Approx(0.8F));
    REQUIRE(def.clips[0].triggers.size() == 1);
    CHECK(def.clips[0].triggers[0].event == "Done");
    CHECK(def.clips[0].triggers[0].relative_to_end);
}
