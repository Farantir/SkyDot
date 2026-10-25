# SPDX-License-Identifier: GPL-3.0-or-later
#
# A dragon on the ground on the real SE pack (SKYDOT_SSE_PACK), headless, in phases: EncDragon01Fire
# 0x1CA03 (DragonRace, dragonproject.hkx, the Dragon_Default_MT movement type: 384 units/s forward,
# 0.589 rad/s turning in place) is built with its skeleton and a clip table, fights the player in
# the combat stance, turns toward him at the movement type's rate when he stands beside it, and walks to
# him. The plain between Whiterun and the Western Watchtower around Skyrim (13100, -13650) is within
# 2.4 m of flat over 35 m. Later phases add the bite, the tail and the breath. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const DRAGON := 0x1CA03
const STEP := 1.0 / 60.0
const SPOT := Vector3(13100.0, -13650.0, -3000.0)  # Skyrim units; the ground is below
const TURN_RATE := 0.5890486  # rad/s, Dragon_Default_MT rotate_in_place
const WALK_SPEED := 384.0 * 0.0142875  # m/s, Dragon_Default_MT forward

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _node: SkydotActor


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
		print("smoke_dragon_ground: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _frames(count: int) -> void:
	for i in count:
		_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100000.0)
		_session.step(STEP, _feet)
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


func _ground(at: Vector3) -> Vector3:
	var space := _camera.get_world_3d().direct_space_state
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(at + Vector3.UP * 200.0, at - Vector3.UP * 400.0,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN))
	return hit["position"] if not hit.is_empty() else at


func _flat(v: Vector3) -> Vector2:
	return Vector2(v.x, v.z)


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
	expect(_session.start_game(), "the game starts")
	_feet = SkydotWorld.skyrim_position(SPOT)
	expect(_session.enter_world("Tamriel", _feet, null), "the worldspace is entered")
	await _frames(240)
	var centre := _ground(_feet)

	# Phase 0: the dragon's clip table (found by name in dragonproject.hkx).
	var loco := world.get_locomotion("meshes/actors/dragon/dragonproject.hkx")
	var table := {}
	for slot in ["idle", "walk", "run", "turn_left", "turn_right"]:
		table[slot] = String(loco[slot]["name"])
	print("dragon: clip table ", table)
	expect(String(loco["idle"]["file"]).ends_with("ground_combatidle.hkx"), "the idle is ground_combatidle: %s" % [loco["idle"]["file"]])
	expect(String(loco["walk"]["file"]).ends_with("mtforwardground.hkx"), "the walk is mtforwardground: %s" % [loco["walk"]["file"]])
	expect(absf(float(loco["walk"]["speed"]) - 384.0) < 1.0, "it walks 384 units/s: %s" % [loco["walk"]["speed"]])
	expect(String(loco["turn_left"]["file"]).ends_with("mtturnleftground45.hkx"), "the left turn: %s" % [loco["turn_left"]["file"]])
	expect(String(loco["turn_right"]["file"]).ends_with("mtturnrightground45.hkx"), "the right turn: %s" % [loco["turn_right"]["file"]])
	var clips: Dictionary = loco["clips"]
	for name in ["clip_ground_combat_default_standing_idle", "clip_ground_combat_attack_bite", "clip_ground_combat_attack_tail",
			"clip_shout_exhale_firebreath", "clip_ground_stagger"]:
		expect(clips.has(name), "the table has " + name)

	# Phase 1: built (skeleton, meshes), standing on the ground in the combat stance. The player stands 25 m
	# to its right (it faces Skyrim's +y, which is -z here).
	var player_at := _ground(centre + Vector3(25.0, 0.0, 0.0))
	_feet = player_at
	var ref: int = _session.papyrus.spawn_actor(DRAGON, _session.place.cell_id, SkydotWorld.godot_to_skyrim(centre), 0.0)
	expect(ref != 0, "the dragon is spawned")
	_node = world.find_ref_node(ref) as SkydotActor
	await _frames(30)
	_node = world.find_ref_node(ref) as SkydotActor
	if _node == null:
		expect(false, "the dragon is built")
		quit(1)
		return
	var skeletons := _node.find_children("*", "Skeleton3D", true, false)
	expect(skeletons.size() == 1 and (skeletons[0] as Skeleton3D).get_bone_count() > 60, "one skeleton with its bones")
	expect(_node.find_children("*", "MeshInstance3D", true, false).size() >= 4, "its meshes")
	expect(_node.height > 3.0 and _node.radius > 1.0, "a big body: %.2f m high, radius %.2f" % [_node.height, _node.radius])
	var landed := await _until(func() -> bool: return _node.get_state() != "held", 5.0)
	expect(landed >= 0.0, "it finds the ground (not held)")
	expect(absf(_node.get_turn_rate() - TURN_RATE) < 0.001, "it turns at the movement type's rate: %.3f" % _node.get_turn_rate())
	await _frames(30)
	expect(_node.get_playing() in ["idle", "stance_idle", "turn_left", "turn_right"], "it stands in its idle or turns: '%s'" % _node.get_playing())

	# Phase 2: combat. The combat stance's idle and forward clips are the ground ones.
	_session.ai.start_combat(ref, PLAYER)
	await _frames(20)
	expect(_session.ai.get_combat_state(ref) == 1, "it fights the player: state %d" % _session.ai.get_combat_state(ref))
	expect(_node.get_stance() == "combat", "in the combat stance: '%s'" % _node.get_stance())
	print("dragon: playing ", _node.get_playing(), " stance ", _node.get_stance())

	# Phase 3: it turns toward the player standing beside it at about 0.589 rad/s, on the spot, in a turn clip.
	var start_yaw := _node.rotation.y
	var start_pos := _node.global_position
	var turn_clips := {}
	var yaw_before := start_yaw
	var turned := 0.0
	var time := 0.0
	var yaw_t0 := -1.0
	var yaw_0 := 0.0
	while time < 1.4:
		await _frames(1)
		time += STEP
		turn_clips[_node.get_playing()] = true
		if time >= 0.4 and yaw_t0 < 0.0:
			yaw_t0 = time
			yaw_0 = _node.rotation.y
	var rate := absf(wrapf(_node.rotation.y - yaw_0, -PI, PI)) / (time - yaw_t0)
	print("dragon: turn rate %.3f rad/s (%.1f deg turned in %.2f s), clips %s" % [rate, rad_to_deg(absf(wrapf(_node.rotation.y - start_yaw, -PI, PI))), time, turn_clips.keys()])
	expect(absf(rate - TURN_RATE) < TURN_RATE * 0.3, "it turns at about %.3f rad/s: %.3f" % [TURN_RATE, rate])
	expect(_flat(_node.global_position).distance_to(_flat(start_pos)) < 1.0, "turning on the spot: %.2f m" % _flat(_node.global_position).distance_to(_flat(start_pos)))
	expect(turn_clips.has("turn_right") or turn_clips.has("turn_left"), "with a turn clip: %s" % [turn_clips.keys()])
	expect(_node.rotation.y < start_yaw, "to the right (the player is at its right): %.2f -> %.2f" % [start_yaw, _node.rotation.y])

	# Phase 4: it faces him and walks at the movement type's forward speed, the walk clip playing, then stops at reach.
	var faced := await _until(func() -> bool:
		return absf(wrapf(_node.rotation.y - atan2(-(_feet.x - _node.global_position.x), -(_feet.z - _node.global_position.z)), -PI, PI)) < 0.2, 10.0)
	expect(faced >= 0.0, "it faces the player")
	var walked := {}
	var here := _node.global_position
	var t0 := 0.0
	var p0 := Vector3.ZERO
	var t_now := 0.0
	var speeds: Array[float] = []
	var last := _node.global_position
	while _flat(_node.global_position).distance_to(_flat(_feet)) > 12.0 and t_now < 20.0:
		await _frames(6)
		t_now += STEP * 6.0
		walked[_node.get_playing()] = true
		speeds.append(_flat(_node.global_position).distance_to(_flat(last)) / (STEP * 6.0))
		last = _node.global_position
	# The median of the moments it moves (it stops to shout or turn on the way).
	var moving := speeds.filter(func(s: float) -> bool: return s > 0.5)
	moving.sort()
	var mean: float = moving[moving.size() / 2] if moving.size() > 0 else 0.0
	print("dragon: speeds ", speeds.map(func(s: float) -> float: return snappedf(s, 0.1)))
	print("dragon: walked %.1f m in %.1f s, speed %.2f m/s (want %.2f), clips %s" % [_flat(here).distance_to(_flat(_node.global_position)), t_now, mean, WALK_SPEED, walked.keys()])
	expect(t_now < 20.0, "it reaches the player")
	expect(absf(mean - WALK_SPEED) < WALK_SPEED * 0.3, "it walks about %.2f m/s: %.2f" % [WALK_SPEED, mean])
	expect(walked.has("stance_walk") or walked.has("walk"), "with its walk clip: %s" % [walked.keys()])
	await _frames(120)
	var gap := _flat(_node.global_position).distance_to(_flat(_feet))
	print("dragon: stands %.1f m from the player, playing %s" % [gap, _node.get_playing()])
	expect(gap < 12.0 and gap > 1.0, "it stops within reach: %.1f m" % gap)
	expect(_session.ai.get_combat_state(ref) == 1, "still in combat")
	# The bite's annotation "Func.DragonBiteEffect" reaches scripts as the graph's "DragonBiteEffect" too.
	var events: Array[String] = []
	_node.animation_event.connect(func(event: String) -> void: events.append(event))
	await _until(func() -> bool: return not _node.is_acting(), 5.0)
	var length := _node.play_action(clips["clip_ground_combat_attack_bite"]["file"], 1.0)
	expect(length > 0.5, "the bite clip plays: %.2f s" % length)
	await _frames(int((length + 0.3) / STEP))
	print("dragon: bite events ", events)
	expect(events.has("Func.DragonBiteEffect") and events.has("DragonBiteEffect"), "the bite raises DragonBiteEffect: %s" % [events])
	print("smoke_dragon_ground: failures=%d" % failures)
	quit(failures)
