// SPDX-License-Identifier: GPL-3.0-or-later
//
// A place built ahead of its use is held: in the scene, hidden, without
// physics (disabled bodies are not in the space) and with its navigation
// regions and links off the map.
// What Godot creates for its nodes then happens within the preparation's
// budget, not on arrival. The streamer holds what it prepares behind a door;
// the places and the streamer release it.
#pragma once

#include <godot_cpp/classes/node3d.hpp>

namespace skydot::held_place {

/// Add `node` to `host` held.
void hold(godot::Node* host, godot::Node3D* node);
/// Undo `hold` on arrival.
void release(godot::Node3D* node);

} // namespace skydot::held_place
