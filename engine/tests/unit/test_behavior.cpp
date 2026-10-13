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
#include <cstdint>
#include <span>
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

TEST_CASE("a trigger late in a looping clip fires before the wrap, not skipped", "[behavior]") {
    BehaviorDef def;
    def.start_state = 1;
    def.clips = {{"loop", "a.hkx", 1, 1.0F, 0.0F, 0.0F, {{1.5F, "Late", false}, {-0.0F, "End", true}}, 2.0F}};
    def.states = {{1, "Loop", 0, {}, {}, {}}};
    BehaviorMachine m(std::move(def));
    BehaviorOutput out;
    m.start(out);
    m.advance(1.0F, out);
    CHECK(out.events.empty());
    m.advance(1.5F, out); // 1.0 -> 2.5: passes 1.5 and the end, wraps to 0.5.
    CHECK(m.clip_time() == Approx(0.5F));
    CHECK(std::ranges::count(out.events, "Late") == 1);
    CHECK(std::ranges::count(out.events, "End") == 1);
    m.advance(1.5F, out); // 0.5 -> 2.0: the next lap's triggers.
    CHECK(std::ranges::count(out.events, "Late") == 2);
}

TEST_CASE("a state without a clip holds the pose of the clip before it", "[behavior]") {
    BehaviorDef def;
    def.start_state = 1;
    def.clips = {{"swing", "a.hkx", 0, 1.0F, 0.0F, 0.0F, {}, 2.0F}};
    def.states = {{1, "Swing", 0, {}, {}, {{"Stop", 2}}}, {2, "Held", -1, {}, {}, {{"Again", 1}}}};
    BehaviorMachine m(std::move(def));
    BehaviorOutput out;
    m.start(out);
    CHECK(m.pose_clip() == 0);
    m.advance(1.25F, out);
    m.send("Stop", out);
    CHECK(m.clip() == -1);
    CHECK(m.pose_clip() == 0);
    CHECK(m.pose_time() == Approx(1.25F));
    m.advance(1.0F, out);
    CHECK(m.pose_time() == Approx(1.25F));
    m.send("Again", out);
    CHECK(m.pose_time() == Approx(0.0F));
}

TEST_CASE("an event to the state already current is accepted, and changes nothing", "[behavior]") {
    BehaviorDef def;
    def.start_state = 1;
    def.clips = {{"closed", "a.hkx", 1, 1.0F, 0.0F, 0.0F, {}, 1.0F}};
    def.states = {{1, "Closed", 0, {}, {}, {{"close", 1}}}};
    BehaviorMachine m(std::move(def));
    BehaviorOutput out;
    m.start(out);
    BehaviorOutput sent;
    m.send("Close", sent);
    CHECK(sent.accepted);
    CHECK(sent.changes.empty());
    BehaviorOutput unknown;
    m.send("Nothing", unknown);
    CHECK_FALSE(unknown.accepted);
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

namespace {

/// The swinging blade's graph: a root machine whose only state runs two
/// machines side by side. One plays Havok clip 0, the other NIF sequences.
BehaviorGraphDef blade_graph() {
    BehaviorGraphDef def;
    def.clips = {{"Clip", "c.hkx", 0, 1.0F, 0.0F, 0.0F, {{0.0F, "Apex", true}}, 1.0F}};
    BehaviorDef root;
    root.start_state = 0;
    root.states = {{0, "State05", -1, {}, {}, {}, "", {1, 2}}};
    BehaviorDef clips;
    clips.start_state = 0;
    clips.states = {{0, "Rest", -1, {}, {}, {{"Single", 1}}}, {1, "Swing", 0, {}, {}, {{"Reset", 0}}}};
    BehaviorDef sequences;
    sequences.start_state = 3;
    sequences.states = {{3, "Idle", -1, {}, {}, {{"Loop", 4}, {"Apex", 5}}, "playanim01"},
                        {4, "Loop", -1, {}, {}, {{"Reset", 3}, {"Apex", 5}}, "playanim02"},
                        {5, "Hit", -1, {}, {}, {{"Reset", 3}}, "playanim03"}};
    def.machines = {root, clips, sequences};
    def.roots = {0};
    return def;
}

} // namespace

TEST_CASE("a state that runs machines starts them side by side, and its events go to all", "[behavior]") {
    BehaviorSet set(blade_graph());
    BehaviorOutput out;
    set.start(out);
    CHECK(set.running() == std::vector<int>{0, 1, 2});
    std::vector<std::string> entered;
    for (const auto& change : out.changes) {
        entered.push_back(change.to);
        if (change.to == "Idle") {
            CHECK(change.sequence == "playanim01");
            CHECK(change.machine == 2);
        }
    }
    CHECK(entered == std::vector<std::string>{"State05", "Rest", "Idle"});

    out = {};
    set.send("Loop", out); // only the sequence machine has it
    REQUIRE(out.changes.size() == 1);
    CHECK(out.changes[0].to == "Loop");
    CHECK(out.changes[0].sequence == "playanim02");
    CHECK(set.machine(1).state()->name == "Rest");

    out = {};
    set.send("Single", out); // the clip machine takes it; the clip's trigger then reaches the other
    CHECK(set.machine(1).state()->name == "Swing");
    out = {};
    set.advance(1.5F, out);
    CHECK(has(out.events, "Apex"));
    CHECK(set.machine(2).state()->name == "Hit");
    CHECK(set.machine(2).state()->sequence == "playanim03");
}

TEST_CASE("a graph's roots are the machines that run; others wait to be named", "[behavior]") {
    BehaviorGraphDef def = blade_graph();
    def.roots = {1, 2};
    BehaviorSet set(def);
    BehaviorOutput out;
    set.start(out);
    CHECK(set.running() == std::vector<int>{1, 2});
    CHECK(set.machine(0).state() == nullptr);
}

TEST_CASE("a saved graph resumes mid-clip with its nested machines, silently", "[behavior]") {
    BehaviorSet set(blade_graph());
    BehaviorOutput out;
    set.start(out);
    set.send("Single", out);
    out = {};
    set.advance(0.4F, out); // mid-swing, before the Apex trigger at the clip's end
    REQUIRE(set.machine(1).state()->name == "Swing");
    const std::vector<std::uint8_t> bytes = set.save();
    REQUIRE_FALSE(bytes.empty());

    BehaviorSet fresh(blade_graph());
    out = {};
    fresh.start(out);
    REQUIRE(fresh.load(bytes));
    CHECK(fresh.running() == std::vector<int>{0, 1, 2});
    CHECK(fresh.machine(1).state()->name == "Swing");
    CHECK(fresh.machine(1).clip_time() == Catch::Approx(0.4F));
    CHECK(fresh.machine(2).state()->name == "Idle");
    // It goes on from there: the trigger fires 0.6 s later, not 1.0 s.
    out = {};
    fresh.advance(0.5F, out);
    CHECK_FALSE(has(out.events, "Apex"));
    out = {};
    fresh.advance(0.2F, out);
    CHECK(has(out.events, "Apex"));
}

TEST_CASE("a snapshot that does not fit the graph is refused and changes nothing", "[behavior]") {
    BehaviorSet set(blade_graph());
    BehaviorOutput out;
    set.start(out);
    set.send("Single", out);
    const std::vector<std::uint8_t> good = set.save();

    BehaviorSet other(blade_graph());
    other.start(out);
    CHECK_FALSE(other.load({}));
    CHECK_FALSE(other.load(std::span<const std::uint8_t>(good.data(), good.size() - 1)));
    std::vector<std::uint8_t> trailing = good;
    trailing.push_back(0);
    CHECK_FALSE(other.load(trailing));
    std::vector<std::uint8_t> bad_magic = good;
    bad_magic[0] ^= 0xFF;
    CHECK_FALSE(other.load(bad_magic));
    // Machine 1 (index 1, first running record after machine 0's) names an unknown state id.
    // Header 9 bytes; record 0: 1 + 4+4+4+4+1+4 = 22 bytes; record 1 starts at 31, id at 32.
    std::vector<std::uint8_t> bad_state = good;
    bad_state[32] = 99;
    CHECK_FALSE(other.load(bad_state));
    CHECK(other.machine(1).state()->name == "Rest");
    CHECK(other.machine(1).clip_time() == 0.0F);

    BehaviorSet different(BehaviorGraphDef{});
    CHECK_FALSE(different.load(good));
}
