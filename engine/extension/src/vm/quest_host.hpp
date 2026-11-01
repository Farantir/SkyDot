// SPDX-License-Identifier: GPL-3.0-or-later
//
// `QuestHost`: what the quest-related natives change or ask of the quest
// state beyond records: the state itself, starting and stopping, stages,
// objectives, aliases and global variables.
#pragma once

#include "state/quest_states.hpp"

#include <cstdint>
#include <string>

namespace skydot::vm {

/// A quest alias as the data names it.
struct AliasInfo {
    std::string name;
    /// A location alias rather than a reference alias.
    bool location = false;
};

class QuestHost {
public:
    virtual ~QuestHost() = default;

    /// Attach the scripts of quest `quest` if it is one and has none yet;
    /// `on_init` also sends their OnInit. Anything else is left alone.
    virtual void attach_quest_scripts(std::uint32_t quest, bool on_init) = 0;
    /// The runtime state of all quests and aliases (the save carries it).
    [[nodiscard]] virtual QuestStates& quests() = 0;
    /// Quest.Start: run `quest`, with its start-up work; false if it cannot start.
    virtual bool start_quest(std::int64_t quest) = 0;
    /// Quest.Stop: stop `quest`, with the work stopping does.
    virtual void stop_quest(std::int64_t quest) = 0;
    /// Quest.SetStage: advance `quest` to `stage`, with the work a stage does;
    /// false if the stage is refused.
    virtual bool set_stage(std::int64_t quest, std::int64_t stage) = 0;
    /// Set (`on`) or clear `bit` of objective `index` of `quest`, with the work that does.
    virtual void set_objective(std::uint32_t quest, std::int32_t index, std::uint8_t bit, bool on) = 0;
    /// The quest the INFO `info` belongs to; false if it is none's.
    [[nodiscard]] virtual bool owning_quest(std::uint32_t info, std::uint32_t& quest) const = 0;
    /// Alias `id` of `quest` from the data; false if it has none.
    [[nodiscard]] virtual bool alias_info(std::uint32_t quest, std::int64_t id, AliasInfo& out) const = 0;
    /// The form id that stands for alias `alias` of `quest` in scripts.
    [[nodiscard]] virtual std::uint32_t alias_handle(std::uint32_t quest, std::uint32_t alias) = 0;
    /// The value of GLOB `global`.
    [[nodiscard]] virtual double global_value(std::int64_t global) const = 0;
    /// Set the value of GLOB `global`.
    virtual void set_global_value(std::int64_t global, double value) = 0;
};

} // namespace skydot::vm
