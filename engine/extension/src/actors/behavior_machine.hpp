// SPDX-License-Identifier: GPL-3.0-or-later
//
// A minimal Havok behaviour graph: one hkbStateMachine whose states play one
// clip each, the subset the swinging wall traps and their like need. Events
// (by name) take a transition of the current state, else a wildcard
// transition; clip triggers and state enter/exit notify events are raised as
// time passes and fed back into the machine. Nothing here touches Godot.
//
// Simplifications, all on purpose:
//   - Transition conditions (hkbExpressionCondition, e.g. "!BPlayerNear") are
//     treated as true.
//   - Transition blends are instant: the new state's pose replaces the old.
//   - A transition to the state already current is ignored.
//   - Of a state's transitions the first matching one wins (no priorities).
//   - Only clip generators are states' generators; any other generator is a
//     state without a clip, which holds the last pose.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::pack::afb {
struct Animation;
}

namespace skydot {

struct BehaviorTrigger {
    float time{};
    std::string event;
    bool relative_to_end{}; ///< `time` (0 or negative) counts back from the clip's end.
};

struct BehaviorClip {
    std::string name;
    std::string animation; ///< The animation file, as the graph states it.
    /// 0 single play, 1 looping, 2 user controlled, 3 ping-pong.
    int mode{};
    float speed{1.0F};
    float start_time{};
    float user_fraction{};
    std::vector<BehaviorTrigger> triggers;
    /// Seconds, set by the host once it has the animation.
    float duration{};
};

struct BehaviorTransition {
    std::string event;
    int to_state{-1}; ///< A state id.
};

struct BehaviorState {
    int id{};
    std::string name;
    int clip{-1}; ///< Index into BehaviorDef::clips, or -1.
    std::vector<std::string> enter_events;
    std::vector<std::string> exit_events;
    std::vector<BehaviorTransition> transitions;
    /// A NIF controller sequence the state plays (BGSGamebryoSequenceGenerator).
    std::string sequence{};
    /// Machines (indexes into BehaviorGraphDef::machines) that run side by
    /// side while this state is current.
    std::vector<int> nested{};
};

struct BehaviorDef {
    int start_state{};
    std::vector<BehaviorState> states;
    std::vector<BehaviorTransition> wildcard;
    std::vector<BehaviorClip> clips;
};

/// A behaviour graph: its state machines (each without clips of its own),
/// the clips they share, and the machines that run from the start.
struct BehaviorGraphDef {
    std::vector<BehaviorClip> clips;
    std::vector<BehaviorDef> machines;
    std::vector<int> roots;
};

/// The first behaviour graph of an animation asset; events are named, clips
/// are the graph's clip generators. Empty `machines` when the asset has none
/// (or was converted from LE). A graph that names no root runs its first
/// machine.
[[nodiscard]] BehaviorGraphDef behavior_graph_from_asset(const bethconv::pack::afb::Animation& asset);

/// The first root machine of `behavior_graph_from_asset`, with the clips.
/// Empty `states` when the asset has none.
[[nodiscard]] BehaviorDef behavior_from_asset(const bethconv::pack::afb::Animation& asset);

struct BehaviorChange {
    std::string from;
    std::string to;
    std::string event; ///< What caused it.
    std::string sequence; ///< The entered state's NIF sequence, if it has one.
    int machine{};        ///< Set by BehaviorSet.
};

/// What a step produced, in order.
struct BehaviorOutput {
    std::vector<std::string> events;       ///< Raised by the graph (not the ones sent in).
    std::vector<BehaviorChange> changes;
    /// Set by `send`: the state, or the wildcards, have a transition for the
    /// event, even one to the state already current (which changes nothing).
    bool accepted{};
};

class BehaviorMachine {
public:
    explicit BehaviorMachine(BehaviorDef def);

    /// Enter the start state.
    void start(BehaviorOutput& out);
    /// An event from outside; not echoed in `out.events`.
    void send(std::string_view event, BehaviorOutput& out);
    void advance(float seconds, BehaviorOutput& out);

    /// The clip to pose and the time within it, in clip seconds. -1 for no clip.
    [[nodiscard]] int clip() const;
    [[nodiscard]] float clip_time() const { return time_; }
    /// The clip to pose and its time: the current clip's, or, in a state
    /// without a clip, the last one that played and where it stopped (the
    /// pose is held). -1 before any clip played.
    [[nodiscard]] int pose_clip() const { return clip() >= 0 ? clip() : held_clip_; }
    [[nodiscard]] float pose_time() const { return clip() >= 0 ? time_ : held_time_; }
    [[nodiscard]] const BehaviorState* state() const;
    [[nodiscard]] const BehaviorDef& def() const { return def_; }
    /// Leave the machine unstarted again (its state is forgotten, no events).
    void stop();

private:
    [[nodiscard]] int find_state(int id) const;
    void change(int index, std::string_view cause, BehaviorOutput& out);
    void raise(std::string event, bool echo, BehaviorOutput& out);
    void drain(BehaviorOutput& out, bool* first_matched = nullptr);

    BehaviorDef def_;
    int current_{-1};
    float time_{};
    int held_clip_{-1}; ///< The last clip played, for states without one.
    float held_time_{};
    bool backwards_{}; ///< Ping-pong.
    std::vector<bool> fired_;
    struct Pending {
        std::string event;
    };
    std::vector<Pending> queue_;
};

/// The machines of a graph: the roots run from the start, side by side;
/// a machine named in the current state of a running one runs while that
/// state is current. Events go to every running machine.
class BehaviorSet {
public:
    explicit BehaviorSet(BehaviorGraphDef def);

    void start(BehaviorOutput& out);
    void send(std::string_view event, BehaviorOutput& out);
    void advance(float seconds, BehaviorOutput& out);

    [[nodiscard]] const BehaviorGraphDef& def() const { return def_; }
    [[nodiscard]] std::size_t machine_count() const { return machines_.size(); }
    /// Machines running now, in index order.
    [[nodiscard]] std::vector<int> running() const;
    [[nodiscard]] const BehaviorMachine& machine(int index) const { return machines_[static_cast<std::size_t>(index)]; }

private:
    /// Start machines that became wanted, stop those no longer wanted.
    void refresh(BehaviorOutput& out);
    void merge(int machine, BehaviorOutput& part, BehaviorOutput& out);

    BehaviorGraphDef def_;
    std::vector<BehaviorMachine> machines_;
    std::vector<bool> running_;
};

} // namespace skydot
