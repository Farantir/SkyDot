// SPDX-License-Identifier: GPL-3.0-or-later
//
// Family hosts for natives tests: they hand out ids or record what a family
// asked, with the knobs a test needs.
#pragma once

#include "vm/quest_host.hpp"
#include "vm/sound_host.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace skydot::testing {

/// Playback ids 1, 2, ...; `exhausted` makes them run out.
struct FakeSounds final : skydot::vm::SoundHost {
    std::int64_t next = 1;
    bool exhausted = false;
    std::int64_t new_sound_instance() override { return exhausted ? 0 : next++; }
};

/// A quest host over a QuestStates of its own: starting and stopping set the
/// flags, and every call is recorded for the test to read.
struct FakeQuests final : skydot::vm::QuestHost {
    struct Attach {
        std::uint32_t quest = 0;
        bool on_init = false;
    };
    struct Objective {
        std::uint32_t quest = 0;
        std::int32_t index = 0;
        std::uint8_t bit = 0;
        bool on = false;
    };
    skydot::vm::QuestStates states;
    std::vector<Attach> attached;
    std::vector<std::int64_t> stopped;
    std::vector<Objective> objectives;
    /// Stages SetStage refuses.
    std::set<std::int64_t> refused_stages;
    std::map<std::uint32_t, std::uint32_t> infos; ///< INFO -> quest
    std::map<std::uint32_t, std::map<std::int64_t, skydot::vm::AliasInfo>> aliases; ///< quest -> id -> alias
    std::map<std::int64_t, double> globals;
    std::uint32_t next_handle = 0x5000;

    void attach_quest_scripts(std::uint32_t quest, bool on_init) override { attached.push_back({quest, on_init}); }
    skydot::vm::QuestStates& quests() override { return states; }
    bool start_quest(std::int64_t quest) override {
        auto& state = states.state(static_cast<std::uint32_t>(quest));
        state.running = state.started = true;
        return true;
    }
    void stop_quest(std::int64_t quest) override {
        stopped.push_back(quest);
        states.state(static_cast<std::uint32_t>(quest)).running = false;
    }
    bool set_stage(std::int64_t quest, std::int64_t stage) override {
        if (refused_stages.contains(stage)) {
            return false;
        }
        auto& state = states.state(static_cast<std::uint32_t>(quest));
        state.stage = static_cast<std::int32_t>(stage);
        state.done.insert(state.stage);
        return true;
    }
    void set_objective(std::uint32_t quest, std::int32_t index, std::uint8_t bit, bool on) override {
        objectives.push_back({quest, index, bit, on});
    }
    bool owning_quest(std::uint32_t info, std::uint32_t& quest) const override {
        const auto it = infos.find(info);
        if (it == infos.end()) {
            return false;
        }
        quest = it->second;
        return true;
    }
    bool alias_info(std::uint32_t quest, std::int64_t id, skydot::vm::AliasInfo& out) const override {
        const auto q = aliases.find(quest);
        if (q == aliases.end() || !q->second.contains(id)) {
            return false;
        }
        out = q->second.at(id);
        return true;
    }
    std::uint32_t alias_handle(std::uint32_t quest, std::uint32_t alias) override {
        if (const auto found = states.find_handle(quest, alias); found != 0) {
            return found;
        }
        states.add_handle(quest, alias, next_handle);
        return next_handle++;
    }
    double global_value(std::int64_t global) const override {
        const auto it = globals.find(global);
        return it != globals.end() ? it->second : 0.0;
    }
    void set_global_value(std::int64_t global, double value) override { globals[global] = value; }
};

} // namespace skydot::testing
