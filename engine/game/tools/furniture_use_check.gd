# SPDX-License-Identifier: GPL-3.0-or-later
#
# Actors using furniture in a built place without a window (the game session of the smoke tests, a
# player that stands where the script says): prints, over time, where an actor is, what GetSitting /
# GetSleeping say and the clip it plays, to compare with the numbers measured in the game.
#
#   godot4.7 --headless --path game --script res://tools/furniture_use_check.gd -- --pack <pack> \
#       [--case spider|draugr|sigrid-chair|sigrid-bed|all] [--every 0.25]
#
# spider  0x3A1E1 (Bleak Falls Barrow): in its furniture 0x46BEF; the player enters trigger 0xAA83E.
# draugr  0x380B5: in its alcove; the player stands 250 units off for 8 s, then combat starts.
# sigrid-chair 0x13483 in her house: walks to her chair at 20:00, sits, gets up at 21:00.
# sigrid-bed: placed on load at 23:30, asleep.
extends SceneTree

const PLAYER := 0x14
const STEP := 1.0 / 60.0
const SPIDER := 0x3A1E1
const DRAUGR := 0x380B5
const SIGRID := 0x13483
const SPIDER_TRIGGER := 0xAA83E

var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _every := 0.25
var _time := 0.0
var _last_package := -1
var _pack := ""
var _case := "all"


func _initialize() -> void:
	var argv := OS.get_cmdline_user_args()
	var args := {}
	for i in range(0, argv.size() - 1):
		args[argv[i]] = argv[i + 1]
	_every = float(args.get("--every", "0.25"))
	_pack = args.get("--pack", "")
	_case = args.get("--case", "all")
	_main()


func _frames(count: int) -> void:
	for i in count:
		_camera.global_position = _feet + Vector3.UP * 1.6
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100.0)
		_session.step(STEP, _feet)
		_time += STEP
		await physics_frame


func _open(pack_dir: String, time_hour: float, time_scale: float, quests: bool = false) -> bool:
	var config := GameConfig.new()
	config.pack = pack_dir
	config.quests = quests
	config.preload_doors = false
	config.wander = true
	config.time = time_hour
	config.time_scale = time_scale
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	root.add_child(_session)
	return _session.open(config, _camera, PlayerBody.new()) and _session.start_game()


func _skyrim(node: Node3D) -> Vector3:
	return SkydotWorld.godot_to_skyrim(node.global_position)


# The yaw the actor faces, in Skyrim degrees (clockwise from +Y).
func _angle(node: Node3D) -> float:
	var forward := -node.global_transform.basis.z
	return fposmod(rad_to_deg(atan2(forward.x, -forward.z)), 360.0)


func _line(ref: int, label: String) -> void:
	var node := _session.world.find_ref_node(ref) as SkydotActor
	if node == null:
		print("%6.2f %s: not built" % [_time, label])
		return
	var f: Dictionary = _session.ai.get_furniture_state(ref)
	var p := _skyrim(node)
	var st: Dictionary = _session.ai.get_actor_state(ref)
	if st.get("package", 0) != _last_package:
		_last_package = st.get("package", 0)
		print("   package ", st.get("package_editor_id"), " steps ", ",".join(st.get("steps", [])), " seconds ", ",".join(PackedStringArray(Array(st.get("step_seconds", PackedFloat64Array())).map(func(x): return str(x)))))
	print("%6.2f h %.2f %s %s pos (%.2f, %.2f, %.2f) angle %.2f  use 0x%X state %s  clip %s  floor %s" % [
		_time, _session.ai.get_hour(), st.get("package_editor_id", ""), label, p.x, p.y, p.z, _angle(node), f.get("ref", 0), f.get("state", "-"),
		f.get("clip", node.get_playing()), node.is_on_floor()])


func _watch(ref: int, label: String, seconds: float) -> void:
	var next := 0.0
	var until := _time + seconds
	while _time < until:
		await _frames(1)
		if _time >= next:
			next = _time + _every
			_line(ref, label)


func _enter(cell: String, at: Vector3, watch_ref: int) -> bool:
	if not _session.enter_cell(cell, SkydotWorld.skyrim_position(at), null):
		printerr("furniture_use_check: cannot enter ", cell)
		return false
	_feet = SkydotWorld.skyrim_position(at)
	var node := _session.world.find_ref_node(watch_ref) as Node3D
	if node == null:
		printerr("furniture_use_check: 0x%X is not built" % watch_ref)
		return false
	_session.ai.attach_built(node.get_parent())
	return true


func _info(ref: int) -> Dictionary:
	return _session.world.get_ref_info(_session.world.get_ref_cell(ref), ref)


func _spider() -> void:
	print("== spider 0x%X: the player enters trigger 0x%X" % [SPIDER, SPIDER_TRIGGER])
	if not _enter("BleakFallsBarrow01", Vector3(5006.8, -893.3, -636), SPIDER):
		return
	var papyrus := _session.papyrus
	print("state ", _session.ai.get_actor_state(SPIDER))
	await _watch(SPIDER, "spider", 3.0)
	print("-- the player walks into the trigger")
	_feet = SkydotWorld.skyrim_position(_info(SPIDER_TRIGGER)["position"])
	var until := _time + 14.0
	var next := 0.0
	while _time < until:
		await _frames(1)
		if _time >= next:
			next = _time + _every
			_line(SPIDER, "spider Variable01 %s Aggression %s combat %s" % [papyrus.get_actor_value(SPIDER, "Variable01"),
				papyrus.get_actor_value(SPIDER, "Aggression"), _session.ai.get_combat_state(SPIDER)])


func _draugr() -> void:
	print("== draugr 0x%X: asleep with the player 250 units away, then combat" % DRAUGR)
	if not _enter("BleakFallsBarrow01", Vector3(5006.8, -893.3, -636), DRAUGR):
		return
	var node := _session.world.find_ref_node(DRAUGR) as Node3D
	var forward := -node.global_transform.basis.z
	forward.y = 0.0
	_feet = node.global_position + forward.normalized() * 250.0 * SkydotWorld.unit_scale()
	await _watch(DRAUGR, "draugr", 8.0)
	print("-- startcombat player")
	_session.ai.start_combat(DRAUGR, PLAYER)
	await _watch(DRAUGR, "draugr", 12.0)


func _sigrid(hour: float, seconds: float, label: String) -> void:
	print("== sigrid 0x%X (%s) from %.2f h" % [SIGRID, label, hour])
	if not _enter("RiverwoodAlvorsHouse", Vector3(-400, -100, 240), SIGRID):
		return
	await _watch(SIGRID, "sigrid", seconds)


func _main() -> void:
	await process_frame
	if _case in ["spider", "all"]:
		if _open(_pack, 12.0, 20.0):
			await _spider()
		_close()
	if _case in ["draugr", "all"]:
		if _open(_pack, 12.0, 20.0):
			await _draugr()
		_close()
	if _case in ["sigrid-chair", "all"]:
		if _open(_pack, 19.95, 60.0, true):
			await _sigrid(19.95, 100.0, "chair")
		_close()
	if _case in ["sigrid-bed", "all"]:
		if _open(_pack, 23.5, 20.0, true):
			await _sigrid(23.5, 5.0, "bed")
		_close()
	quit(0)


func _close() -> void:
	if _session != null:
		_session.queue_free()
	if _camera != null:
		_camera.queue_free()
	_session = null
	_camera = null
