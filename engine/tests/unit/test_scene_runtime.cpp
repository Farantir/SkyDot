// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scene domain module (vm/scene_runtime.cpp) over a world.fb built in
// memory and fake hosts: who counts as ready, fragment scripts attached once,
// package push and pop forwarded to the host, and the Scene natives.
#include "support/fake_condition_world.hpp"
#include "support/fake_hosts.hpp"
#include "support/fake_world.hpp"
#include "support/native_calls.hpp"
#include "support/vm_rig.hpp"

#include "vm/dialogue_runtime.hpp"
#include "vm/scene_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::Op;
using skydot::vm::SceneRuntime;
namespace wfb = bethconv::pack::wfb;

namespace {

constexpr std::uint32_t k_quest = 0x100;
constexpr std::uint32_t k_scene = 0x500;
constexpr std::uint32_t k_a = 0x20;
constexpr std::uint32_t k_package = 0x700;
constexpr std::uint32_t k_handle = 0x5100;

/// One scene: a single phase with a package action on alias 1 and the fragments of SF_Test.
class Built {
public:
    Built() {
        flatbuffers::FlatBufferBuilder fbb;
        const std::vector<flatbuffers::Offset<wfb::ScenePhase>> phases{wfb::CreateScenePhaseDirect(fbb, "")};
        const std::vector<flatbuffers::Offset<wfb::SceneActor>> actors{wfb::CreateSceneActor(fbb, 1, 0, 0)};
        const std::vector<std::uint32_t> packages{k_package};
        const std::vector<flatbuffers::Offset<wfb::SceneAction>> actions{
            wfb::CreateSceneActionDirect(fbb, 1, "", 1, 1, 0, 0, 0, 0, &packages, 0.0F)};
        const std::vector<flatbuffers::Offset<wfb::ScenePhaseFragment>> fragments{
            wfb::CreateScenePhaseFragmentDirect(fbb, 0, false, "PhaseBegin0"),
            wfb::CreateScenePhaseFragmentDirect(fbb, 0, true, "PhaseEnd0")};
        const std::vector<flatbuffers::Offset<wfb::Condition>> none;
        const std::vector<flatbuffers::Offset<wfb::Scene>> scenes{wfb::CreateSceneDirect(
            fbb, k_scene, "TestScene", k_quest, wfb::SceneFlags::NONE, &none, &phases, &actors, &actions, nullptr,
            "SF_Test", "Begin", "End", &fragments)};
        const auto v = fbb.CreateVector(scenes);
        wfb::WorldBuilder world(fbb);
        world.add_scenes(v);
        wfb::FinishWorldBuffer(fbb, world.Finish());
        bytes_.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    }
    [[nodiscard]] const wfb::World* root() const { return wfb::GetWorld(bytes_.data()); }

private:
    std::vector<std::uint8_t> bytes_;
};

struct Host final : SceneRuntime::Host, skydot::vm::DialogueRuntime::Host {
    Built data;
    FakeWorld world;
    FakeQuests quests;
    FakeConditionWorld conditions;
    FakeActors actors;
    std::vector<std::string> events;
    int attached = 0;

    const wfb::World* world_root() const override { return data.root(); }
    skydot::vm::WorldAccess& world_access() override { return world; }
    skydot::vm::QuestHost& quest_host() override { return quests; }
    skydot::conditions::ConditionWorld& condition_world() override { return conditions; }
    skydot::vm::ActorHost& actor_host() override { return actors; }
    std::uint32_t base_of(std::uint32_t ref) const override { return ref; }
    void attach_scene_scripts(std::uint32_t) override { ++attached; }
    void package_signal(std::uint32_t actor, std::uint32_t package, bool push) override {
        events.push_back(std::string(push ? "push " : "pop ") + std::to_string(actor) + " " + std::to_string(package));
    }
    void log(const std::string&) override {}
};

struct Fixture {
    Host host;
    Rig rig;
    skydot::vm::DialogueRuntime dialogue;
    SceneRuntime scenes;
    Fixture() {
        auto& state = host.quests.states.state(k_quest);
        state.running = state.started = true;
        host.quests.states.add_handle(k_quest, 1, k_handle);
        host.quests.states.set_fill(k_handle, k_a);
        host.actors.actors = {k_a};
        host.world.loaded_3d = {k_a};

        rig.add({.name = "Scene",
                 .functions = {native("Start"), native("ForceStart"), native("Stop"), native("IsPlaying", {}, "Bool"),
                               native("GetOwningQuest", {}, "Quest"),
                               native("IsActionComplete", {{"i", "Int"}}, "Bool")}});
        rig.add({.name = "Quest", .functions = {}});
        // The fragment script: every function notes its name.
        std::vector<FunctionSpec> fragments;
        for (const char* name : {"Begin", "End", "PhaseBegin0", "PhaseEnd0"}) {
            auto f = method(name);
            note(f, name);
            fragments.push_back(f);
        }
        rig.add({.name = "SF_Test", .parent = "Scene", .functions = fragments});
        rig.add(script("Caller", {wrap("start", "Scene", "Start"), wrap("stop", "Scene", "Stop"),
                                  wrap("playing", "Scene", "IsPlaying", {}, "Bool"),
                                  wrap("owner", "Scene", "GetOwningQuest", {}, "Quest"),
                                  wrap("complete", "Scene", "IsActionComplete", {{"i", "Int"}}, "Bool")}));
        dialogue.bind(host);
        scenes.bind_natives(rig.vm, host, dialogue);
    }
    skydot::vm::Value scene() { return rig.vm.object(k_scene, "Scene"); }
    bool noted(const std::string& text) const { return std::ranges::find(rig.notes, text) != rig.notes.end(); }
};

} // namespace

TEST_CASE("an actor is ready when it can talk, is enabled and (by default) has its 3D loaded", "[scenes][vm]") {
    Fixture f;
    CHECK(f.scenes.actor_ready(k_a));
    CHECK_FALSE(f.scenes.actor_ready(0x99)); // not an actor

    f.host.world.disabled.insert(k_a);
    CHECK_FALSE(f.scenes.actor_ready(k_a));
    f.host.world.disabled.clear();

    f.host.actors.actors.insert(0x21);
    f.host.world.loaded_3d.insert(0x21);
    CHECK(f.scenes.actor_ready(0x21));
    f.host.actors.states.damage(0x21, skydot::vm::av::k_health, 1000); // dead
    CHECK_FALSE(f.scenes.actor_ready(0x21));

    f.host.world.loaded_3d.clear();
    CHECK_FALSE(f.scenes.actor_ready(k_a));
    f.scenes.set_actors_need_3d(false);
    CHECK(f.scenes.actor_ready(k_a));
    f.host.world.disabled.insert(k_a); // disabled counts even when 3D is not needed
    CHECK_FALSE(f.scenes.actor_ready(k_a));
}

TEST_CASE("the quest and alias come from the quest host", "[scenes][vm]") {
    Fixture f;
    CHECK(f.scenes.quest_running(k_quest));
    CHECK(f.scenes.alias_ref(k_quest, 1) == k_a);
    CHECK(f.scenes.alias_ref(k_quest, 2) == 0);
    f.host.quests.states.state(k_quest).running = false;
    CHECK_FALSE(f.scenes.quest_running(k_quest));
}

TEST_CASE("fragment scripts attach once, however many fragments run", "[scenes][vm]") {
    Fixture f;
    REQUIRE(f.scenes.start(k_scene, false));
    f.scenes.update(0.1);
    f.rig.vm.update(0.0); // the fragments are threads
    CHECK(f.host.attached == 1);
    CHECK(f.noted("Begin"));
    CHECK(f.noted("PhaseBegin0"));
    f.scenes.update(0.1);
    f.rig.vm.update(0.0);
    CHECK_FALSE(f.scenes.is_playing(k_scene));
    CHECK(f.noted("PhaseEnd0"));
    CHECK(f.noted("End"));
    CHECK(f.host.attached == 1);
    // A reset forgets it: the next world attaches again.
    f.scenes.reset();
    CHECK_FALSE(f.scenes.started());
    CHECK(f.scenes.start(k_scene, false));
    CHECK(f.host.attached == 2);
}

TEST_CASE("packages the scene pushes and pops reach the host", "[scenes][vm]") {
    Fixture f;
    REQUIRE(f.scenes.start(k_scene, false));
    CHECK(f.host.events == std::vector<std::string>{"push 32 1792"});
    f.scenes.update(0.1);
    CHECK(f.host.events == std::vector<std::string>{"push 32 1792", "pop 32 1792"});
}

TEST_CASE("a loaded section pushes its packages again, none clears the runner", "[scenes][vm]") {
    Fixture f;
    REQUIRE(f.scenes.start(k_scene, false));
    const auto bytes = f.scenes.save();
    f.scenes.stop(k_scene);
    f.host.events.clear();
    auto loaded = f.scenes.load(skydot::save::ByteReader(bytes));
    REQUIRE(loaded != nullptr);
    f.scenes.adopt(std::move(loaded));
    CHECK(f.scenes.is_playing(k_scene));
    CHECK(f.host.events == std::vector<std::string>{"push 32 1792"});
    f.scenes.adopt(nullptr);
    CHECK_FALSE(f.scenes.is_playing(k_scene));
}

TEST_CASE("Scene.Start, IsPlaying, Stop and GetOwningQuest", "[scenes][vm][natives]") {
    Fixture f;
    CHECK_FALSE(f.rig.call("caller", "playing", {f.scene()}).b());
    f.rig.call("caller", "start", {f.scene()});
    CHECK(f.rig.call("caller", "playing", {f.scene()}).b());
    CHECK(f.rig.call("caller", "complete", {f.scene(), Int(1)}).b()); // a package action is done at once
    CHECK(f.rig.call("caller", "owner", {f.scene()}).form() == k_quest);
    f.rig.call("caller", "stop", {f.scene()});
    CHECK_FALSE(f.rig.call("caller", "playing", {f.scene()}).b());
}
