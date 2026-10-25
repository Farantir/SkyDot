# SPDX-License-Identifier: GPL-3.0-or-later
#
# A dragon dies and the player absorbs its soul (step 7b-7) on the real SE pack (SKYDOT_SSE_PACK),
# headless, in phases: EncDragon01Fire 0x1CA03 on the plain between Whiterun and the Western Watchtower,
# Skyrim (13100, -13650), killed by the player 25 m away. The game's own scripts run: DragonActorScript
# (onDeath, deadAndWaiting, deadDisintegrated) and MQKillDragonScript.deathSequence (the absorb effects,
# SetGhost, DragonSouls +1 with the message, MQKillDragonFaction, the skin items). Later phases add the
# air death, the skeleton, the loot and the save. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const DRAGON := 0x1CA03
const STEP := 1.0 / 60.0
const SPOT := Vector3(13100.0, -13650.0, -3000.0)  # Skyrim units; the ground is below

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _ref := 0
var _node: SkydotActor
var _effects: Array[Array] = []  # [effect, ref, target, seconds, playing]
var _animations: Array[Array] = []  # [ref, name]
var _messages: Array[String] = []
var _shaders := 0


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
		print("smoke_dragon_soul: SKIP (SKYDOT_SSE_PACK unset)")
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


func _state() -> String:
	for script in _session.papyrus.get_scripts(_ref):
		if String(script["name"]).to_lower() == "dragonactorscript":
			return script["state"]
	return "(no DragonActorScript)"


func _souls() -> float:
	return _session.papyrus.get_actor_value(PLAYER, "DragonSouls")


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
	expect(_session.start_game(), "the game starts")
	_session.papyrus.visual_effect.connect(func(effect: int, ref: int, target: int, seconds: float, playing: bool) -> void:
		_effects.append([effect, ref, target, seconds, playing]))
	_session.papyrus.sub_graph_animation.connect(func(ref: int, name: String) -> void: _animations.append([ref, name]))
	_session.papyrus.message.connect(func(text: String, _box: bool) -> void: _messages.append(text))
	_session.papyrus.effect_shader.connect(func(_shader: int, _ref: int, _playing: bool) -> void: _shaders += 1)
	_feet = SkydotWorld.skyrim_position(SPOT)
	expect(_session.enter_world("Tamriel", _feet, null), "the worldspace is entered")
	await _frames(240)

	await _spawn_dragon()
	if _node == null:
		quit(1)
		return
	await _ground_death()
	print("smoke_dragon_soul: failures=%d" % failures)
	quit(failures)


## The dragon stands 25 m from the player, on the ground, unaware of him.
func _spawn_dragon() -> void:
	var centre := _ground(_feet)
	_feet = _ground(centre + Vector3(25.0, 0.0, 0.0))
	_ref = _session.papyrus.spawn_actor(DRAGON, _session.place.cell_id, SkydotWorld.godot_to_skyrim(centre), 0.0)
	expect(_ref != 0, "the dragon is spawned")
	_session.papyrus.set_allowed_to_fly(_ref, false)
	await _frames(30)
	_node = _session.world.find_ref_node(_ref) as SkydotActor
	expect(_node != null, "the dragon is built")
	expect(_session.papyrus.get_scripts(_ref).any(func(s: Dictionary) -> bool: return String(s["name"]).to_lower() == "dragonactorscript"), "it runs DragonActorScript")
	expect(_state() != "deadAndWaiting" and _state() != "deadDisintegrated", "alive, it is in no death state: '%s'" % _state())
	expect(_session.papyrus.faction_rank(_ref, 0) == -1, "faction 0 is none")


## Killed on the ground within 1000 units of the player: the whole death sequence runs to its end.
func _ground_death() -> void:
	var papyrus := _session.papyrus
	var quest := _session.world.find_quest("MQKillDragon")
	expect(quest != 0, "the quest MQKillDragon is there")
	var hider: int = papyrus.get_variable(quest, "MQKillDragonScript", "::SkinDragonHider_var")
	var underskin: int = papyrus.get_variable(quest, "MQKillDragonScript", "::DragonUnderskin_var")
	var faction: int = papyrus.get_variable(quest, "MQKillDragonScript", "::MQKillDragonFaction_var")
	expect(hider != 0 and underskin != 0 and faction != 0, "the sequence's forms are properties: %x %x %x" % [hider, underskin, faction])
	expect(_souls() == 0.0, "no dragon souls yet: %s" % _souls())
	expect(not papyrus.is_actor_ghost(_ref), "alive it is no ghost")
	expect(papyrus.faction_rank(_ref, faction) == -1, "and in no MQKillDragonFaction")
	var errors: int = papyrus.get_error_count()
	# A fist kills it: leave 1 health, then hit (the --kill path: apply_hit, OnDying, OnDeath).
	papyrus.damage_actor_value(_ref, "Health", papyrus.get_actor_value(_ref, "Health") - 1.0)
	expect(_session.kill_actor(_ref) >= 1, "the player kills it")
	expect(papyrus.is_actor_dead(_ref), "it is dead")
	await _frames(30)
	expect(_state() == "deadAndWaiting", "DragonActorScript waits for the player: '%s'" % _state())
	# At 25 m (1750 units) it is beyond deathFXrange (1000 units): it keeps waiting.
	await _frames(240)
	expect(_state() == "deadAndWaiting" and _effects.is_empty(), "out of range it waits: '%s', %d effects" % [_state(), _effects.size()])
	_feet = _ground(_node.global_position + Vector3(10.0, 0.0, 0.0))
	var done := await _until(func() -> bool: return _state() == "deadDisintegrated", 20.0)
	expect(done >= 0.0, "it reaches deadDisintegrated: '%s'" % _state())
	# deathSequence runs about 24 s of Waits.
	var ended := await _until(func() -> bool: return _souls() >= 1.0, 60.0)
	expect(ended >= 0.0, "the player absorbs the soul: DragonSouls %s" % _souls())
	print("dragon soul: absorbed after %.1f s of the sequence, %d effects, %d animations, %d shaders" % [ended, _effects.size(), _animations.size(), _shaders])
	await _frames(300)
	expect(_souls() == 1.0, "exactly one soul: %s" % _souls())
	expect(_effects.any(func(e: Array) -> bool: return e[1] == _ref and e[2] == PLAYER and e[4]), "a visual effect plays dragon -> player: %s" % [_effects])
	expect(_effects.any(func(e: Array) -> bool: return e[1] == PLAYER and e[2] == _ref and e[4]), "and one player -> dragon")
	expect(_animations.any(func(a: Array) -> bool: return a[0] == _ref and a[1] == "SkinFadeOut"), "SkinFadeOut plays on the dragon: %s" % [_animations])
	expect(papyrus.is_actor_ghost(_ref), "the dragon is a ghost")
	expect(_messages.has("Dragon Soul Absorbed"), "the player is told: %s" % [_messages])
	expect(_messages.count("Dragon Soul Absorbed") == 1, "once")
	expect(papyrus.faction_rank(_ref, faction) >= 0, "the dragon is in MQKillDragonFaction: %d" % papyrus.faction_rank(_ref, faction))
	var worn: Array = papyrus.equipment(_ref)["worn"]
	expect(worn.has(hider), "it wears SkinDragonHider: %s" % [worn])
	expect(worn.has(underskin), "and DragonUnderskin")
	expect(_node.get_meta("skydot_skin_faded", false), "its skin faded out")
	var visible_skin := 0
	for mesh in _node.find_children("*", "MeshInstance3D", true, false):
		if mesh.get_parent() is Skeleton3D and mesh.visible:
			visible_skin += 1
	expect(visible_skin == 0, "no skin mesh is left: %d" % visible_skin)
	expect(_session.visual_effects.stream_count() == 0, "no stream is left: %d" % _session.visual_effects.stream_count())
	# A ghost takes no hits.
	var result := papyrus.apply_hit(PLAYER, _ref, 0, 0, "attackStart", 0)
	expect(result.has("error"), "a hit on the dead ghost lands nowhere: %s" % [result])
	expect(papyrus.get_error_count() == errors, "the VM logged no new errors: %d" % (papyrus.get_error_count() - errors))
	# Nothing absorbs twice.
	await _frames(600)
	expect(_souls() == 1.0, "no second absorb: %s" % _souls())
