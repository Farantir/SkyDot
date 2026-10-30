// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scene runner (scenes/scenes.cpp) over a world.fb built in memory with a
// fake host: phase order, a skipped phase, completion conditions that wait,
// timers, package push and pop, Stop, ForceStart, the quest-start flag,
// fragments and the SCEN save round trip.
#include "support/fake_condition_world.hpp"

#include "conditions/conditions.hpp"
#include "conditions/functions.hpp"
#include "scenes/scenes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace skydot::testing;
namespace scenes = skydot::scenes;
namespace wfb = bethconv::pack::wfb;

namespace {

constexpr std::uint32_t k_quest = 0x100;
constexpr std::uint32_t k_scene = 0x500;
constexpr std::uint32_t k_other_scene = 0x501;
constexpr std::uint32_t k_a = 0x20; // alias 1
constexpr std::uint32_t k_b = 0x21; // alias 2

struct ActionSpec {
    std::uint16_t type; ///< 0 dialogue, 1 package, 2 timer
    std::uint32_t index;
    std::int32_t alias;
    std::uint32_t start;
    std::uint32_t end;
    std::uint32_t topic = 0;
    std::vector<std::uint32_t> packages = {};
    float seconds = 0.0F;
};
struct PhaseSpec {
    std::uint32_t start_global = 0;      ///< GetGlobalValue(g) == 1 or the phase is skipped
    std::uint32_t completion_global = 0; ///< likewise, the phase waits for it
};
struct SceneSpec {
    std::uint32_t id = k_scene;
    wfb::SceneFlags flags = wfb::SceneFlags::NONE;
    std::vector<PhaseSpec> phases;
    std::vector<ActionSpec> actions;
    std::vector<std::int32_t> aliases = {1, 2};
    std::uint32_t scene_global = 0;
};

class Built {
public:
    explicit Built(const std::vector<SceneSpec>& specs) {
        flatbuffers::FlatBufferBuilder fbb;
        const auto conds = [&](std::uint32_t global) {
            std::vector<flatbuffers::Offset<wfb::Condition>> out;
            if (global != 0) {
                out.push_back(wfb::CreateConditionDirect(fbb, 0, skydot::conditions::fn::get_global_value, 1.0F, 0, global));
            }
            return out;
        };
        std::vector<flatbuffers::Offset<wfb::Scene>> ss;
        for (const auto& spec : specs) {
            std::vector<flatbuffers::Offset<wfb::ScenePhase>> phases;
            for (const auto& p : spec.phases) {
                const auto start = conds(p.start_global);
                const auto done = conds(p.completion_global);
                phases.push_back(wfb::CreateScenePhaseDirect(fbb, "", &start, &done));
            }
            std::vector<flatbuffers::Offset<wfb::SceneActor>> actors;
            for (const auto alias : spec.aliases) {
                actors.push_back(wfb::CreateSceneActor(fbb, alias, 0, 0));
            }
            std::vector<flatbuffers::Offset<wfb::SceneAction>> actions;
            for (const auto& a : spec.actions) {
                actions.push_back(wfb::CreateSceneActionDirect(fbb, a.type, "", a.alias, a.index, 0, a.start, a.end,
                                                               a.topic, &a.packages, a.seconds));
            }
            const auto scene_conditions = conds(spec.scene_global);
            std::vector<flatbuffers::Offset<wfb::ScenePhaseFragment>> fragments;
            fragments.push_back(wfb::CreateScenePhaseFragmentDirect(fbb, 0, false, "PhaseBegin0"));
            fragments.push_back(wfb::CreateScenePhaseFragmentDirect(fbb, 0, true, "PhaseEnd0"));
            ss.push_back(wfb::CreateSceneDirect(fbb, spec.id, spec.id == k_scene ? "TestScene" : "OtherScene", k_quest,
                                                spec.flags, &scene_conditions, &phases, &actors, &actions, nullptr,
                                                "SF_Test", "Begin", "End", &fragments));
        }
        const auto v = fbb.CreateVector(ss);
        wfb::WorldBuilder world(fbb);
        world.add_scenes(v);
        wfb::FinishWorldBuffer(fbb, world.Finish());
        bytes_.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
        root_ = wfb::GetWorld(bytes_.data());
    }
    const wfb::World& operator*() const { return *root_; }

private:
    std::vector<std::uint8_t> bytes_;
    const wfb::World* root_ = nullptr;
};

class FakeHost final : public scenes::SceneHost {
public:
    FakeConditionWorld conditions;
    bool running = true;
    std::map<std::int32_t, std::uint32_t> alias_fill = {{1, k_a}, {2, k_b}};
    std::set<std::uint32_t> not_ready;
    std::vector<std::string> events; ///< fragments, packages and the log
    bool quest_running(std::uint32_t) const override { return running; }
    std::uint32_t alias_ref(std::uint32_t, std::int32_t alias) const override {
        const auto it = alias_fill.find(alias);
        return it != alias_fill.end() ? it->second : 0;
    }
    bool actor_ready(std::uint32_t actor) const override { return !not_ready.contains(actor); }
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list, std::uint32_t quest,
                         std::uint32_t subject) override {
        return skydot::conditions::conditions_pass(list, conditions, {.subject = subject, .quest = quest});
    }
    std::vector<skydot::dialogue::Line> say(std::uint32_t actor, std::uint32_t topic) override {
        events.push_back("say " + std::to_string(actor) + " " + std::to_string(topic));
        if (topic == 0) {
            return {};
        }
        return {{.text = "line", .voice = "", .info = topic}};
    }
    void run_fragment(std::uint32_t, const std::string&, const std::string& function) override {
        events.push_back("fragment " + function);
    }
    void push_package(std::uint32_t actor, std::uint32_t package) override {
        events.push_back("push " + std::to_string(actor) + " " + std::to_string(package));
    }
    void pop_package(std::uint32_t actor, std::uint32_t package) override {
        events.push_back("pop " + std::to_string(actor) + " " + std::to_string(package));
    }
    void log(const std::string& text) override { events.push_back(text); }
    [[nodiscard]] bool has(const std::string& text) const {
        return std::ranges::find(events, text) != events.end();
    }
};

SceneSpec dialogue_scene() {
    return {.phases = {{}, {}, {}},
            .actions = {{0, 1, 1, 0, 0, 900}, {0, 2, 2, 1, 1, 901}, {0, 3, 1, 2, 2, 902}}};
}

} // namespace

TEST_CASE("phases run in order, a dialogue action waits for the game layer", "[scenes]") {
    Built world({dialogue_scene()});
    FakeHost host;
    scenes::Scenes s(*world, host);
    REQUIRE(s.start(k_scene));
    CHECK(s.is_playing(k_scene));
    CHECK(host.has("scene TestScene start"));
    CHECK(host.has("fragment Begin"));
    CHECK(host.has("fragment PhaseBegin0"));
    for (std::uint32_t i = 1; i <= 3; ++i) {
        auto out = s.update(0.1);
        REQUIRE(out.size() == 1);
        CHECK(out[0].action == i);
        CHECK(out[0].speaker == (i == 2 ? k_b : k_a));
        CHECK(out[0].lines.size() == 1);
        CHECK(s.update(5.0).empty()); // still speaking: the phase waits
        CHECK_FALSE(s.action_complete(k_scene, i));
        s.action_finished(k_scene, i);
        CHECK(s.action_complete(k_scene, i));
        if (i < 3) {
            // The next phase begins in the next update; its request comes with it.
            host.events.push_back("--");
        }
    }
    s.update(0.1);
    CHECK_FALSE(s.is_playing(k_scene));
    CHECK(host.has("scene TestScene end"));
    CHECK(host.has("fragment End"));
    CHECK(host.has("fragment PhaseEnd0"));
}

TEST_CASE("a phase whose start conditions fail is skipped", "[scenes]") {
    SceneSpec spec = dialogue_scene();
    spec.phases[1].start_global = 7;
    Built world({spec});
    FakeHost host;
    scenes::Scenes s(*world, host);
    REQUIRE(s.start(k_scene));
    auto out = s.update(0.1);
    REQUIRE(out.size() == 1);
    s.action_finished(k_scene, 1);
    out = s.update(0.1); // phase 1 is skipped; phase 2 begins
    REQUIRE(out.size() == 1);
    CHECK(out[0].action == 3);
    CHECK(host.has("scene TestScene skip phase 1 (conditions)"));
    CHECK(s.action_complete(k_scene, 2)); // the skipped phase's action counts as done
}

TEST_CASE("a phase waits for its completion conditions", "[scenes]") {
    SceneSpec spec{.phases = {{.completion_global = 9}}, .actions = {{2, 1, -1, 0, 0, 0, {}, 1.0F}}};
    Built world({spec});
    FakeHost host;
    scenes::Scenes s(*world, host);
    REQUIRE(s.start(k_scene));
    s.update(2.0); // the timer is done, the condition is not
    CHECK(s.is_playing(k_scene));
    CHECK(s.action_complete(k_scene, 1));
    host.conditions.globals[9] = 1.0;
    s.update(0.1);
    CHECK_FALSE(s.is_playing(k_scene));
}

TEST_CASE("a timer runs on the update time", "[scenes]") {
    SceneSpec spec{.phases = {{}, {}}, .actions = {{2, 1, -1, 0, 0, 0, {}, 3.0F}, {2, 2, -1, 1, 1, 0, {}, 1.0F}}};
    Built world({spec});
    FakeHost host;
    scenes::Scenes s(*world, host);
    REQUIRE(s.start(k_scene));
    s.update(1.0);
    s.update(1.0);
    CHECK_FALSE(s.action_complete(k_scene, 1));
    s.update(1.5);
    CHECK(s.action_complete(k_scene, 1));
    CHECK_FALSE(s.action_complete(k_scene, 2)); // phase 1 began, its timer just started
    s.update(1.0);
    CHECK_FALSE(s.is_playing(k_scene));
}

TEST_CASE("a package is pushed while the action runs and popped when its end phase completes", "[scenes]") {
    SceneSpec spec{.phases = {{}, {}, {}},
                   .actions = {{1, 1, 1, 0, 1, 0, {700, 701}}, {2, 2, -1, 1, 1, 0, {}, 5.0F}, {2, 3, -1, 2, 2, 0, {}, 1.0F}}};
    Built world({spec});
    FakeHost host;
    scenes::Scenes s(*world, host);
    REQUIRE(s.start(k_scene));
    CHECK(host.has("push 32 700"));
    CHECK(host.has("push 32 701"));
    CHECK(s.action_complete(k_scene, 1)); // done at once
    s.update(0.1);                         // phase 0 completes, phase 1 begins; the package stays
    CHECK_FALSE(host.has("pop 32 700"));
    s.update(6.0); // phase 1 completes: the package comes off
    CHECK(host.has("pop 32 700"));
    CHECK(host.has("pop 32 701"));
}

TEST_CASE("Stop pops packages and runs the end fragment; the quest's end stops flagged scenes", "[scenes]") {
    SceneSpec a{.flags = wfb::SceneFlags::stop_on_quest_end, .phases = {{}}, .actions = {{1, 1, 1, 0, 0, 0, {700}}, {2, 2, -1, 0, 0, 0, {}, 99.0F}}};
    SceneSpec b{.id = k_other_scene, .phases = {{}}, .actions = {{2, 1, -1, 0, 0, 0, {}, 99.0F}}, .aliases = {}};
    Built world({a, b});
    FakeHost host;
    scenes::Scenes s(*world, host);
    REQUIRE(s.start(k_scene));
    REQUIRE(s.start(k_other_scene));
    s.stop(k_other_scene);
    CHECK_FALSE(s.is_playing(k_other_scene));
    CHECK(host.has("scene OtherScene stop"));
    s.quest_stopped(k_quest);
    CHECK_FALSE(s.is_playing(k_scene));
    CHECK(host.has("pop 32 700"));
    CHECK(host.has("scene TestScene stop"));
}

TEST_CASE("Start needs the quest, the conditions and ready actors; ForceStart skips the conditions", "[scenes]") {
    SceneSpec spec = dialogue_scene();
    spec.scene_global = 5;
    Built world({spec});
    FakeHost host;
    scenes::Scenes s(*world, host);
    CHECK_FALSE(s.start(k_scene)); // the condition fails
    CHECK(s.force_start(k_scene));
    s.stop(k_scene);
    host.conditions.globals[5] = 1.0;
    host.running = false;
    CHECK_FALSE(s.start(k_scene));
    host.running = true;
    host.not_ready.insert(k_b);
    CHECK_FALSE(s.start(k_scene)); // an actor is not loaded
    host.not_ready.clear();
    host.alias_fill.erase(2);
    CHECK_FALSE(s.start(k_scene)); // an alias is empty
    host.alias_fill[2] = k_b;
    CHECK(s.start(k_scene));
    CHECK_FALSE(s.start(k_scene)); // already playing
}

TEST_CASE("ForceStart stops the other scenes of its actors; Start does not take them", "[scenes]") {
    SceneSpec a = dialogue_scene();
    SceneSpec b = dialogue_scene();
    b.id = k_other_scene;
    Built world({a, b});
    FakeHost host;
    scenes::Scenes s(*world, host);
    REQUIRE(s.start(k_scene));
    CHECK_FALSE(s.start(k_other_scene));
    CHECK(s.force_start(k_other_scene));
    CHECK_FALSE(s.is_playing(k_scene));
    CHECK(s.is_playing(k_other_scene));
}

TEST_CASE("a scene with the quest-start flag begins when the quest starts, waiting for its actors", "[scenes]") {
    SceneSpec spec = dialogue_scene();
    spec.flags = wfb::SceneFlags::begin_on_quest_start;
    Built world({spec});
    FakeHost host;
    host.not_ready.insert(k_a);
    scenes::Scenes s(*world, host);
    s.quest_started(k_quest);
    CHECK_FALSE(s.is_playing(k_scene));
    s.update(0.1);
    CHECK_FALSE(s.is_playing(k_scene));
    host.not_ready.clear();
    s.update(0.1);
    CHECK(s.is_playing(k_scene));
}

TEST_CASE("running scenes survive a save; a speaking action asks for its line again", "[scenes]") {
    SceneSpec spec{.phases = {{}, {}}, .actions = {{1, 1, 1, 0, 1, 0, {700}}, {0, 2, 2, 1, 1, 901}, {2, 3, -1, 1, 1, 0, {}, 10.0F}}};
    Built world({spec});
    FakeHost host;
    scenes::Scenes s(*world, host);
    REQUIRE(s.start(k_scene));
    s.update(0.1); // phase 1 begins: the line goes out, the timer runs
    REQUIRE(host.has("say 33 901"));
    s.update(4.0);
    const auto bytes = s.save();

    FakeHost later;
    scenes::Scenes loaded(*world, later);
    REQUIRE(scenes::Scenes::load(skydot::save::ByteReader(bytes), loaded));
    CHECK(loaded.is_playing(k_scene));
    CHECK(loaded.save() == bytes);
    const auto out = loaded.update(1.0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].action == 2);
    CHECK(out[0].speaker == k_b);
    CHECK_FALSE(loaded.action_complete(k_scene, 3)); // 10 s timer, 5 s run
    loaded.action_finished(k_scene, 2);
    loaded.update(6.0);
    CHECK_FALSE(loaded.is_playing(k_scene));
    CHECK(later.has("pop 32 700"));

    auto damaged = bytes;
    damaged.pop_back();
    scenes::Scenes bad(*world, later);
    CHECK_FALSE(scenes::Scenes::load(skydot::save::ByteReader(damaged), bad));
}

TEST_CASE("a scene started by a script waits for unbuilt actors, then begins without them after the grace time", "[scenes]") {
    Built world({dialogue_scene()});
    FakeHost host;
    host.not_ready.insert(k_b);
    scenes::Scenes s(*world, host);
    CHECK_FALSE(s.start(k_scene));
    s.update(1.0);
    CHECK_FALSE(s.is_playing(k_scene));
    s.update(scenes::Scenes::k_grace);
    CHECK(s.is_playing(k_scene));

    // Built in time, it begins as soon as it can.
    s.stop(k_scene);
    FakeHost other;
    other.not_ready.insert(k_a);
    scenes::Scenes t(*world, other);
    CHECK_FALSE(t.start(k_scene));
    other.not_ready.clear();
    t.update(0.1);
    CHECK(t.is_playing(k_scene));
}
