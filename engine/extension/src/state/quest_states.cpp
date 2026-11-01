// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/quest_states.hpp"

#include <array>

namespace skydot::vm {

namespace {
/// A completed stage: index (u32).
constexpr std::size_t k_quest_stage_min_bytes = 4;
/// An objective: index (u32), bits (u8).
constexpr std::size_t k_quest_objective_min_bytes = 4 + 1;
/// A quest: id (u32), five flags (u8), stage (u32), done count (u32), objective count (u32).
constexpr std::size_t k_quest_min_bytes = 4 + 5 + 4 + 4 + 4;
/// A script-running quest: id (u32).
constexpr std::size_t k_quest_script_min_bytes = 4;
/// An alias handle: quest (u32), alias (u32), handle (u32), fill (u32).
constexpr std::size_t k_quest_handle_min_bytes = 4 + 4 + 4 + 4;
/// A quest global: id (u32), value (f64).
constexpr std::size_t k_quest_global_min_bytes = 4 + 8;
} // namespace

std::vector<std::uint8_t> QuestStates::save() const {
    save::ByteWriter q;
    q.u32(static_cast<std::uint32_t>(quests_.size()));
    for (const auto& [quest, state] : quests_) {
        const auto start = q.out.size();
        q.u32(quest);
        q.u8(state.running ? 1 : 0);
        q.u8(state.started ? 1 : 0);
        q.u8(state.completed ? 1 : 0);
        q.u8(state.failed ? 1 : 0);
        q.u8(state.active ? 1 : 0);
        q.u32(static_cast<std::uint32_t>(state.stage));
        q.u32(static_cast<std::uint32_t>(state.done.size()));
        for (const auto stage : state.done) {
            const auto stage_start = q.out.size();
            q.u32(static_cast<std::uint32_t>(stage));
            q.check_record(stage_start, k_quest_stage_min_bytes);
        }
        q.u32(static_cast<std::uint32_t>(state.objectives.size()));
        for (const auto& [index, bits] : state.objectives) {
            const auto objective_start = q.out.size();
            q.u32(static_cast<std::uint32_t>(index));
            q.u8(bits);
            q.check_record(objective_start, k_quest_objective_min_bytes);
        }
        q.check_record(start, k_quest_min_bytes);
    }
    q.u32(static_cast<std::uint32_t>(scripts_.size()));
    for (const auto quest : scripts_) {
        const auto start = q.out.size();
        q.u32(quest);
        q.check_record(start, k_quest_script_min_bytes);
    }
    q.u32(static_cast<std::uint32_t>(handles_.size()));
    for (const auto& [key, handle] : handles_) {
        const auto start = q.out.size();
        q.u32(key.first);
        q.u32(key.second);
        q.u32(handle);
        q.u32(fill(handle));
        q.check_record(start, k_quest_handle_min_bytes);
    }
    q.u32(static_cast<std::uint32_t>(globals_.size()));
    for (const auto& [global, value] : globals_) {
        const auto start = q.out.size();
        q.u32(global);
        q.f64(value);
        q.check_record(start, k_quest_global_min_bytes);
    }
    q.u32(handle_ids.next());
    return q.out;
}

bool QuestStates::load(save::ByteReader r, QuestStates& out) {
    out = QuestStates{};
    std::uint32_t n = 0;
    if (!r.count(n, k_quest_min_bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t quest = 0, stage = 0, done = 0, objectives = 0;
        std::uint8_t flags[5] = {};
        QuestState q;
        bool ok = r.u32(quest);
        for (auto& f : flags) {
            ok = ok && r.u8(f);
        }
        ok = ok && r.u32(stage) && r.count(done, k_quest_stage_min_bytes);
        if (!ok) {
            return false;
        }
        q.running = flags[0] != 0;
        q.started = flags[1] != 0;
        q.completed = flags[2] != 0;
        q.failed = flags[3] != 0;
        q.active = flags[4] != 0;
        q.stage = static_cast<std::int32_t>(stage);
        for (std::uint32_t k = 0; k < done; ++k) {
            std::uint32_t d = 0;
            if (!r.u32(d)) {
                return false;
            }
            q.done.insert(static_cast<std::int32_t>(d));
        }
        if (!r.count(objectives, k_quest_objective_min_bytes)) {
            return false;
        }
        for (std::uint32_t k = 0; k < objectives; ++k) {
            std::uint32_t index = 0;
            std::uint8_t bits = 0;
            if (!r.u32(index) || !r.u8(bits)) {
                return false;
            }
            q.objectives[static_cast<std::int32_t>(index)] = bits;
        }
        out.quests_[quest] = std::move(q);
    }
    if (!r.count(n, k_quest_script_min_bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t quest = 0;
        if (!r.u32(quest)) {
            return false;
        }
        out.scripts_.insert(quest);
    }
    if (!r.count(n, k_quest_handle_min_bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::array<std::uint32_t, 4> alias{};
        for (auto& v : alias) {
            if (!r.u32(v)) {
                return false;
            }
        }
        out.add_handle(alias[0], alias[1], alias[2]);
        out.set_fill(alias[2], alias[3]);
    }
    if (!r.count(n, k_quest_global_min_bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t global = 0;
        double value = 0.0;
        if (!r.u32(global) || !r.f64(value)) {
            return false;
        }
        out.globals_[global] = value;
    }
    std::uint32_t next = 0;
    return r.u32(next) && out.handle_ids.restore(next) && r.at_end();
}

} // namespace skydot::vm
