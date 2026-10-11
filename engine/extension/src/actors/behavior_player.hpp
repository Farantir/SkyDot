// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotBehavior`: plays a placed model's Havok behaviour graph (the
// swinging wall traps) with the machine of behavior_machine.hpp.
//
// A model whose converter extras carry `behavior_graph` (the NIF's
// BSBehaviorGraphExtraData, `Traps\WallTrap\WallTrapCW\WallTraps.hkx`) is a
// behaviour project: folder meshes/<path>/, name the file's stem. Its project
// file (meshes/animationdata/<name>.txt) lists the character
// (Characters\Character01.hkx), whose rig is the skeleton the clips are
// bound to and whose behaviour is the graph. A clip's tracks drive the
// model's nodes with the bones' names (Base01, Spikes01), in game units like
// the NIF's node transforms. The node is a child of the model's root and
// steps itself.
//
// Events the graph raises (clip triggers, state enter/exit) go out as the
// `event_raised` signal for Papyrus' PlayAnimationAndWait.
#pragma once

#include "actors/behavior_machine.hpp"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>
#include <vector>

namespace skydot {

class SkydotBehavior : public godot::Node {
    GDCLASS(SkydotBehavior, godot::Node)

public:
    /// A behaviour player under `model` for reference `ref` (log lines only),
    /// or null if the model has no behaviour graph or its assets are missing
    /// from `pack`. Starts in the graph's start state.
    static godot::Node* attach(godot::Node* model, godot::Object* pack, std::int64_t ref);

    /// Send an event by name. Returns whether the current state, or the
    /// wildcard transitions, take it.
    bool send_event(const godot::String& event);
    [[nodiscard]] godot::String get_state() const;

    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    struct Target {
        godot::Node3D* node{};
        int position{-1};
        int rotation{-1};
    };
    struct ClipPose {
        godot::Ref<godot::Animation> animation;
        std::vector<Target> targets;
    };

    void apply();
    void report(const BehaviorOutput& out);

    std::unique_ptr<BehaviorMachine> machine_;
    std::vector<ClipPose> poses_;
    std::int64_t ref_{};
};

} // namespace skydot
