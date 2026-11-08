# SPDX-License-Identifier: GPL-3.0-or-later
#
# Creatures on the real SE pack (SKYDOT_SSE_PACK), headless (GAMEPLAY-PLAN.md,
# 6j): in BleakFallsBarrow01 a live skeever (aggression 1, an aggro radius of 1000
# units) enters combat with the player, runs at him, attacks with a clip of its
# project (`Attack*`) and bites for the race's unarmed damage, and dies to
# `kill_actor` with a generic ragdoll. An ambush draugr (aggression 0, no aggro radius) sleeps in
# its alcove (the furniture use of the AI: GetSleeping 3) and stays there with the player close by;
# when combat starts it gets up (GetSleeping 4, the exit clip of its graph), its scripts hear
# OnGetUp (masterAmbushScript sets Aggression 2) and it fights with its weapon and dies. The
# frostbite spider hangs in its shaft until the player enters the trigger, drops out of it (the
# exit and fall clips of its graph), lands, then moves and fights. The player cannot die here.
# Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const SKEEVER := 0x3B9F7
const DRAUGR := 0x380B5
const SPIDER := 0x3A1E1
const SPIDER_TRIGGER := 0xAA83E
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _hits: Array[Dictionary] = []
var _clips: Array[Array] = []
var _states: Array[Array] = []


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_creatures: SKIP (SKYDOT_SSE_PACK unset)")
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


## Put the player `distance` metres before `node`, along its facing, and let the session learn it.
func _stand_before(node: Node3D, distance: float) -> void:
	var forward := -node.global_transform.basis.z
	forward.y = 0.0
	_feet = node.global_position + forward.normalized() * distance
	await _frames(2)


## Fight one creature: it enters combat, moves, attacks (a clip of its project), hits the player
## and dies with a ragdoll. Returns the number of bodies of its ragdoll.
func _fight(ref: int, label: String, expect_stance: String) -> void:
	var node := _session.world.find_ref_node(ref) as SkydotActor
	expect(node != null, "%s is built" % label)
	if node == null:
		return
	var home := node.global_position
	await _stand_before(node, 3.0)
	_hits.clear()
	_clips.clear()
	var waited := await _until(func() -> bool: return _session.ai.get_combat_state(ref) == 1, 10.0)
	expect(waited >= 0.0 and _session.ai.get_combat_target(ref) == PLAYER, "%s enters combat with the player" % label)
	await _frames(5)
	if expect_stance != "":
		expect(node.get_stance() == expect_stance, "%s stands in %s: '%s'" % [label, expect_stance, node.get_stance()])
	var hit_time := await _until(func() -> bool: return _hits.any(func(h: Dictionary) -> bool: return h["result"]["attacker"] == ref and h["target"] == PLAYER), 25.0)
	print("%s: first hit on the player after %s s, clips %s" % [label, hit_time, _clips])
	expect(hit_time >= 0.0, "%s hits the player" % label)
	expect(node.global_position.distance_to(home) > 0.5, "%s moved: %.2f m" % [label, node.global_position.distance_to(home)])
	expect(_clips.any(func(c: Array) -> bool: return c[0] == ref and String(c[1]).to_lower().contains("attack")), "%s plays an attack clip: %s" % [label, _clips])
	expect(_states.any(func(s: Array) -> bool: return s[0] == ref and s[2] == 1), "%s reported combat" % label)
	var hits := _session.kill_actor(ref)
	expect(_session.papyrus.is_actor_dead(ref), "%s is killed (%d hits)" % [label, hits])
	await _frames(10)
	expect(_session.ai.get_combat_state(ref) == 0, "%s leaves combat" % label)
	var ragdoll := node.find_child("Ragdoll", true, false)
	expect(ragdoll != null and ragdoll.get_child_count() >= 8, "%s lies down in a ragdoll: %d bodies" % [label, ragdoll.get_child_count() if ragdoll != null else 0])


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.quests = false
	config.preload_doors = false
	config.wander = true  # the AI drives the actors (the sleepers go into their furniture)
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, _camera, PlayerBody.new()), "the session opens")
	if _session.world == null or _session.ai == null:
		quit(1)
		return
	var papyrus := _session.papyrus
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	_session.hit_landed.connect(func(target: int, result: Dictionary, _point: Vector3) -> void:
		_hits.append({"target": target, "result": result}))
	_session.combat.npc_attack_started.connect(func(attacker: int, clip: String, _power: bool) -> void: _clips.append([attacker, clip]))
	_session.ai.combat_state_changed.connect(func(ref: int, target: int, state: int) -> void: _states.append([ref, target, state]))
	var skeever := _session.world.find_ref_node(SKEEVER) as Node3D
	expect(skeever != null, "the skeever is built")
	if skeever == null:
		quit(1)
		return
	_session.ai.attach_built(skeever.get_parent())
	await _frames(30)

	# Reactions: neutral to the player on both sides (the fights come from aggro radii and aggression).
	expect(_session.ai.get_reaction(SKEEVER, PLAYER) == 1 and _session.ai.get_reaction(PLAYER, SKEEVER) == 1, "a skeever is neutral to the player")
	expect(papyrus.get_actor_value(SKEEVER, "Aggression") == 1, "the skeever is aggressive (1)")
	expect(papyrus.get_actor_value(DRAUGR, "Aggression") == 0, "the ambush draugr starts unaggressive")

	await _fight(SKEEVER, "skeever", "combat")

	# The ambush draugr sleeps in its alcove, whatever happens in sight, until combat starts.
	var draugr := _session.world.find_ref_node(DRAUGR) as Node3D
	expect(draugr != null, "the draugr is built")
	if draugr != null:
		var bed_at := draugr.global_position
		await _stand_before(draugr, 12.0)
		await _frames(180)
		var lying: Dictionary = _session.ai.get_furniture_state(DRAUGR)
		expect(lying.get("state", 0) == 3 and lying.get("begun", false), "the draugr lies in its alcove (GetSleeping 3): %s" % lying)
		expect(_session.ai.get_combat_state(DRAUGR) == 0 and papyrus.get_actor_value(DRAUGR, "Aggression") == 0,
			"the ambush draugr is dormant while the player is far")
		# 250 units (3.6 m) from it for eight seconds: it is in the trigger 0x38098, whose script sets Aggression 2,
		# and still asleep (as in the game): nothing wakes a sleeper but combat.
		await _stand_before(draugr, 250.0 * SkydotWorld.unit_scale())
		await _frames(480)
		expect(_session.ai.get_furniture_state(DRAUGR).get("state", 0) == 3 and _session.ai.get_combat_state(DRAUGR) == 0,
			"the draugr sleeps on with the player close by: %s combat %s" % [_session.ai.get_furniture_state(DRAUGR), _session.ai.get_combat_state(DRAUGR)])
		expect(draugr.global_position.distance_to(bed_at) < 0.01, "it did not move")
		# Combat starts: it gets up (GetSleeping 4), its scripts hear OnGetUp (Aggression 2), and it is out after the clip.
		_session.ai.start_combat(DRAUGR, PLAYER)
		await _frames(30)
		expect(_session.ai.get_furniture_state(DRAUGR).get("state", 0) == 4, "combat makes it get up: GetSleeping 4: %s" % _session.ai.get_furniture_state(DRAUGR))
		var up := await _until(func() -> bool: return _session.ai.get_furniture_state(DRAUGR).is_empty(), 10.0)
		expect(up >= 0.0, "the draugr is out of the alcove after its exit clip (%.1f s)" % up)
		expect(draugr.global_position.distance_to(bed_at) > 0.3, "it moved out: %.2f m" % draugr.global_position.distance_to(bed_at))
		await _fight(DRAUGR, "draugr", "")

	# The frostbite spider hangs in its shaft until the player enters trigger 0xAA83E, then drops.
	var spider := _session.world.find_ref_node(SPIDER) as SkydotActor
	expect(spider != null, "the spider is built")
	if spider != null:
		var shaft := spider.global_position
		var hanging: Dictionary = _session.ai.get_furniture_state(SPIDER)
		expect(hanging.get("state", 0) == 3, "the spider sleeps in its furniture (GetSleeping 3): %s" % hanging)
		_feet = SkydotWorld.skyrim_position(_session.world.get_ref_info(_session.world.get_ref_cell(SPIDER_TRIGGER), SPIDER_TRIGGER)["position"])
		var dropped := await _until(func() -> bool: return _session.ai.get_furniture_state(SPIDER).get("state", 0) == 4, 5.0)
		expect(dropped >= 0.0, "the trigger makes it leave the furniture (GetSleeping 4)")
		var landed := await _until(func() -> bool: return spider.is_on_floor() and _session.ai.get_furniture_state(SPIDER).is_empty(), 15.0)
		expect(landed >= 0.0, "it drops out of the shaft and lands")
		expect(shaft.y - spider.global_position.y > 10.0, "it came down: %.1f m" % (shaft.y - spider.global_position.y))
		await _fight(SPIDER, "spider", "combat")
	print("smoke_creatures: failures=%d" % failures)
	quit(failures)
