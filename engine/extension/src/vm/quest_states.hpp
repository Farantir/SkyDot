// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a game has done with its quests, kept and saved (the QUST section):
// each quest's run state, stages and objectives, which quests have their
// scripts attached, the alias objects (a handle per quest alias, from the
// alias range of the ID allocator) with what fills them, and the values
// scripts gave to global variables. The rules of starting a quest and running
// a stage's fragment are SkydotPapyrus's (vm/quests.cpp). Nothing here
// touches Godot.
#pragma once

#include "save/bytes.hpp"
#include "vm/id_ranges.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace skydot::vm {

struct QuestState {
    bool running = false;
    bool started = false; ///< Has ever started.
    bool completed = false;
    bool failed = false;
    bool active = false;
    std::int32_t stage = 0;
    std::set<std::int32_t> done;
    /// Objective index -> bit 0 displayed, 1 completed, 2 failed.
    std::map<std::int32_t, std::uint8_t> objectives;
};

class QuestStates {
public:
    static constexpr std::uint32_t k_section_version = 1;
    using AliasKey = std::pair<std::uint32_t, std::uint32_t>; ///< quest, alias id

    // ---- quests
    /// The state of `quest`, made if there is none.
    QuestState& state(std::uint32_t quest) { return quests_[quest]; }
    [[nodiscard]] const QuestState* find(std::uint32_t quest) const {
        const auto it = quests_.find(quest);
        return it != quests_.end() ? &it->second : nullptr;
    }
    [[nodiscard]] const std::map<std::uint32_t, QuestState>& quests() const { return quests_; }

    // ---- scripts
    [[nodiscard]] bool scripts_attached(std::uint32_t quest) const { return scripts_.contains(quest); }
    void mark_scripts_attached(std::uint32_t quest) { scripts_.insert(quest); }

    // ---- aliases
    /// The handle of alias `alias` of `quest`; 0 if it has none.
    [[nodiscard]] std::uint32_t find_handle(std::uint32_t quest, std::uint32_t alias) const {
        const auto it = handles_.find({quest, alias});
        return it != handles_.end() ? it->second : 0;
    }
    /// Give alias `alias` of `quest` the handle `handle` (from the allocator).
    void add_handle(std::uint32_t quest, std::uint32_t alias, std::uint32_t handle) {
        handles_[{quest, alias}] = handle;
        owners_[handle] = {quest, alias};
    }
    /// The quest and alias a handle stands for; null if it is none.
    [[nodiscard]] const AliasKey* owner(std::uint32_t handle) const {
        const auto it = owners_.find(handle);
        return it != owners_.end() ? &it->second : nullptr;
    }
    /// What fills the alias behind `handle`; 0 if empty.
    [[nodiscard]] std::uint32_t fill(std::uint32_t handle) const {
        const auto it = fills_.find(handle);
        return it != fills_.end() ? it->second : 0;
    }
    void set_fill(std::uint32_t handle, std::uint32_t ref) { fills_[handle] = ref; }
    /// Empty every alias of `quest`.
    void clear_fills(std::uint32_t quest) {
        for (auto& [handle, fill] : fills_) {
            if (const AliasKey* key = owner(handle); key != nullptr && key->first == quest) {
                fill = 0;
            }
        }
    }
    [[nodiscard]] const std::map<std::uint32_t, std::uint32_t>& fills() const { return fills_; }
    /// (alias id, handle) of every alias `quest` has made, by alias id.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>> aliases_of(std::uint32_t quest) const {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
        for (auto it = handles_.lower_bound({quest, 0}); it != handles_.end() && it->first.first == quest; ++it) {
            out.emplace_back(it->first.second, it->second);
        }
        return out;
    }
    /// The handles of `quest`'s aliases in the order they were made (the
    /// allocator hands them out ascending).
    [[nodiscard]] std::vector<std::uint32_t> handles_of(std::uint32_t quest) const {
        std::vector<std::uint32_t> out;
        for (const auto& [alias, handle] : aliases_of(quest)) {
            out.push_back(handle);
        }
        std::ranges::sort(out);
        return out;
    }

    // ---- global variables scripts set
    [[nodiscard]] const double* global(std::uint32_t id) const {
        const auto it = globals_.find(id);
        return it != globals_.end() ? &it->second : nullptr;
    }
    void set_global(std::uint32_t id, double value) { globals_[id] = value; }

    /// Where alias handles come from (id_ranges::alias_handles); 0 once used up.
    IdAllocator handle_ids{id_ranges::alias_handles};

    void clear() { *this = QuestStates{}; }

    [[nodiscard]] std::vector<std::uint8_t> save() const;
    /// Decode a section into a fresh object; false if it is cut short or
    /// has bytes left over.
    [[nodiscard]] static bool load(save::ByteReader r, QuestStates& out);

private:
    std::map<std::uint32_t, QuestState> quests_;
    std::set<std::uint32_t> scripts_;
    std::map<AliasKey, std::uint32_t> handles_;
    std::map<std::uint32_t, AliasKey> owners_;
    /// Alias handle -> the reference or location filling it.
    std::map<std::uint32_t, std::uint32_t> fills_;
    std::map<std::uint32_t, double> globals_;
};

} // namespace skydot::vm
