// SPDX-License-Identifier: GPL-3.0-or-later
//
// The behaviour graph of an actor, as Skyrim runs it for furniture and other
// scripted animation: the race's behaviour project -> its character -> the
// root behaviour graph (0_Master.hkx for a human), with every
// hkbBehaviorReferenceGenerator expanded, so that the state that references a
// graph runs the referenced graph's root machines as nested machines. Events
// and variables are named, so graphs are joined by name. The machines run in
// one BehaviorSet (the same system as the traps' behaviours).
//
// `ActorGraphDef` is the immutable result, shared by every actor of a project
// (ActorGraphLibrary caches it per behaviour file); `ActorGraph` is one actor's
// running copy. No Godot in here.
#pragma once

#include "actors/animation_project.hpp"
#include "actors/behavior_machine.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace skydot {

/// Where the actor is after some time of a clip at speed 1 (game units, Z up, +Y forward, unscaled)
/// and which way it faces (radians about Z).
struct RootMotion {
    float x{};
    float y{};
    float z{};
    float yaw{};
};

struct ActorGraphDef {
    BehaviorGraphDef graph;
    /// The virtual path of each `graph.clips` entry's animation.
    std::vector<std::string> clip_files;
    /// How many states of nested machines lie above each machine (a root: 0).
    std::vector<int> depth;
    AnimationProject project;
    /// For each machine the states that nest it: (machine, state id).
    std::vector<std::vector<std::pair<int, int>>> parents;
    std::string behaviour; ///< The root behaviour graph's virtual path.
    std::string missing;   ///< Why there is no graph, or empty.

    [[nodiscard]] bool empty() const { return graph.machines.empty(); }
    /// The project clip `name` (a clip generator's name, any case) at `time` seconds, interpolated;
    /// clamped to its ends. Nothing if the project has no motion for it.
    [[nodiscard]] std::optional<RootMotion> root_motion(const std::string& clip, float time) const;
};

/// The actor graph of a race's behaviour project (`actors/character/defaultmale.hkx`).
std::shared_ptr<const ActorGraphDef> build_actor_graph(const std::string& behaviour, const AssetReader& read);

/// Graphs cached by behaviour file, and the lengths of clip animations by file.
class ActorGraphLibrary {
public:
    explicit ActorGraphLibrary(AssetReader read) : read_(std::move(read)) {}

    /// Never null; `missing` says why it is empty.
    std::shared_ptr<const ActorGraphDef> graph(const std::string& behaviour);
    /// The animation's length in seconds, 0 if the pack has none.
    float clip_duration(const std::string& vpath);

private:
    AssetReader read_;
    std::unordered_map<std::string, std::shared_ptr<const ActorGraphDef>> graphs_;
    std::unordered_map<std::string, float> durations_;
};

/// One clip playing.
struct PlayingClip {
    std::string clip;   ///< The clip generator's name.
    std::string file;   ///< The animation's virtual path.
    float time{};       ///< Seconds into the clip.
    int mode{};         ///< 0 once, 1 loop, 2 user controlled, 3 ping-pong.
    float speed{1.0F};
};

class ActorGraph {
public:
    /// `duration(vpath)` gives a clip animation's length (ActorGraphLibrary::clip_duration), asked for
    /// when a state with that clip is entered.
    ActorGraph(std::shared_ptr<const ActorGraphDef> def, std::function<float(const std::string&)> duration);

    /// The variables the game sets for an actor before its graph runs (a non-player character: IsNPC 1,
    /// IsPlayer 0, third person); the graph's own initial values are the player's.
    void configure(bool player);
    BehaviorOutput start();
    BehaviorOutput send(std::string_view event);
    /// Send an event that takes the actor into a furniture (or out of its way): as `send`, but an event no
    /// running machine takes and that a machine nested below a running one handles (a wildcard of the
    /// furniture machines, which run only while the actor is in a furniture state) first puts the running
    /// machines into the states that nest it, as the game's enter events do, then delivers it. The machine
    /// that left its state and the state it left are remembered: `in_furniture` says whether it has come back.
    BehaviorOutput send_entering(std::string_view event);
    /// The root-most machine `send_entering` took out of its state is not back in it.
    [[nodiscard]] bool in_furniture() const;
    /// Events the graph raises come back in `events`, state changes in `changes`.
    BehaviorOutput advance(float seconds);
    /// A graph variable the game sets from outside (IsNPC, ...); see BehaviorSet::set_variable.
    void set_variable(std::string_view name, float value) { set_.set_variable(name, value); }

    /// The clip of the deepest running machine whose current state has one; nothing when none of
    /// the running states has a clip (the actor's normal animation then).
    [[nodiscard]] std::optional<PlayingClip> playing() const;
    [[nodiscard]] const ActorGraphDef& def() const { return *def_; }
    [[nodiscard]] const BehaviorSet& set() const { return set_; }
    /// Root motion of the clip now playing at its time, nothing if there is none.
    [[nodiscard]] std::optional<RootMotion> root_motion() const;

private:
    void resolve_durations();

    std::shared_ptr<const ActorGraphDef> def_;
    std::function<float(const std::string&)> duration_;
    BehaviorSet set_;
    std::vector<bool> known_;
    int home_machine_{-1};
    int home_state_{-1};
};

} // namespace skydot
