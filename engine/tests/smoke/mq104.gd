# SPDX-License-Identifier: GPL-3.0-or-later
#
# MQ104 "Dragon Rising" on the real SE pack (SKYDOT_SSE_PACK), headless: the player stands at the
# Western Watchtower, Skyrim (5300, -14600), with his Health restored every frame; the quest is set to
# stages 12, 50 and 55 as `--set-stage` does. The scene goes by itself: stages 48, 52, 60 (Mirmulnir
# is enabled high in the air), 65, the guards shoot arrows at him while he flies, he lands near the
# landing marker 0x5A6D1, the ground fight kills him (if he still lives 150 s after landing he is
# killed through Papyrus: "fallback kill", not a failure), then 80, 85, 90 (the player absorbs the
# soul, learns and unlocks Fus), and the shout: equipping Unrelenting Force sends OnObjectEquipped
# (stage 92), saying a word sends OnSpellCast (stage 100). No soldier or tower guard may die to
# anyone but the dragon. Each check prints a line with the game time. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const MQ104 := "MQ104"
const DRAGON := 0x5A6CA
const MARKER := Vector3(5692.0, -14659.0, -5190.0)  # the landing marker 0x5A6D1, Skyrim units
const SPOT := Vector3(5300.0, -14600.0, -5100.0)
const UNIT := 0.0142875  # metres per Skyrim unit
const TICKS := 30
const SPEED := 3.0
const STEP := SPEED / TICKS  # seconds of simulation per frame
const SHOUT := 0x13E07  # Unrelenting Force
const WORD_FUS := 0x13E22
const FRIENDS: Array[int] = [0xFF001005, 0xFF001006, 0xFF001007, 0xFF001008, 0xFF001009, 0xFF00100A,
	0xD8C56, 0xD8C57, 0xD8C58, 0xD9073, 0xD9074, 0xD9075]
const LIMIT := 900.0  # game seconds the whole fight may take

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _clock := 0.0
var _arrows := 0
var _arrows_air := 0
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
		print("smoke_mq104: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _surface(at: Vector3) -> float:
	var space := _camera.get_world_3d().direct_space_state
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(Vector3(at.x, at.y + 4000.0, at.z), Vector3(at.x, -2000.0, at.z),
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN))
	return hit["position"].y if not hit.is_empty() else -1.0e9


func _ground(at: Vector3) -> Vector3:
	var space := _camera.get_world_3d().direct_space_state
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(at + Vector3.UP * 200.0, at - Vector3.UP * 400.0,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN))
	return hit["position"] if not hit.is_empty() else at


func _frames(count: int) -> void:
	for i in count:
		_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100000.0)
		_session.step(STEP, _feet)
		_clock += STEP
		_track()
		_watch_landing()
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


func _stage_done(stage: int) -> bool:
	var state: Dictionary = _session.papyrus.get_quest_state(_session.world.find_quest(MQ104))
	return (state.get("stages_done", []) as Array).has(stage)


func _log(what: String) -> void:
	print("mq104: t=%6.1f %s" % [_clock, what])


func _stage(stage: int, limit: float) -> void:
	var t := await _until(func() -> bool: return _stage_done(stage), limit)
	expect(t >= 0.0, "stage %d is done within %.0f s" % [stage, limit])
	_log("stage %d %s" % [stage, "done" if t >= 0.0 else "MISSING"])


func _run(pack_dir: String) -> void:
	Engine.physics_ticks_per_second = TICKS
	Engine.time_scale = SPEED
	await process_frame
	var started := Time.get_ticks_msec()
	var config := GameConfig.new()
	config.pack = pack_dir
	config.quests = true
	config.set_stage = "MQ104:12,MQ104:50,MQ104:55"
	config.preload_doors = false
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
	expect(_session.start_game(), "the game starts")
	var killed: Array[String] = []
	_session.actor_died.connect(func(ref: int) -> void:
		if FRIENDS.has(ref):
			var killer := int(_session.papyrus.get_killer(ref))
			_log("friend 0x%X died, killer 0x%X" % [ref, killer])
			if killer != DRAGON:
				killed.append("0x%X by 0x%X" % [ref, killer]))
	_session.combat.npc_arrow.connect(func(_attacker: int, target: int) -> void:
		if target == DRAGON:
			_arrows += 1
			if _node != null and is_instance_valid(_node) and _node.is_flying():
				_arrows_air += 1)
	_feet = SkydotWorld.skyrim_position(SPOT)
	expect(_session.enter_world("Tamriel", _feet, null), "the worldspace is entered")
	await _frames(30)
	_feet = _ground(_feet)
	_log("the player stands at %s" % [_feet])

	await _stage(48, 240.0)

	# Mirmulnir is built after stage 60, high in the air.
	await _until(func() -> bool: return _session.world.find_ref_node(DRAGON) != null, 240.0)
	_node = _session.world.find_ref_node(DRAGON) as SkydotActor
	expect(_node != null, "Mirmulnir is built")
	if _node == null:
		print("smoke_mq104: failures=%d" % failures)
		quit(1)
		return
	_log("Mirmulnir built at %s, flying=%s" % [SkydotWorld.godot_to_skyrim(_node.global_position), _node.is_flying()])
	expect(_node.is_flying(), "he starts in the air")
	_log("Mirmulnir: Health %.0f (max %.0f), HealRate %.2f, HealRateMult %.0f" % [_av("Health"), _session.papyrus.get_actor_value_max(DRAGON, "Health"),
		_av("HealRate"), _av("HealRateMult")])
	expect(_av("Health") > 800.0 and absf(_av("HealRate") - 0.7) < 0.01, "level-10 dragon values: Health 905, HealRate 0.7")
	for stage in [52, 55, 60, 65]:
		await _stage(stage, 240.0)

	# He lands on the marker (the forced landing marker), at least once.
	var t := await _until(func() -> bool: return not _landings.is_empty(), LIMIT)
	expect(t >= 0.0, "he lands")
	for landing in _landings:
		expect(float(landing["distance"]) < 50.0, "he lands within 50 units of the marker: %.0f" % float(landing["distance"]))
	expect(_arrows_air >= 1, "the guards shot at him while he flew: %d" % _arrows_air)
	var landed_at: float = _landings[0]["time"] if not _landings.is_empty() else _clock

	# The ground fight kills him, or the fallback does.
	var dead := func() -> bool: return _session.papyrus.is_actor_dead(DRAGON)
	t = await _until(dead, 150.0)
	if t < 0.0:
		_session.kill_actor(DRAGON)
		_log("fallback kill")
		await _until(dead, 10.0)
	expect(dead.call(), "Mirmulnir dies")
	_dead_at = _clock
	_log("dead %.1f s after the first landing, %.1f s after the last, %d landings" % [_clock - landed_at, _clock - float(_landings[-1]["time"]), _landings.size()])
	expect(t >= 0.0 and _clock - float(_landings[-1]["time"]) < 60.0, "he dies within 60 s of his last landing: %.1f" % (_clock - float(_landings[-1]["time"])))
	_log("Mirmulnir is dead at %s, the player %.0f units away" % [SkydotWorld.godot_to_skyrim(_node.global_position),
		_node.global_position.distance_to(_feet) / UNIT])
	# "Investigate the dragon": the player walks up to the body (the soul is absorbed within deathFXrange).
	_feet = _ground(_node.global_position + Vector3(10.0, 0.0, 0.0))

	for stage in [80, 85, 90]:
		await _stage(stage, 240.0)
		_log("  = %.1f s after death, DragonSouls %s" % [_clock - _dead_at, _session.papyrus.get_actor_value(PLAYER, "DragonSouls")])
	var papyrus := _session.papyrus
	var fus: Dictionary = {}
	for row: Dictionary in papyrus.shout_info(PLAYER, SHOUT).get("words", []):
		if int(row["word"]) == WORD_FUS:
			fus = row
	expect(bool(fus.get("known", false)) and bool(fus.get("unlocked", false)), "Fus is known and unlocked: %s" % [fus])
	var souls := papyrus.get_actor_value(PLAYER, "DragonSouls")
	_log("Fus learned, DragonSouls %s" % souls)
	# (The quest spends the soul on Fus: vanilla unlocks it for free, so DragonSouls is back to 0.)

	# The shout.
	expect(_session.equip_shout(SHOUT), "Unrelenting Force is equipped")
	await _stage(92, 20.0)
	var voice := _session.voice_pressed()
	expect(voice["ok"], "the voice key goes down: %s" % [voice])
	await _until(func() -> bool:
		var state := _session.magic.shout_state()
		return state.is_empty() or bool(state["fired"]), 10.0)
	_session.voice_released()
	await _stage(100, 20.0)

	expect(killed.is_empty(), "no soldier or tower guard was killed by anyone but the dragon: %s" % [killed])
	_log("%.1f s of game time in %.1f s of wall time" % [_clock, (Time.get_ticks_msec() - started) / 1000.0])
	print("smoke_mq104: failures=%d" % failures)
	quit(failures)


var _airborne := false
var _landings: Array[Dictionary] = []
var _next_health := 0.0
var _dead_at := 0.0


## Every 5 s once Mirmulnir exists: stage 60's time, his height over the ground, distance from the marker, Health.
func _track() -> void:
	if _clock < _next_health:
		return
	var node := _session.world.find_ref_node(DRAGON) as Node3D
	if node == null:
		return
	_next_health = _clock + 5.0
	var at := SkydotWorld.godot_to_skyrim(node.global_position)
	var flying: bool = node.has_method("is_flying") and node.is_flying()
	_log("Mirmulnir %5.0f u from the marker, %5.0f u up, flying=%s, Health %.0f%s" % [Vector2(at.x - MARKER.x, at.y - MARKER.y).length(),
		at.z - MARKER.z, flying, _av("Health"), "" if _stage_done(60) else " (stage 60 not yet)"])


## Every landing (flying, then not flying) with its distance from the marker, Health and the time since the last one.
func _watch_landing() -> void:
	var node := _session.world.find_ref_node(DRAGON) as SkydotActor
	if node == null:
		return
	if node.is_flying():
		_airborne = true
	elif _airborne:
		_airborne = false
		var at := SkydotWorld.godot_to_skyrim(node.global_position)
		var distance := Vector2(at.x - MARKER.x, at.y - MARKER.y).length()
		_landings.append({"time": _clock, "distance": distance})
		_log("landing %d: %.0f units from the marker, Health %.0f (%.0f %%), %d arrows at him (%d in the air)" % [_landings.size(), distance,
			_av("Health"), 100.0 * _av("Health") / maxf(_session.papyrus.get_actor_value_max(DRAGON, "Health"), 1.0), _arrows, _arrows_air])


func _av(name: String) -> float:
	return float(_session.papyrus.get_actor_value(DRAGON, name))
