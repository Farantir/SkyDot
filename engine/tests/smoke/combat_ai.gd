# SPDX-License-Identifier: GPL-3.0-or-later
#
# Combat AI on the real SE pack (SKYDOT_SSE_PACK), headless (GAMEPLAY-PLAN.md,
# 6h and 6i): in BleakFallsBarrow01 the player stands in sight of the bandit
# 0x39FE4 (very aggressive, neutral to the player): it enters combat with 0x14,
# draws (stance 1hm), approaches, and a hit line names the player; a second
# bandit, given a bow and arrows and no melee weapon, keeps its distance and
# shoots an arrow; after `kill_actor` the bandit leaves combat and attacks no
# more. The player cannot die here (his Health is restored each frame). Skips
# without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const HUNTING_BOW := 0x13985
const IRON_ARROW := 0x1397D
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _states: Array[Array] = []
var _attacks: Array[Array] = []
var _hits: Array[Dictionary] = []
var _arrows: Array[Array] = []


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
		print("smoke_combat_ai: SKIP (SKYDOT_SSE_PACK unset)")
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
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100.0)  # the player cannot die here
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
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, _camera, FakePlayer.new()), "the session opens")
	if _session.world == null or _session.ai == null:
		expect(false, "the session has an AI")
		quit(1)
		return
	var world := _session.world
	var papyrus := _session.papyrus
	var ai := _session.ai
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	ai.combat_state_changed.connect(func(ref: int, target: int, state: int) -> void: _states.append([ref, target, state]))
	ai.combat_attack.connect(func(ref: int, target: int, kind: String) -> void: _attacks.append([ref, target, kind]))
	_session.hit_landed.connect(func(target: int, result: Dictionary, _point: Vector3) -> void:
		_hits.append({"target": target, "result": result}))
	_session.combat.npc_arrow.connect(func(attacker: int, target: int) -> void: _arrows.append([attacker, target]))
	var node := world.find_ref_node(BANDIT) as SkydotActor
	expect(node != null, "the bandit is built")
	if node == null:
		quit(1)
		return
	ai.attach_built(node.get_parent())
	await _frames(30)
	var home := node.global_position
	var forward := -node.global_transform.basis.z
	forward.y = 0.0
	forward = forward.normalized()
	_feet = home + forward * 6.0
	expect(papyrus.get_actor_value(BANDIT, "Aggression") == 2, "the bandit is very aggressive")
	# The corpses at the entrance start dead (ACHR "Starts Dead"): no combat, no attacks from them.
	for corpse in [0x373A6, 0x375AA]:
		expect(papyrus.is_actor_dead(corpse), "0x%X starts dead" % corpse)
		expect(papyrus.get_actor_value(corpse, "Health") <= 0.0, "0x%X has no health" % corpse)

	# Another aggressive actor becomes the archer.
	var archer_ref := 0
	var archer: SkydotActor = null
	for candidate in node.get_parent().find_children("*", "SkydotActor", true, false):
		var ref: int = candidate.get_meta("skydot_ref", 0)
		if ref != 0 and ref != BANDIT and not papyrus.is_actor_dead(ref) and papyrus.get_actor_value(ref, "Aggression") >= 2 \
				and papyrus.melee_weapon(ref) != 0 and archer == null:
			archer = candidate
			archer_ref = ref
	expect(archer != null, "another aggressive actor is in the cell")

	# He sees the player (neutral by faction, aggression 2) and fights.
	var waited := await _until(func() -> bool: return ai.get_combat_state(BANDIT) == 1, 10.0)
	print("combat began after ", waited, " s: ", _states)
	expect(ai.get_combat_state(BANDIT) == 1 and ai.get_combat_target(BANDIT) == PLAYER, "the bandit enters combat with the player")
	expect(_states.has([BANDIT, PLAYER, 1]), "combat_state_changed says so: %s" % [_states])
	expect(papyrus.is_actor_dead(BANDIT) == false, "he lives")
	await _frames(5)
	expect(node.get_stance() == "1hm", "he draws: stance %s" % node.get_stance())
	expect(_session.combat.npc_is_drawn(BANDIT), "his weapon is drawn")

	# He comes and hits.
	var before := papyrus.get_actor_value(PLAYER, "Health")
	var hit_time := await _until(func() -> bool: return _hits.any(func(h: Dictionary) -> bool: return h["target"] == PLAYER and h["result"]["attacker"] == BANDIT), 25.0)
	print("first hit on the player after ", hit_time, " s; attacks ", _attacks.size(), " hits ", _hits.size())
	expect(hit_time >= 0.0, "a hit line names the player")
	expect(_attacks.any(func(a: Array) -> bool: return a[0] == BANDIT and a[1] == PLAYER and a[2] in ["light", "power"]), "combat_attack asked for a swing")
	if hit_time >= 0.0:
		var hit: Dictionary = _hits.filter(func(h: Dictionary) -> bool: return h["result"]["attacker"] == BANDIT)[0]
		expect(hit["result"]["weapon"] != 0, "with a weapon")
		expect(before > 0.0, "(the player was alive)")

	if archer != null:
		papyrus.add_item(archer_ref, HUNTING_BOW, 1)
		papyrus.add_item(archer_ref, IRON_ARROW, 30)
		while papyrus.melee_weapon(archer_ref) != 0:
			var form := papyrus.melee_weapon(archer_ref)
			expect(papyrus.transfer(archer_ref, PLAYER, form, 1) > 0, "his melee weapon goes")
		expect(not papyrus.ranged_weapon(archer_ref).is_empty(), "he has a bow now")
		_session.kill_actor(BANDIT)
		await _frames(10)
		archer.teleport(home)
		archer.face(atan2(-forward.x, -forward.z))
		var arrows_before := _arrows.size()
		var shot := await _until(func() -> bool: return _arrows.size() > arrows_before, 30.0)
		print("archer 0x%X: first arrow after %s s; states %s" % [archer_ref, shot, _states])
		expect(shot >= 0.0, "an arrow leaves the archer's bow")
		expect(_attacks.any(func(a: Array) -> bool: return a[0] == archer_ref and a[2] == "shoot"), "combat_attack asked for a shot")
		expect(archer.get_stance() == "bow", "stance bow: %s" % archer.get_stance())
		# An arrow finds the player: bow plus arrow damage (Hunting Bow 7 + Iron Arrow 8).
		var landed := await _until(func() -> bool: return _hits.any(func(h: Dictionary) -> bool: return h["result"]["attacker"] == archer_ref and h["result"]["ammo"] == IRON_ARROW), 30.0)
		expect(landed >= 0.0, "an arrow hits the player")
		for h in _hits:
			if h["result"]["attacker"] == archer_ref and h["result"]["ammo"] == IRON_ARROW:
				expect(absf(h["result"]["base"] - 15.0) < 0.01, "an arrow does bow + arrow: %s" % h["result"]["base"])
				break

	# After his death the first bandit has left combat and attacks no more.
	expect(papyrus.is_actor_dead(BANDIT), "the bandit is dead (kill_actor)")
	expect(ai.get_combat_state(BANDIT) == 0, "he left combat")
	var count := _attacks.filter(func(a: Array) -> bool: return a[0] == BANDIT).size()
	await _frames(180)
	expect(_attacks.filter(func(a: Array) -> bool: return a[0] == BANDIT).size() == count, "the dead attack no more")
	for corpse in [0x373A6, 0x375AA]:
		expect(ai.get_combat_state(corpse) == 0 and not _states.any(func(e: Array) -> bool: return e[0] == corpse) \
			and not _attacks.any(func(e: Array) -> bool: return e[0] == corpse), "0x%X never fought" % corpse)
	print("smoke_combat_ai: failures=%d" % failures)
	quit(failures)
