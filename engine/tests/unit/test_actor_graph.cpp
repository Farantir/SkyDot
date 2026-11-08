// SPDX-License-Identifier: GPL-3.0-or-later
//
// ActorGraph (actors/actor_graph.cpp): the root behaviour graph of a project with the graphs it
// references joined in, the clip that plays now, and the project's root motion. The assets are
// built in memory.
#include "actors/actor_graph.hpp"

#include "animation_generated.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>

using skydot::ActorGraph;
using skydot::ActorGraphLibrary;
namespace afb = bethconv::pack::afb;
using Catch::Approx;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Fbb = flatbuffers::FlatBufferBuilder;

Bytes finish(Fbb& b, flatbuffers::Offset<afb::Animation> root) {
    afb::FinishAnimationBuffer(b, root);
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes strings_asset(const std::vector<std::string>& files, const std::vector<std::pair<std::string, std::uint32_t>>& clips) {
    Fbb b;
    std::vector<flatbuffers::Offset<afb::ProjectClip>> list;
    for (const auto& [name, id] : clips) {
        list.push_back(afb::CreateProjectClipDirect(b, name.c_str(), id, 1.0F));
    }
    const auto f = b.CreateVectorOfStrings(files);
    const auto c = b.CreateVector(list);
    afb::AnimationBuilder ab(b);
    ab.add_format_version(6);
    ab.add_project_files(f);
    ab.add_project_clips(c);
    return finish(b, ab.Finish());
}

Bytes motion_asset(std::uint32_t id, float duration, const std::vector<afb::MotionKey>& keys,
                   const std::vector<afb::RotationKey>& turns) {
    Fbb b;
    const auto m = afb::CreateMotionDirect(b, id, duration, &keys, &turns);
    const auto list = b.CreateVector(std::vector{m});
    afb::AnimationBuilder ab(b);
    ab.add_format_version(6);
    ab.add_motions(list);
    return finish(b, ab.Finish());
}

Bytes character_asset(const std::string& behavior, const std::vector<std::string>& animations = {}) {
    Fbb b;
    const auto c = afb::CreateCharacter(b, 0, 0, 0, b.CreateString(behavior), b.CreateVectorOfStrings(animations));
    const auto list = b.CreateVector(std::vector{c});
    afb::AnimationBuilder ab(b);
    ab.add_format_version(6);
    ab.add_characters(list);
    return finish(b, ab.Finish());
}

Bytes clip_asset(float duration) {
    Fbb b;
    const auto c = afb::CreateClip(b, afb::ClipEncoding::Spline, duration);
    const auto list = b.CreateVector(std::vector{c});
    afb::AnimationBuilder ab(b);
    ab.add_format_version(6);
    ab.add_clips(list);
    return finish(b, ab.Finish());
}

struct StateSpec {
    int id;
    std::string name;
    int clip = -1;
    std::vector<std::pair<int, int>> transitions = {}; // event index, target state id
    std::string behavior = {};
};

/// A behaviour asset: events, clip generators (name, file, mode) and one machine.
Bytes graph_asset(const std::vector<std::string>& events,
                  const std::vector<std::tuple<std::string, std::string, int>>& clips, const std::string& machine,
                  const std::vector<StateSpec>& states, const std::vector<std::pair<int, int>>& wildcards = {}) {
    Fbb b;
    std::vector<flatbuffers::Offset<afb::ClipGenerator>> generators;
    for (const auto& [name, file, mode] : clips) {
        generators.push_back(afb::CreateClipGeneratorDirect(b, name.c_str(), file.c_str(), static_cast<std::int8_t>(mode)));
    }
    std::vector<flatbuffers::Offset<afb::BehaviorState>> built;
    for (const StateSpec& s : states) {
        std::vector<flatbuffers::Offset<afb::StateTransition>> transitions;
        for (const auto& [event, target] : s.transitions) {
            transitions.push_back(afb::CreateStateTransition(b, event, target, false, 0));
        }
        built.push_back(afb::CreateBehaviorState(b, s.id, b.CreateString(s.name), s.clip, 0, 0,
                                                 b.CreateVector(transitions), 0, 0, b.CreateString(s.behavior)));
    }
    std::vector<flatbuffers::Offset<afb::StateTransition>> wild;
    for (const auto& [event, target] : wildcards) {
        wild.push_back(afb::CreateStateTransition(b, event, target, false, 0));
    }
    const auto m = afb::CreateStateMachine(b, b.CreateString(machine), states.front().id, b.CreateVector(built),
                                           b.CreateVector(wild));
    const auto g = afb::CreateBehaviorGraph(b, b.CreateVectorOfStrings(events), 0, b.CreateVector(std::vector{m}));
    const auto gens = b.CreateVector(generators);
    const auto graphs = b.CreateVector(std::vector{g});
    afb::AnimationBuilder ab(b);
    ab.add_format_version(6);
    ab.add_clip_generators(gens);
    ab.add_behavior_graphs(graphs);
    return finish(b, ab.Finish());
}

/// Project x/p: root graph Root (Idle, then the state that references Ref), Ref's two clips.
std::map<std::string, Bytes> pack() {
    std::map<std::string, Bytes> files;
    files["meshes/animationdata/p.txt"] =
        strings_asset({"Characters\\C.hkx", "Behaviors\\Root.hkx", "Behaviors\\Ref.hkx"}, {{"ClipA", 6}, {"ClipB", 7}});
    files["meshes/animationdata/boundanims/anims_p.txt"] = motion_asset(
        7, 2.0F, {{0.0F, 0, 0, 0}, {2.0F, 0, 0, -10.0F}}, {{0.0F, 0, 0, 0, 1}, {2.0F, 0, 0, 0.70710678F, 0.70710678F}});
    files["meshes/x/characters/c.hkx"] = character_asset("Behaviors\\Root.hkx");
    files["meshes/x/behaviors/root.hkx"] =
        graph_asset({"EnterRef", "Next", "Instant", "Leave"}, {}, "Root",
                    {{0, "Idle", -1, {{0, 1}}}, {1, "Ref", -1, {{3, 0}}, "Behaviors\\Ref.hkx"}});
    files["meshes/x/behaviors/ref.hkx"] =
        graph_asset({"EnterRef", "Next", "Instant", "Leave"}, {{"ClipA", "Animations\\a.hkx", 1}, {"ClipB", "Animations\\b.hkx", 0}},
                    "RefRoot", {{0, "A", 0, {{1, 1}}}, {1, "B", 1}}, {{2, 1}});
    files["meshes/x/animations/a.hkx"] = clip_asset(3.0F);
    files["meshes/x/animations/b.hkx"] = clip_asset(2.0F);
    return files;
}

} // namespace

TEST_CASE("the graph of a project joins the graphs its states reference", "[actor_graph]") {
    const auto files = pack();
    int reads = 0;
    ActorGraphLibrary library([&](const std::string& vpath) {
        ++reads;
        const auto it = files.find(vpath);
        return it != files.end() ? it->second : Bytes{};
    });
    const auto def = library.graph("x/p.hkx");
    REQUIRE(def->missing.empty());
    CHECK(def->behaviour == "meshes/x/behaviors/root.hkx");
    REQUIRE(def->graph.machines.size() == 2);
    CHECK(def->graph.roots == std::vector<int>{0});
    REQUIRE(def->graph.machines[0].states[1].nested == std::vector<int>{1});
    CHECK(def->depth == std::vector<int>{0, 1});
    CHECK(def->clip_files == std::vector<std::string>{"meshes/x/animations/a.hkx", "meshes/x/animations/b.hkx"});
    const int after = reads;
    CHECK(library.graph("meshes/x/P.hkx") == def); // cached by file
    CHECK(reads == after);

    ActorGraph graph(def, [&](const std::string& file) { return library.clip_duration(file); });
    auto out = graph.start();
    CHECK_FALSE(graph.playing().has_value()); // Idle has no clip: the actor's own animation

    out = graph.send("EnterRef");
    REQUIRE(graph.playing().has_value());
    CHECK(graph.playing()->clip == "ClipA");
    CHECK(graph.playing()->file == "meshes/x/animations/a.hkx");
    CHECK(graph.playing()->mode == 1);
    out = graph.advance(1.0F);
    CHECK(graph.playing()->time == Approx(1.0F));
    out = graph.advance(2.5F); // a loop of 3 s wraps
    CHECK(graph.playing()->time == Approx(0.5F));

    out = graph.send("Next");
    REQUIRE(graph.playing().has_value());
    CHECK(graph.playing()->clip == "ClipB");
    CHECK(graph.playing()->mode == 0);
    graph.advance(1.0F);
    const auto motion = graph.root_motion();
    REQUIRE(motion.has_value());
    CHECK(motion->z == Approx(-5.0F));
    CHECK(motion->yaw == Approx(1.5707963F / 2.0F).margin(1e-3)); // halfway to a quarter turn
    graph.advance(5.0F); // plays once and stays at its end
    CHECK(graph.playing()->time == Approx(2.0F));
    CHECK(graph.root_motion()->z == Approx(-10.0F));
    CHECK_FALSE(def->root_motion("ClipA", 1.0F).has_value()); // no motion for it
    CHECK(def->root_motion("clipb", 3.0F)->z == Approx(-10.0F));
}

TEST_CASE("a project without a character or a graph is empty with a reason", "[actor_graph]") {
    ActorGraphLibrary library([](const std::string&) { return Bytes{}; });
    const auto def = library.graph("x/none.hkx");
    CHECK(def->empty());
    CHECK_FALSE(def->missing.empty());
}

TEST_CASE("an event only a nested machine handles puts the machines into the state that nests it", "[actor_graph]") {
    const auto files = pack();
    ActorGraphLibrary library([&](const std::string& vpath) {
        const auto it = files.find(vpath);
        return it != files.end() ? it->second : Bytes{};
    });
    const auto def = library.graph("x/p.hkx");
    REQUIRE(def->missing.empty());
    REQUIRE(def->parents.size() == 2);
    CHECK(def->parents[1] == std::vector<std::pair<int, int>>{{0, 1}});
    ActorGraph graph(def, [&](const std::string& file) { return library.clip_duration(file); });
    graph.start();
    // The root has no transition for "Instant": a plain send does nothing.
    CHECK_FALSE(graph.send("Instant").accepted);
    CHECK_FALSE(graph.playing().has_value());
    CHECK_FALSE(graph.in_furniture());
    // Entering routes it: the root goes into the state that nests RefRoot, which takes the event.
    const auto out = graph.send_entering("Instant");
    CHECK(out.accepted);
    REQUIRE(graph.playing().has_value());
    CHECK(graph.playing()->clip == "ClipB");
    CHECK(graph.in_furniture());
    graph.send("Leave");
    CHECK_FALSE(graph.in_furniture()); // back in the state it left
    CHECK_FALSE(graph.playing().has_value());
}

TEST_CASE("a character that lists its own file of a clip's name plays that one", "[actor_graph]") {
    auto files = pack();
    files["meshes/x/characters/c.hkx"] = character_asset("Behaviors\\Root.hkx", {"Animations\\female\\a.hkx", "Animations\\b.hkx"});
    files["meshes/x/animations/female/a.hkx"] = clip_asset(1.0F);
    ActorGraphLibrary library([&](const std::string& vpath) {
        const auto it = files.find(vpath);
        return it != files.end() ? it->second : Bytes{};
    });
    const auto def = library.graph("x/p.hkx");
    REQUIRE(def->missing.empty());
    // ClipA named Animations\a.hkx: the character has female\a.hkx; ClipB is listed as it is.
    CHECK(def->clip_files == std::vector<std::string>{"meshes/x/animations/female/a.hkx", "meshes/x/animations/b.hkx"});
}

TEST_CASE("root motion before a clip's first key comes from the origin", "[actor_graph]") {
    auto files = pack();
    files["meshes/animationdata/boundanims/anims_p.txt"] =
        motion_asset(6, 2.0F, {{2.0F, 0, 20.0F, 0}}, {{2.0F, 0, 0, 0.70710678F, 0.70710678F}});
    ActorGraphLibrary library([&](const std::string& vpath) {
        const auto it = files.find(vpath);
        return it != files.end() ? it->second : Bytes{};
    });
    const auto def = library.graph("x/p.hkx");
    REQUIRE(def->missing.empty());
    CHECK(def->root_motion("ClipA", 0.0F)->y == Approx(0.0F));
    CHECK(def->root_motion("ClipA", 1.0F)->y == Approx(10.0F));
    CHECK(def->root_motion("ClipA", 1.0F)->yaw == Approx(1.5707963F / 2.0F).margin(1e-3));
    CHECK(def->root_motion("ClipA", 5.0F)->y == Approx(20.0F));
}
