// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyrim's space to Godot's. world.fb keeps Skyrim's: Z-up, game units,
// radians. These apply the axis rotation (Z-up -> Y-up, -90 degrees about X)
// and the unit scale (formats::k_metres_per_unit) the converter applies to
// every mesh, so a model placed with `skyrim_transform` lands where the game
// puts it. SkydotWorld binds them for scripts.
#pragma once

#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace skydot {

/// Skyrim position (game units), rotation (radians) and scale to a Godot
/// transform.
godot::Transform3D skyrim_transform(const godot::Vector3& position, const godot::Vector3& rotation,
                                    double scale);
/// Skyrim position (game units) to Godot space (metres).
godot::Vector3 skyrim_position(const godot::Vector3& position);
/// Godot space (metres) back to a Skyrim position (game units).
godot::Vector3 godot_to_skyrim(const godot::Vector3& position);

} // namespace skydot
