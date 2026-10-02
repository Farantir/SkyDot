# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds an interior, opens the plain doors near a spot and checks that their
# collision follows the model: each animated body's rotation in the physics
# server against its node's after the Open clip. Then a player walks from the
# spot along the heading (game degrees) and must get past the doors.
#
#   godot4.7 --headless --path game --script res://tools/door_check.gd -- \
#       --pack <pack> --cell RiverwoodSleepingGiantInn \
#       --at -790,-278,0 --heading 169 [--range 400] [--walk 3]
#
# Exits 1 if a body lags its door or the walk ends short of --walk metres.
extends SceneTree

const UNIT := 0.0142875 # metres per game unit
const OPEN_TICK := 10
const CHECK_TICK := 250 # the clips take about 2 s
const WALK_TICKS := 120

var _world: SkydotWorld
var _args := {}
var _cell := 0
var _doors: Array = []
var _bodies := {} # AnimatableBody3D -> rotation before opening
var _player: SkydotPlayer
var _start := Vector3()
var _ticks := 0
var _failed := false

func _initialize() -> void:
	var argv := OS.get_cmdline_user_args()
	for i in range(0, argv.size() - 1):
		_args[argv[i]] = argv[i + 1]
	var pack := SkydotPack.new()
	if pack.open(_args.get("--pack", "")) != OK:
		printerr("door_check: ", pack.get_error())
		quit(1)
		return
	_world = pack.open_world()
	_world.actors = false
	_cell = _world.find_cell(_args.get("--cell", "RiverwoodSleepingGiantInn"))
	if _cell == 0:
		printerr("door_check: no such cell")
		quit(1)
		return
	get_root().add_child(_world.build_cell(_cell))
	physics_frame.connect(_tick)

## The spot in engine metres.
func _spot() -> Vector3:
	var at: PackedStringArray = String(_args.get("--at", "-790,-278,0")).split(",")
	return Vector3(float(at[0]), float(at[2]), -float(at[1])) * UNIT

func _refs(node: Node) -> Array:
	var out := []
	for child in node.get_children():
		if child.has_meta("skydot_ref"):
			out.append(child)
		else:
			out.append_array(_refs(child))
	return out

func _animated_bodies(node: Node) -> Array:
	var out := []
	for child in node.get_children():
		if child is AnimatableBody3D:
			out.append(child)
		out.append_array(_animated_bodies(child))
	return out

func _find_doors() -> void:
	var reach := float(_args.get("--range", "400")) * UNIT
	for node in _refs(get_root()):
		var info: Dictionary = _world.get_ref_info(_cell, node.get_meta("skydot_ref"))
		if info.get("type", "") != "DOOR" or info["door"] != null:
			continue
		if node.global_position.distance_to(_spot()) > reach:
			continue
		_doors.append(node)
		for body in _animated_bodies(node):
			_bodies[body] = body.global_transform.basis.get_rotation_quaternion()
		print("door 0x%08X %s, %d animated bodies" % [node.get_meta("skydot_ref"), info["editor_id"],
			_animated_bodies(node).size()])

func _check_bodies() -> void:
	for body: AnimatableBody3D in _bodies:
		var node_q := body.global_transform.basis.get_rotation_quaternion()
		var state: Transform3D = PhysicsServer3D.body_get_state(body.get_rid(), PhysicsServer3D.BODY_STATE_TRANSFORM)
		var swing := rad_to_deg(node_q.angle_to(_bodies[body]))
		var lag := rad_to_deg(node_q.angle_to(state.basis.get_rotation_quaternion()))
		var shift := body.global_position.distance_to(state.origin)
		var bad := lag > 1.0 or shift > 0.01
		_failed = _failed or bad
		print("%s: swung %.1f deg, physics off by %.1f deg, %.3f m%s" % [body.get_parent().name, swing, lag, shift,
			"  FAIL" if bad else ""])

func _tick() -> void:
	_ticks += 1
	if _ticks == 2:
		_find_doors()
		if _doors.is_empty():
			printerr("door_check: no plain door near the spot")
			quit(1)
	elif _ticks == OPEN_TICK:
		for door in _doors:
			var animator = door.get_node_or_null("SkydotAnimator")
			if animator == null or not animator.play("Open"):
				print("0x%08X has no Open clip" % door.get_meta("skydot_ref"))
	elif _ticks == CHECK_TICK:
		_check_bodies()
		_player = SkydotPlayer.new()
		get_root().add_child(_player)
		_start = _spot()
		_player.teleport(_start + Vector3(0, 0.05, 0))
		# The game's heading turns clockwise from north (-Z here).
		_player.set_look(-deg_to_rad(float(_args.get("--heading", "169"))), 0.0)
		_player.set_input(Vector2(0, 1), 0.0, SkydotPlayer.RUN)
	elif _ticks == CHECK_TICK + WALK_TICKS:
		var p := _player.global_position
		var walked := Vector2(p.x - _start.x, p.z - _start.z).length()
		var need := float(_args.get("--walk", "3"))
		var bad := walked < need
		_failed = _failed or bad
		print("walked %.2f m of %.1f%s" % [walked, need, "  FAIL" if bad else ""])
		quit(1 if _failed else 0)
