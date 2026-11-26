# SPDX-License-Identifier: GPL-3.0-or-later
#
# AI blocking on the real SE pack (SKYDOT_SSE_PACK), headless: the bandit
# 0x39FE4 of BleakFallsBarrow01, given an Iron Shield, fights the player; the
# player swings his Iron Sword at him until the bandit raises his block
# (combat_block, IsBlocking, the shield's block idle plays). A swing that meets
# the raised block is flagged blocked and does less than it would (AttackRules'
# blocked share), the others do their full damage; the block goes down after.
# Fighting, he refuses to talk (dialogue_open is refused).
# His style (csHumanMeleeLvl1, defensive 0.11) blocks about one swing in 70
# (none while he swings himself), so block_chance_scale makes it 0.5 here.
# Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const IRON_SWORD := 0x12EB7
const IRON_SHIELD := 0x12EB6
const STEP := 1.0 / 60.0
const SWINGS := 40

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _bandit: SkydotActor
var _hits: Array[Dictionary] = []
var _blocks: Array[Array] = []
var _acting_on_block := false


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_combat_block: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _frames(count: int) -> void:
	for i in count:
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100.0)  # nobody dies here
		_session.papyrus.restore_actor_value(BANDIT, "Health", 1000.0)
		_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT
		if _bandit != null:
			_camera.look_at(_bandit.global_position + Vector3.UP * 1.2, Vector3.UP)
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
	expect(_session.open(config, _camera, PlayerBody.new()), "the session opens")
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
	_session.hit_landed.connect(func(target: int, result: Dictionary, _point: Vector3) -> void:
		_hits.append({"target": target, "result": result}))
	ai.combat_block.connect(func(ref: int, up: bool) -> void:
		_blocks.append([ref, up])
		if ref == BANDIT and up:
			_acting_on_block = _bandit.is_acting() and papyrus.is_blocking(BANDIT))
	_bandit = world.find_ref_node(BANDIT) as SkydotActor
	expect(_bandit != null, "the bandit is built")
	if _bandit == null:
		quit(1)
		return
	ai.attach_built(_bandit.get_parent())
	ai.block_chance_scale = 0.5 / 0.11
	# The bandit alone with the player.
	for other in _bandit.get_parent().find_children("*", "SkydotActor", true, false):
		var ref: int = other.get_meta("skydot_ref", 0)
		if ref != 0 and ref != BANDIT and not papyrus.is_actor_dead(ref):
			_session.kill_actor(ref)
	papyrus.add_item(BANDIT, IRON_SHIELD, 1)
	papyrus.equip(BANDIT, IRON_SHIELD)
	expect(int(papyrus.equipment(BANDIT)["left"]) == IRON_SHIELD, "the bandit holds the Iron Shield: %s" % [papyrus.equipment(BANDIT)])
	var forward := -_bandit.global_transform.basis.z
	forward.y = 0.0
	_feet = _bandit.global_position + forward.normalized() * 1.2
	await _frames(30)

	_session.give([Vector2i(IRON_SWORD, 1)])
	expect(_session.equip(IRON_SWORD), "the Iron Sword is equipped")
	_session.toggle_weapon()
	var drawn := await _until(func() -> bool: return _session.weapon_state() == Combat.State.DRAWN, 5.0)
	expect(drawn >= 0.0, "the sword is drawn")
	expect(ai.start_combat(BANDIT, PLAYER), "the bandit fights the player")
	await _until(func() -> bool: return _session.combat.npc_is_drawn(BANDIT), 5.0)
	# Fighting (and hostile, a very aggressive bandit), he does not talk.
	expect(ai.refuses_talk(BANDIT) and not _session.dialogue_open(BANDIT), "the bandit in combat does not talk")

	# Swing until one swing meets the raised block and one does not.
	var blocked: Array[Dictionary] = []
	var unblocked: Array[Dictionary] = []
	var swings := 0
	while swings < SWINGS and (blocked.is_empty() or unblocked.is_empty()):
		var waited := 0.0
		while not _session.attack(false) and waited < 3.0:
			await _frames(1)
			waited += STEP
		if waited >= 3.0:
			break
		swings += 1
		var before := _hits.size()
		await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)
		for h in _hits.slice(before):
			if h["target"] == BANDIT and h["result"]["attacker"] == PLAYER:
				(blocked if h["result"]["blocked_percent"] > 0.0 else unblocked).append(h["result"])
	var ups := _blocks.filter(func(b: Array) -> bool: return b[0] == BANDIT and b[1])
	print("swings %d: hits %d blocked, %d not; block raised %d times" % [swings, blocked.size(), unblocked.size(), ups.size()])
	expect(not ups.is_empty(), "the bandit raises his block")
	expect(_acting_on_block, "his block idle plays and IsBlocking holds while it is up")
	expect(not unblocked.is_empty(), "some swings get through")
	expect(not blocked.is_empty(), "a swing meets the block")
	if not blocked.is_empty() and not unblocked.is_empty():
		var b: Dictionary = blocked[0]
		var u: Dictionary = unblocked[0]
		print("blocked: %s of %s (%.1f%%); unblocked %s" % [b["damage"], b["damage_before_block"], b["blocked_percent"], u["damage"]])
		expect(b["damage"] < b["damage_before_block"] and b["damage"] < u["damage"], "the blocked swing does less")
		expect(is_equal_approx(float(u["damage"]), float(u["damage_before_block"])), "an unblocked one does its all")
	# The block goes down again.
	await _until(func() -> bool: return not ai.is_blocking(BANDIT), 3.0)
	expect(not ai.is_blocking(BANDIT) and not papyrus.is_blocking(BANDIT), "the block is lowered")
	expect(_blocks.any(func(b: Array) -> bool: return b[0] == BANDIT and not b[1]), "combat_block says so")
	print("smoke_combat_block: failures=%d" % failures)
	quit(failures)
