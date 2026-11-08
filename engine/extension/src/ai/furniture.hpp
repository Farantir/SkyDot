// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotFurniture`: a probe for the furniture system's pieces against a real pack, for
// tools/furniture_check.gd: which idle an actor plays for an action (IdleChooser, with the
// furniture facts the caller states) and one actor graph to send events to and advance. It
// drives nothing in the game.
#pragma once

#include "actors/actor_graph.hpp"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <memory>

namespace skydot {

class SkydotWorld;

class SkydotFurniture : public godot::RefCounted {
    GDCLASS(SkydotFurniture, godot::RefCounted)

public:
    godot::Error setup(const godot::Ref<SkydotWorld>& world);

    /// The idle of `action` for `actor` at `furniture` under the root behaviour `behaviour`.
    /// `facts`: quick (bool: IsEnteringInteractionQuick), sitting, sleeping, anim_type (ints), vars
    /// (graph variable name -> int). Returns {idle, editor_id, event} or {} when nothing is chosen;
    /// `unknown` lists the condition functions nothing answers.
    godot::Dictionary choose_idle(std::int64_t action, const godot::String& behaviour, std::int64_t actor,
                                  std::int64_t furniture, const godot::Dictionary& facts) const;

    /// Open the actor graph of a race's behaviour project; false (see `get_missing`) when there is none.
    bool open_graph(const godot::String& behaviour);
    godot::String get_missing() const { return missing_; }
    /// The root behaviour graph of the open graph (a virtual path), for `choose_idle`.
    godot::String get_graph_behaviour() const;
    /// The furniture markers of a model (SkydotWorld.build_model): [{offset, heading, animation_type,
    /// entry_points}], NIF units.
    godot::Array get_markers(godot::Node* model) const;
    /// Each returns {events, changes (strings "machine: from -> to"), playing}.
    godot::Dictionary graph_start();
    godot::Dictionary graph_send(const godot::String& event);
    godot::Dictionary graph_advance(double seconds);
    /// {} when no state has a clip, else {clip, file, time, mode, speed}.
    godot::Dictionary graph_playing() const;
    /// {x, y, z, yaw} of the playing clip at its time, in game units and radians, or {}.
    godot::Dictionary graph_root_motion() const;
    /// The motion of project clip `clip` at `time` seconds, or {}.
    godot::Dictionary root_motion(const godot::String& clip, double time) const;

protected:
    static void _bind_methods();

private:
    godot::Dictionary report(const BehaviorOutput& out) const;

    godot::Ref<SkydotWorld> world_;
    std::unique_ptr<ActorGraph> graph_;
    godot::String missing_;
};

} // namespace skydot
