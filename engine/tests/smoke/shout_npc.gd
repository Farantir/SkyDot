# SPDX-License-Identifier: GPL-3.0-or-later
#
# NPC shouts on the real SE pack (SKYDOT_SSE_PACK), headless, 7b-4: a Draugr Ambush Boss
# (LvlDraugrAmbushBossMale, spawned in front of the player in Bleak Falls Barrow; its NPC_ chain
# carries the shout UnrelentingForceShoutDraugr 0x9DE93 and a combat style with a shout mult) fights
# the player. It must face him for fAIShoutMinAimSeconds before it shouts ("combat: 0x.. shouts 0x.."),
# inhales (the draugr's shoutinhale clip plays), says the words (shoutexhale, its voice line), the shout
# lands on the player (UF: hostile spell landed, the push shoves him) and a second shout does not come
# before the voice has recovered. The player cannot die here. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const DRAUGR_SHOUT := 0x9DE93
const ARCHETYPE_STAGGER := 33
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _home := Vector3.ZERO
var _right := Vector3.RIGHT
var _clock := 0.0
var _shouts: Array[Dictionary] = []  # {ref, shout, phase, time, acting}
var _landed: Array[Dictionary] = []
var _pushes: Array[Dictionary] = []
var _effects: Array[Dictionary] = []
var _said: Array[Dictionary] = []
var _min_health := 1e9
var _fight_since := -1.0


class FakeBody:
	extends RefCounted
	var pushes: Array[Vector3] = []

	func impulse(velocity: Vector3) -> void:
		pushes.append(velocity)


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false
	var loaded_position := Vector3.ZERO
	var loaded_yaw := 0.0
	var loaded_pitch := 0.0
	var player := FakeBody.new()

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
		print("smoke_shout_npc: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


## One frame: the player stands where he is, looks at the draugr and cannot die here.
func _frames(count: int) -> void:
	var papyrus := _session.papyrus
	for i in count:
		_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT
		var health := papyrus.get_actor_value(PLAYER, "Health")
		_min_health = minf(_min_health, health)
		if health < 40.0:
			papyrus.restore_actor_value(PLAYER, "Health", 100.0)
		_session.step(STEP, _feet)
		_clock += STEP
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


func _spawn(editor_id: String) -> int:
	var world := _session.world
	var npc := world.find_npc(editor_id)
	expect(npc != 0, editor_id + " is in the pack")
	var here := SkydotWorld.godot_to_skyrim(_home)
	var to := SkydotWorld.godot_to_skyrim(_feet) - here
	var ref := _session.papyrus.spawn_actor(npc, world.get_ref_cell(BANDIT), here, atan2(to.x, to.y))
	expect(ref != 0, editor_id + " is spawned")
	return ref


func _pick_feet(home: Vector3, forward: Vector3, distance: float) -> Vector3:
	var space := _camera.get_world_3d().direct_space_state
	var from := home + Vector3.UP * 1.6
	for scale in [1.0, 0.8, 1.2, 0.6]:
		for step in 16:
			var dir := forward.rotated(Vector3.UP, step * TAU / 16.0)
			var feet: Vector3 = home + dir * distance * scale
			var free := space.intersect_ray(PhysicsRayQueryParameters3D.create(from, feet + Vector3.UP * 1.2, 1 | 2 | 8)).is_empty()
			var ground := not space.intersect_ray(PhysicsRayQueryParameters3D.create(feet + Vector3.UP * 0.5, feet - Vector3.UP * 0.5, 1 | 2 | 8)).is_empty()
			if free and ground:
				return feet
	expect(false, "no clear place to stand")
	return home + forward * distance


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.quests = false
	config.preload_doors = false
	config.wander = false
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	var fake := FakePlayer.new()
	root.add_child(_session)
	expect(_session.open(config, _camera, fake), "the session opens")
	if _session.world == null or _session.ai == null:
		expect(false, "the session has an AI")
		quit(1)
		return
	var papyrus := _session.papyrus
	var ai := _session.ai
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	ai.combat_shout.connect(func(ref: int, target: int, shout: int, phase: String) -> void:
		var node := _session.world.find_ref_node(ref) as SkydotActor
		_shouts.append({"ref": ref, "target": target, "shout": shout, "phase": phase, "time": _clock,
			"acting": node != null and node.is_acting(), "recovery": papyrus.voice_recovery(ref)}))
	_session.npc_shouts.shouted.connect(func(ref: int, shout: int, words: int, spell: int) -> void:
		_said.append({"ref": ref, "shout": shout, "words": words, "spell": spell, "time": _clock,
			"speaking": _session.speech.is_speaking(ref), "recovery": papyrus.voice_recovery(ref)}))
	_session.magic.npc_spell_landed.connect(func(caster: int, spell: int, target: int, _point: Vector3, hostile: bool) -> void:
		_landed.append({"caster": caster, "spell": spell, "target": target, "hostile": hostile, "time": _clock}))
	_session.magic_effect_changed.connect(func(target: int, mgef: int, archetype: int, started: bool, _instance: int) -> void:
		_effects.append({"target": target, "mgef": mgef, "archetype": archetype, "started": started}))
	_session.player_pushed.connect(func(source: int, velocity: Vector3) -> void:
		_pushes.append({"source": source, "velocity": velocity, "time": _clock}))
	var bandit := _session.world.find_ref_node(BANDIT) as SkydotActor
	expect(bandit != null, "the old bandit's place is built")
	if bandit == null:
		quit(1)
		return
	ai.attach_built(bandit.get_parent())
	await _frames(30)
	_home = bandit.global_position
	var forward := -bandit.global_transform.basis.z
	forward.y = 0.0
	forward = forward.normalized()
	_right = forward.cross(Vector3.UP).normalized()
	_feet = _pick_feet(_home, forward, 6.0)
	for other in _session.world.get_cell_actors(_session.world.get_ref_cell(BANDIT)):
		papyrus.damage_actor_value(other, "Health", 100000.0)
	await _frames(10)

	var draugr := _spawn("LvlDraugrAmbushBossMale")
	await _frames(5)
	var shouts := papyrus.shouts_of(draugr)
	print("shouts of the draugr: ", shouts)
	expect(shouts.has(DRAUGR_SHOUT), "the draugr's NPC_ chain gives it UnrelentingForceShoutDraugr: %s" % [shouts])
	var style := _session.world.get_combat_style(int(_session.world.get_npc(papyrus.get_ref_base(draugr)).get("combat_style", 0)))
	print("its combat style shout mult: ", style.get("shout_score_mult"))
	expect(float(style.get("shout_score_mult", 0.0)) > 0.0, "its combat style has a shout mult")
	var project := _session.combat.behaviour_of(draugr)
	expect(_session.pack.has(project.get_base_dir() + "/animations/shoutinhale.hkx")
		and _session.pack.has(project.get_base_dir() + "/animations/shoutexhale.hkx"), "its project has the shout clips")
	expect(papyrus.start_quest(0x13E06), "the quest of the shout lines runs")
	var node := _session.world.find_ref_node(draugr) as SkydotActor
	node.set_look_target(_feet + Vector3.UP)
	_fight_since = _clock
	ai.start_combat(draugr, PLAYER)
	var seen := await _until(func() -> bool: return ai.get_combat_state(draugr) == 1, 10.0)
	expect(seen >= 0.0 and ai.get_combat_target(draugr) == PLAYER, "the draugr sees the player and enters combat")

	# ---- the first shout, then the voice recovers, then the second
	var first := await _until(func() -> bool: return _said.size() >= 1, 30.0)
	expect(first >= 0.0, "the draugr shouts: %s" % [_shouts])
	await _frames(30)
	if _shouts.size() >= 2:
		var start: Dictionary = _shouts[0]
		var release: Dictionary = _shouts[1]
		expect(start["phase"] == "start" and release["phase"] == "release" and start["shout"] == DRAUGR_SHOUT,
			"it inhales and says Unrelenting Force: %s" % [_shouts])
		print("it shouts %.2f s into the fight; the words come %.2f s after the inhale" % [start["time"] - _fight_since, release["time"] - start["time"]])
		expect(start["time"] - _fight_since >= 2.0, "after facing the player for at least fAIShoutMinAimSeconds (2): %.2f s" % [start["time"] - _fight_since])
		expect(bool(start["acting"]), "the inhale clip plays (shoutinhale)")
	var said: Dictionary = _said[0] if not _said.is_empty() else {}
	expect(not said.is_empty() and int(said["words"]) == 3 and bool(said["speaking"]), "it says all three words and its voice line plays: %s" % [said])
	expect(not _landed.is_empty() and _landed.any(func(l: Dictionary) -> bool: return l["hostile"] and l["target"] == PLAYER and l["caster"] == draugr),
		"the shout lands on the player: %s" % [_landed])
	expect(_effects.any(func(e: Dictionary) -> bool: return e["target"] == PLAYER and e["archetype"] == ARCHETYPE_STAGGER and e["started"]),
		"and staggers him: %s" % [_effects])
	# The draugr's own shout only staggers; a push (PushActorAway of a stronger shout) shoves the player's body.
	papyrus.push_actor.emit(draugr, PLAYER, 20.0)
	expect(_pushes.size() == 1 and _pushes[0]["source"] == draugr and (_pushes[0]["velocity"] as Vector3).length() > 1.0,
		"a push from the draugr shoves him: %s" % [_pushes])
	expect(fake.player.pushes.size() == 1 and (fake.player.pushes[0] as Vector3).y > 0.0, "the body of the player got the shove")
	expect(_min_health < 100.0, "his Health dropped: %f" % _min_health)

	if not _said.is_empty():
		var recovery := float(said["recovery"])
		print("the voice recovers for %.1f s" % recovery)
		expect(recovery > 1.0, "the voice recovers for seconds after the shout: %f" % recovery)
		# Until the voice has recovered no second shout may start.
		await _until(func() -> bool: return _said.size() >= 2, recovery + 40.0)
		expect(_said.size() >= 2, "it shouts again once the voice has recovered")
		if _said.size() >= 2:
			var gap: float = _said[1]["time"] - _said[0]["time"]
			print("the second shout is %.1f s after the first" % gap)
			expect(gap >= recovery - 0.1, "and not before: %.1f s of %.1f" % [gap, recovery])
	print("smoke_shout_npc: ", "OK" if failures == 0 else "%d failure(s)" % failures)
	quit(1 if failures > 0 else 0)
