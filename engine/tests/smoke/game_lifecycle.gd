# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game layer's lifecycle on the real SE pack (SKYDOT_SSE_PACK), headless (docs/adr/0009,
# ARCHITECTURE.md 8.2): mid-fight in BleakFallsBarrow01 a save is loaded and nothing of the fight
# before it goes on. Staging: the player stands 6 m in front of the bandit 0x39FE4; a second
# aggressive actor gets a bow and arrows (as combat_ai.gd does) and shoots; the bandit is
# paralysed and lies as a ragdoll (as magic_archetypes.gd does). The save is taken before all
# that. At the moment of the load: an arrow stuck or in flight (Projectiles._live), an arrow
# still to leave the bow (Combat._shots), a paralysed bandit (Corpses._paralysed, is_down).
# After it: no projectile node of before is alive, Combat and Corpses hold nothing of before,
# the bandit is not paralysed or down, and for 5 s no hit with an arrow and no arrow from the
# archer lands (after the load he has no bow: the save is from before he got it).
#
# Not staged: a melee attack under way (Combat._npc_attacks): the only melee attacker in the
# cell is the paralysed bandit, so the field is checked empty after the load but was not
# filled before it. A mage's cast, a shouter's breath and a visual effect stream are not staged
# either (gd/test_lifecycle.gd covers them). The place-left path (a door) is not run here.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const HUNTING_BOW := 0x13985
const IRON_ARROW := 0x1397D
const PARALYZE := 0x5AD5F
const SAVE := "user://game_lifecycle_smoke.skydot"
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _hits: Array[Dictionary] = []
var _arrows: Array[Array] = []


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_game_lifecycle: SKIP (SKYDOT_SSE_PACK unset)")
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
	expect(_session.open(config, _camera, PlayerBody.new()), "the session opens")
	if _session.world == null or _session.ai == null:
		expect(false, "the session has an AI")
		quit(1)
		return
	var papyrus := _session.papyrus
	var ai := _session.ai
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	_session.hit_landed.connect(func(target: int, result: Dictionary, _point: Vector3) -> void:
		_hits.append({"target": target, "result": result}))
	_session.combat.npc_arrow.connect(func(attacker: int, target: int) -> void: _arrows.append([attacker, target]))
	var node := _session.world.find_ref_node(BANDIT) as SkydotActor
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
	var archer_ref := 0
	var archer: SkydotActor = null
	for candidate in node.get_parent().find_children("*", "SkydotActor", true, false):
		var ref: int = candidate.get_meta("skydot_ref", 0)
		if ref != 0 and ref != BANDIT and not papyrus.is_actor_dead(ref) and papyrus.get_actor_value(ref, "Aggression") >= 2 \
				and papyrus.melee_weapon(ref) != 0 and archer == null:
			archer = candidate
			archer_ref = ref
	expect(archer != null, "another aggressive actor is in the cell")
	if archer == null:
		quit(1)
		return
	expect(_session.save_game(SAVE), "saved before the fight")

	# The fight: the archer shoots, the bandit is paralysed.
	papyrus.add_item(archer_ref, HUNTING_BOW, 1)
	papyrus.add_item(archer_ref, IRON_ARROW, 30)
	while papyrus.melee_weapon(archer_ref) != 0:
		papyrus.transfer(archer_ref, PLAYER, papyrus.melee_weapon(archer_ref), 1)
	archer.teleport(home)
	archer.face(atan2(-forward.x, -forward.z))
	expect(_session.combat.npc_shoot(archer_ref), "the archer draws on the player")
	var shooting := await _until(func() -> bool: return not _arrows.is_empty(), 30.0)
	expect(shooting >= 0.0, "the archer shoots")
	papyrus.break_invisibility(PLAYER)
	expect(papyrus.land_spell(PLAYER, PARALYZE, BANDIT)["ok"], "Paralyze lands on the bandit")
	var staged := await _until(func() -> bool:
		if _session.combat._shots.is_empty():
			_session.combat.npc_shoot(archer_ref)
		return _session.corpses.is_down(BANDIT) and not _session.projectiles._live.is_empty() \
			and not _session.combat._shots.is_empty(), 40.0)
	var combat := _session.combat
	var corpses := _session.corpses
	var projectiles := _session.projectiles
	print("staged after %s s: live %d, shots %d, paralysed %s, down %s" % [staged, projectiles._live.size(),
		combat._shots.size(), corpses._paralysed.keys(), corpses.is_down(BANDIT)])
	expect(staged >= 0.0, "mid-fight: an arrow flies or sticks, another is drawn, the bandit lies paralysed")
	expect(not corpses._paralysed.is_empty() and corpses.is_down(BANDIT), "the bandit is paralysed and down")
	var old_arrows: Array[Node] = []
	for shot: Dictionary in projectiles._live:
		old_arrows.append(shot["node"])
	var old_bodies: Array = []
	for body: Dictionary in corpses._bodies:
		old_bodies.append(body["node"])
	var old_hits := _hits.size()
	var old_arrow_count := _arrows.size()
	expect(not old_arrows.is_empty(), "there are arrow nodes to be freed")

	# The load.
	expect(_session.load_game(SAVE), "the save loads")
	expect(combat._shots.is_empty(), "no arrow is still to leave a bow")
	expect(combat._npc_attacks.is_empty(), "no actor's attack goes on")
	expect(combat._drawn.is_empty(), "nobody's weapon is out in the stance table")
	expect(combat._swaps.is_empty() and combat._effects.is_empty(), "no hand change or impact art goes on")
	expect(corpses._paralysed.is_empty(), "nobody is marked paralysed")
	expect(corpses._hits.is_empty() and corpses._live.is_empty(), "no pending hit or death")
	for body: Dictionary in corpses._bodies:
		expect(not old_bodies.has(body["node"]), "no body of an actor node of before")
	expect(not corpses.is_down(BANDIT), "the bandit does not lie")
	expect(not papyrus.magic_overrides(BANDIT).get("paralyzed", false), "and the paralysis is gone from the scripts")
	expect(projectiles._live.is_empty(), "no projectile is in the list")
	expect(_session.npc_magic._casts.is_empty() and _session.npc_shouts._breaths.is_empty(), "no cast or breath")
	await _frames(3)
	for arrow in old_arrows:
		expect(not is_instance_valid(arrow), "an arrow node of before is freed")
	for body_node in old_bodies:
		expect(not is_instance_valid(body_node) or body_node != _session.world.find_ref_node(BANDIT), "the old bandit node is gone")

	# And nothing of the old fight lands afterwards.
	await _frames(300)
	var late := _hits.slice(old_hits).filter(func(h: Dictionary) -> bool: return h["result"].get("ammo", 0) == IRON_ARROW)
	expect(late.is_empty(), "no hit of an arrow after the load: %s" % [late])
	expect(_arrows.size() == old_arrow_count, "no arrow leaves a bow after the load")
	expect(not corpses.is_down(BANDIT), "the bandit still stands after 5 s")
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE))
	print("smoke_game_lifecycle: failures=%d" % failures)
	quit(failures)
