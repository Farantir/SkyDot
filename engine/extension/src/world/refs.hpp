// SPDX-License-Identifier: GPL-3.0-or-later
//
// Helpers shared by world.cpp and refs.cpp for what a reference does beyond
// being placed: scripts, and the node metadata pick_ref reads.
#pragma once

#include "world_generated.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/array.hpp>

#include <cstdint>

namespace skydot {

using ScriptVector = flatbuffers::Vector<flatbuffers::Offset<bethconv::pack::wfb::Script>>;

/// Scripts as SkydotWorld::get_ref_info describes them.
godot::Array script_list(const ScriptVector* scripts, bool from_ref);

/// Mark a placed model with its reference and cell ("skydot_ref",
/// "skydot_cell"). Activatable ones also get "skydot_activatable" and their
/// bounds in the node's own space ("skydot_bounds"), for pick_ref.
void tag_ref(godot::Node3D* node, std::uint32_t ref, std::uint32_t cell, bool activatable);

/// Whether a base of record type `type` is a door (DOOR).
bool door_type(std::uint32_t type);

} // namespace skydot
