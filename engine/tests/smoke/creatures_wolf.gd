# SPDX-License-Identifier: GPL-3.0-or-later
#
# A wolf near Riverwood on the real SE pack (SKYDOT_SSE_PACK), headless: EncWolf
# 0x9F361 (aggression 0, an aggro radius of 1500 units) on the road east of Riverwood
# attacks the player who comes within it, with a clip of its project, and dies to
# `kill_actor` in a generic ragdoll. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const WOLF := 0x9F361
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _hits: Array[Dictionary] = []
var _clips: Array[Array] = []


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false
	var loaded_position := Vector3.ZERO
	var loaded_yaw := 0.0
	var loaded_pitch := 0.0

	func place_at(_at: Vector3, new_yaw: float, new_pitch: float) -> void:
		yaw = new_yaw
		pitch = new_pitch

	func set_view_distance(_metres: float) -> void:
		pass

	func save_section() -> PackedByteArray:
		return PackedByteArray()

	func load_section(_bytes: PackedByteArray) -> bool:
		return true


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_creatures_wolf: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _frames(count: int) -> void:
	for i in count:
		_camera.global_position = _feet + Vector3.UP * 1.6
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100.0)
		_session.step(STEP, _feet)
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.quests = false
	config.preload_doors = false
	config.wander = false
	config.radius = 1
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, _camera, FakePlayer.new()), "the session opens")
	if _session.world == null or _session.ai == null:
		quit(1)
		return
	var world := _session.world
	var papyrus := _session.papyrus
	expect(_session.start_game(), "the game starts")
	var wolf_at := Vector3(21182.5, -33186.8, 1137.7)  # Skyrim units
	_feet = SkydotWorld.skyrim_position(wolf_at + Vector3(900.0, 0.0, 60.0))
	expect(_session.enter_world("Tamriel", _feet, null), "the worldspace is entered")
	_session.hit_landed.connect(func(target: int, result: Dictionary, _point: Vector3) -> void:
		_hits.append({"target": target, "result": result}))
	_session.combat.npc_attack_started.connect(func(attacker: int, clip: String, _power: bool) -> void: _clips.append([attacker, clip]))
	var built := await _until(func() -> bool: return world.find_ref_node(WOLF) != null, 60.0)
	expect(built >= 0.0, "the wolf is built")
	var node := world.find_ref_node(WOLF) as SkydotActor
	if node == null:
		quit(1)
		return
	# Stand within its aggro radius, on the ground.
	# A spot 5 m off with a clear line from the wolf's eyes to the player's (trees and rocks stand about).
	var space := node.get_world_3d().direct_space_state
	for i in 16:
		var spot := node.global_position + Vector3(5.0, 0.4, 0.0).rotated(Vector3.UP, i * TAU / 16.0)
		var query := PhysicsRayQueryParameters3D.create(node.global_position + Vector3.UP * node.height * 0.93, spot + Vector3.UP * 1.7,
			SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
		if space.intersect_ray(query).is_empty():
			_feet = spot
			break
	await _frames(60)
	expect(papyrus.get_actor_value(WOLF, "Aggression") == 0, "the wolf is unaggressive: its aggro radius makes it attack")
	var home := node.global_position
	var waited := await _until(func() -> bool: return _session.ai.get_combat_state(WOLF) == 1, 15.0)
	expect(waited >= 0.0, "the wolf enters combat with the player")
	var hit_time := await _until(func() -> bool: return _hits.any(func(h: Dictionary) -> bool: return h["result"]["attacker"] == WOLF), 30.0)
	print("wolf: first hit after %s s, clips %s" % [hit_time, _clips])
	expect(hit_time >= 0.0, "the wolf bites the player")
	expect(node.global_position.distance_to(home) > 0.5, "it moved: %.2f m" % node.global_position.distance_to(home))
	expect(_clips.any(func(c: Array) -> bool: return String(c[1]).to_lower().contains("attack")), "with an attack clip: %s" % [_clips])
	_session.kill_actor(WOLF)
	await _frames(10)
	expect(papyrus.is_actor_dead(WOLF) and _session.ai.get_combat_state(WOLF) == 0, "it dies and leaves combat")
	var ragdoll := node.find_child("Ragdoll", true, false)
	expect(ragdoll != null and ragdoll.get_child_count() >= 8, "in a ragdoll: %d bodies" % [ragdoll.get_child_count() if ragdoll != null else 0])
	print("smoke_creatures_wolf: failures=%d" % failures)
	quit(failures)
