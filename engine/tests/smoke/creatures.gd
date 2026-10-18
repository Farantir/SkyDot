# SPDX-License-Identifier: GPL-3.0-or-later
#
# Creatures on the real SE pack (SKYDOT_SSE_PACK), headless (GAMEPLAY-PLAN.md,
# 6j): in BleakFallsBarrow01 a live skeever (aggression 1, an aggro radius of 1000
# units) enters combat with the player, runs at him, attacks with a clip of its
# project (`Attack*`) and bites for the race's unarmed damage, and dies to
# `kill_actor` with a generic ragdoll. An ambush draugr (aggression 0, no aggro radius)
# stays dormant until its script wakes it (`OnActivate` from the player sets Aggression 2,
# as masterAmbushScript does), then fights with its weapon and dies. The frostbite spider
# moves and fights too. The player cannot die here. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const SKEEVER := 0x3B9F7
const DRAUGR := 0x380B5
const SPIDER := 0x3A1E1
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _hits: Array[Dictionary] = []
var _clips: Array[Array] = []
var _states: Array[Array] = []


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
	config.wander = false
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

	# The ambush draugr stays where it is, whatever happens in sight, until woken.
	var draugr := _session.world.find_ref_node(DRAUGR) as Node3D
	expect(draugr != null, "the draugr is built")
	if draugr != null:
		await _stand_before(draugr, 12.0)
		await _frames(180)
		expect(_session.ai.get_combat_state(DRAUGR) == 0 and papyrus.get_actor_value(DRAUGR, "Aggression") == 0,
			"the ambush draugr is dormant while the player is far")
		# The player walks up to it: the sleeper hears him (OnGetUp) and masterAmbushScript sets Aggression 2.
		await _stand_before(draugr, 3.0)
		var woke := await _until(func() -> bool: return papyrus.get_actor_value(DRAUGR, "Aggression") == 2, 5.0)
		expect(woke >= 0.0, "the script woke it on approach: Aggression %s" % papyrus.get_actor_value(DRAUGR, "Aggression"))
		await _fight(DRAUGR, "draugr", "")

	# The frostbite spider (dropped from the ceiling, found the floor).
	var spider := _session.world.find_ref_node(SPIDER) as Node3D
	expect(spider != null, "the spider is built")
	if spider != null:
		_session.ai.start_combat(SPIDER, PLAYER)
		await _fight(SPIDER, "spider", "combat")
	print("smoke_creatures: failures=%d" % failures)
	quit(failures)
