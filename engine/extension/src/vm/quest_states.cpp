// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/quest_states.hpp"

#include <array>

namespace skydot::vm {

std::vector<std::uint8_t> QuestStates::save() const {
    save::ByteWriter q;
    q.u32(static_cast<std::uint32_t>(quests_.size()));
    for (const auto& [quest, state] : quests_) {
        q.u32(quest);
        q.u8(state.running ? 1 : 0);
        q.u8(state.started ? 1 : 0);
        q.u8(state.completed ? 1 : 0);
        q.u8(state.failed ? 1 : 0);
        q.u8(state.active ? 1 : 0);
        q.u32(static_cast<std::uint32_t>(state.stage));
        q.u32(static_cast<std::uint32_t>(state.done.size()));
        for (const auto stage : state.done) {
            q.u32(static_cast<std::uint32_t>(stage));
        }
        q.u32(static_cast<std::uint32_t>(state.objectives.size()));
        for (const auto& [index, bits] : state.objectives) {
            q.u32(static_cast<std::uint32_t>(index));
            q.u8(bits);
        }
    }
    q.u32(static_cast<std::uint32_t>(scripts_.size()));
    for (const auto quest : scripts_) {
        q.u32(quest);
    }
    q.u32(static_cast<std::uint32_t>(handles_.size()));
    for (const auto& [key, handle] : handles_) {
        q.u32(key.first);
        q.u32(key.second);
        q.u32(handle);
        q.u32(fill(handle));
    }
    q.u32(static_cast<std::uint32_t>(globals_.size()));
    for (const auto& [global, value] : globals_) {
        q.u32(global);
        q.f64(value);
    }
    q.u32(handle_ids.next());
    return q.out;
}

bool QuestStates::load(save::ByteReader r, QuestStates& out) {
    out = QuestStates{};
    std::uint32_t n = 0;
    if (!r.count(n, 24)) {
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
        ok = ok && r.u32(stage) && r.count(done, 4);
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
        if (!r.count(objectives, 5)) {
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
    if (!r.count(n, 4)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t quest = 0;
        if (!r.u32(quest)) {
            return false;
        }
        out.scripts_.insert(quest);
    }
    if (!r.count(n, 16)) {
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
    if (!r.count(n, 12)) {
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
