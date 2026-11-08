// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ragdoll a skeleton model carries (the converter's `bethconv.ragdoll`
// root extra, formats/pack-format.md "Ragdolls in meshes"): the game's rigid
// bodies of a race's skeleton.nif and the constraints between them, as Godot
// values. Everything of a body is in its bone's frame (the Skeleton3D bone of
// the same name), metres, Skyrim's axes (Z up), angles in radians.
//
//   bodies: [{bone, block, collision_object, collision_flags, heir_gain, vel_gain,
//             body_transform: Transform3D (the Havok body in the bone),
//             bone_bind: Transform3D (the bone at bind, NIF root space),
//             shapes: [{kind, block, havok_material, transform: Transform3D, radius,
//                       point_a, point_b: Vector3 (capsule, cylinder), half_extents: Vector3 (box),
//                       vertices: PackedVector3Array (hulls)}],
//             mass, center_of_mass: Vector3, inertia: Basis (kg m^2, rows as stored),
//             linear_damping, angular_damping, time_factor, gravity_factor, friction,
//             rolling_friction_multiplier, restitution, max_linear_velocity,
//             max_angular_velocity, penetration_depth, motion_type, deactivator_type,
//             solver_deactivation, quality_type,
//             layer, flags_and_part, part_number (bits 0-4), group,
//             constraints: PackedInt32Array}]
//   constraints: [{type, block, body_a, body_b (A the child), priority,
//                  in_a, in_b: {pivot: Vector3, and per type twist/plane/motor, axle/perp_1/perp_2,
//                               sliding/rotation/plane: Vector3},
//                  the type's limits (cone_max, plane_min, plane_max, twist_min, twist_max;
//                  min_angle, max_angle; min_distance, max_distance, friction; length),
//                  max_friction, motor: {type, enabled, min_force, max_force, ...},
//                  wrapper, strength, tau, damping, remove_when_broken (wrapped ones)}]
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace skydot {

/// The ragdoll of an instantiated model (see SkydotMaterials for how models are built);
/// an empty Dictionary if it has none. A constraint naming a body that is not there is left
/// out (the bodies' constraint lists are renumbered), as is a body or shape that is not a
/// Dictionary.
[[nodiscard]] godot::Dictionary model_ragdoll(const godot::Node* model);

} // namespace skydot
