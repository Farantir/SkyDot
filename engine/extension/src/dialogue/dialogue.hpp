// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dialogue selection over world.fb's dialogue tables (ADR 0004). Which
// greeting an actor gives, which topics the player may pick, which INFO
// answers a topic and what follows it, all decided with the shared condition
// evaluator and the dialogue states. Text only: no voice, no scenes. Nothing
// here touches Godot.
//
// - A topic is available when its quest runs, the quest's dialogue conditions
//   pass and one of its INFOs does. The INFO is the first, in group order,
//   whose conditions pass (subject: the speaker, target: the player, quest:
//   the topic's) and that is not a Say Once INFO already said; of a run of
//   INFOs flagged Random, a random one of those that pass.
// - Opening a dialogue speaks, in this order, the INFO of the first available
//   start topic of a top-level blocking branch (and then only blocking topics
//   are listed), else of the first available GREE or HELO topic by quest priority;
//   or nothing.
// - The topic list is the available start topics of top-level player
//   branches, by quest priority, topic priority (both high first) and id.
// - After an INFO: Goodbye closes; Invisible Continue speaks the first
//   available link at once; links (TCLT) make the list (with the top-level
//   topics after them, unless their branch is exclusive); else the top-level
//   list returns.
// - Response text keeps `<...>` tags as text, except `<Alias=X>` and
//   `<Alias.ShortName=X>`, which take the name of what fills alias X of the
//   topic's quest; unresolved tags are counted.
#pragma once

#include "conditions/conditions.hpp"
#include "dialogue/dialogue_states.hpp"
#include "world_generated.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace skydot::dialogue {

namespace wfb = bethconv::pack::wfb;

/// What dialogue asks of its owner besides conditions.
class DialogueHost {
public:
    virtual ~DialogueHost() = default;
    /// A number in [0, 1).
    [[nodiscard]] virtual double random() = 0;
    /// The name of an actor (what fills an alias); empty if unknown.
    [[nodiscard]] virtual std::string name_of(std::uint32_t ref) const = 0;
};

/// One response of an INFO. An INFO without responses gives one silent line,
/// so its fragments still have a place to start and end. `first` and `last`
/// mark the begin and end of an INFO (its fragments run there).
struct Line {
    std::string text;
    std::uint32_t info = 0;
    bool first = false;
    bool last = false;
};

struct TopicEntry {
    std::uint32_t topic = 0;
    std::string text;
    bool said = false;
};

struct Result {
    std::vector<Line> lines;
    std::vector<TopicEntry> topics;
    bool closed = false;
};

class Dialogue {
public:
    static constexpr std::uint32_t k_player = 0x14;
    /// Invisible Continue chains stop here (a loop in the data).
    static constexpr int k_max_chain = 8;

    Dialogue(const wfb::World& world, DialogueStates& states, conditions::ConditionWorld& conditions,
             DialogueHost& host)
        : world_(world), states_(states), conditions_(conditions), host_(host) {}

    /// The player starts talking to `speaker`.
    Result open(std::uint32_t speaker);
    /// The player picks a topic of the list.
    Result choose(std::uint32_t topic);
    void close();
    [[nodiscard]] bool is_open() const { return speaker_ != 0; }

    /// What opening would say and list, without saying it: no state changes
    /// and no dice (the first INFO that passes is taken).
    Result peek(std::uint32_t speaker);

    /// `<...>` tags left in text, in all lines spoken so far.
    [[nodiscard]] std::uint64_t unresolved_tags() const { return unresolved_tags_; }
    /// The INFO with this id, null if there is none (shared INFOs).
    [[nodiscard]] const wfb::DialogueInfo* info(std::uint32_t id) const;
    /// The topic an INFO belongs to; 0 if unknown.
    [[nodiscard]] std::uint32_t topic_of_info(std::uint32_t id) const;

private:
    struct Start {
        const wfb::DialogueBranch* branch;
        const wfb::DialogueTopic* topic;
        std::int32_t quest_priority;
    };

    void build_indexes() const;
    [[nodiscard]] conditions::ConditionContext context(const wfb::DialogueTopic& topic) const;
    [[nodiscard]] bool topic_open(const wfb::DialogueTopic& topic) const;
    [[nodiscard]] const wfb::DialogueInfo* pick(const wfb::DialogueTopic& topic, bool dice);
    [[nodiscard]] bool passes(const wfb::DialogueInfo& info, const wfb::DialogueTopic& topic) const;
    [[nodiscard]] std::int32_t quest_priority(std::uint32_t quest) const;
    [[nodiscard]] const wfb::DialogueBranch* branch_of(const wfb::DialogueTopic& topic) const;
    [[nodiscard]] std::vector<TopicEntry> top_level();
    [[nodiscard]] std::vector<TopicEntry> list_for(const std::vector<std::uint32_t>& topics);
    [[nodiscard]] bool entry_of(const wfb::DialogueTopic& topic, TopicEntry& out);
    void speak(const wfb::DialogueTopic& topic, const wfb::DialogueInfo& info, Result& out, bool commit,
               int depth);
    [[nodiscard]] std::string substitute(const std::string& text, const wfb::DialogueTopic& topic);
    void finish(Result& out);
    [[nodiscard]] Result begin(std::uint32_t speaker, bool commit);

    const wfb::World& world_;
    DialogueStates& states_;
    conditions::ConditionWorld& conditions_;
    DialogueHost& host_;

    std::uint32_t speaker_ = 0;
    bool blocking_only_ = false;
    bool dice_ = true;
    /// Set by `speak` for the last INFO: its links, and whether to list only them.
    std::vector<std::uint32_t> links_;
    bool exclusive_ = false;
    std::uint64_t unresolved_tags_ = 0;

    mutable bool indexed_ = false;
    mutable std::vector<Start> starts_;
    mutable std::vector<const wfb::DialogueTopic*> greetings_;
    mutable std::unordered_map<std::uint32_t, std::pair<const wfb::DialogueInfo*, std::uint32_t>> infos_;
};

} // namespace skydot::dialogue
