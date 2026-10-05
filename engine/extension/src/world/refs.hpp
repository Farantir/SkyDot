// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a reference does beyond being placed: its scripts as SkydotWorld
// describes them, the node metadata pick_ref reads, and picking.
#pragma once

#include "world/fb_search.hpp"
#include "world_generated.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace skydot {

class WorldData;

using ScriptVector = flatbuffers::Vector<flatbuffers::Offset<bethconv::pack::wfb::Script>>;

/// The scripts reference `ref` of `cell` carries itself (VMAD), or null.
inline const bethconv::pack::wfb::RefScripts* ref_scripts(const bethconv::pack::wfb::Cell& cell,
                                                          std::uint32_t ref) {
    return lookup(cell.scripts(), ref);
}

/// Scripts as SkydotWorld::get_ref_info describes them.
godot::Array script_list(const ScriptVector* scripts, bool from_ref);

/// Mark a placed model with its reference and cell ("skydot_ref",
/// "skydot_cell"). Activatable ones also get "skydot_activatable" and their
/// bounds in the node's own space ("skydot_bounds"), for pick_ref.
void tag_ref(godot::Node3D* node, std::uint32_t ref, std::uint32_t cell, bool activatable);

/// Whether activating a reference of `base` (null if it has none) can do
/// anything: its type is usable, or it or the reference has scripts, or it is
/// a load door.
bool activatable(const WorldData& data, const bethconv::pack::wfb::Base* base, std::uint32_t cell,
                 std::uint32_t ref);

/// The usable reference the segment `from`-`to` (Godot space) points at among
/// the cells built under `root`, as SkydotWorld::pick_ref describes.
godot::Dictionary pick_ref(godot::Node* root, const godot::Vector3& from, const godot::Vector3& to);

/// Whether a base of record type `type` is a door (DOOR).
bool door_type(std::uint32_t type);

} // namespace skydot
