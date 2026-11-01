# SPDX-License-Identifier: GPL-3.0-or-later
#
# What scripts do to clutter: push it (ApplyHavokImpulse), hold or release it
# (SetMotionType), and wake what rested on a thing that went away or moved.
class_name ClutterPhysics
extends RefCounted

## Metres added to a node's half diagonal for the clutter that falls once it is gone.
const WAKE_MARGIN := 0.5
## SetMotionType values that hold clutter: keyframed and fixed. The others release it.
const MOTION_HOLDING := [4, 5]

var _world: SkydotWorld
var _host: Node3D  # the place's root, where waking clutter starts


func _init(world: SkydotWorld, host: Node3D) -> void:
	_world = world
	_host = host


## Clutter resting on or against `node` falls once it is gone or moves.
func wake_around(node: Node) -> void:
	var bounds := Place.mesh_bounds(node)
	if bounds.size != Vector3.ZERO:
		SkydotWorld.wake_clutter(_host, bounds.get_center(), bounds.size.length() / 2 + WAKE_MARGIN)


func _body(ref: int) -> SkydotDynamicBody:
	var node := _world.find_ref_node(ref)
	if node == null:
		return null
	var bodies := node.find_children("*", "SkydotDynamicBody", true, false)
	return bodies[0] if not bodies.is_empty() else null


## ApplyHavokImpulse: Skyrim's direction, Havok's magnitude (Havok units are
## metres here, so it applies as is).
func on_havok_impulse(ref: int, direction: Vector3, magnitude: float) -> void:
	var body := _body(ref)
	if body == null:
		print("0x%08X has no movable body for an impulse" % ref)
		return
	body.wake()
	var godot_direction := SkydotWorld.skyrim_position(direction).normalized()
	body.apply_central_impulse(godot_direction * magnitude)


## SetMotionType: the moving types release clutter, keyframed and fixed hold
## it. Static models cannot be made to move.
func on_motion_type(ref: int, motion_type: int) -> void:
	var body := _body(ref)
	if body == null:
		print("0x%08X has no movable body for motion type %d" % [ref, motion_type])
		return
	if motion_type in MOTION_HOLDING:
		body.freeze = true
	else:
		body.wake()
