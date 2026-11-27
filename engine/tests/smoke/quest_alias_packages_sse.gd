# SPDX-License-Identifier: GPL-3.0-or-later
#
# Quest alias packages on the real SE pack (SKYDOT_SSE_PACK): Hadvar fills the alias Hadvar of MQ101 (priority
# 80) and of MQ101DragonAttack (81); their packages (ALPC) come before his own, the higher priority first. The
# player stands by the block. At stage 180 he runs one of MQ101DragonAttack's; at stage 200 MQ101's
# MQ101HadvarIntoKeep: he walks to the keep door (about 400 units, a stretch where he does not stick) and goes
# in. MQ101 stopped, its alias is empty and none of its packages run. Headless; skips without the pack.
extends SceneTree

const QUEST := 0x3372B  # MQ101
const DRAGON_ATTACK := 0xD0593  # MQ101DragonAttack
const ALIAS := 1  # Hadvar, in both
const INTO_KEEP := 0x27219  # MQ101HadvarIntoKeep
const KEEP := 0x5DE24  # its destination's cell
const BLOCK := Vector3(15210.0, -78987.0, 8192.0)  # where the player stands
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _viewpoint: Camera3D


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_quest_alias_packages_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


func _steps(count: int) -> void:
	_viewpoint.position = SkydotWorld.skyrim_position(BLOCK) + Vector3(0, GameSession.EYE_HEIGHT, 0)
	for i in count:
		_session.step(STEP, SkydotWorld.skyrim_position(BLOCK))
		await process_frame


func _at(ref: int) -> Vector3:
	var node := _session.world.find_ref_node(ref) as Node3D
	return SkydotWorld.godot_to_skyrim(node.global_position) if node != null and node.is_inside_tree() else Vector3.INF


func _alias_packages(quest: int) -> Array:
	for a in _session.world.get_quest(quest).get("aliases", []):
		if int(a["id"]) == ALIAS:
			return a["packages"]
	return []


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.preload_doors = false
	config.grass = false
	_viewpoint = Camera3D.new()
	root.add_child(_viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	root.add_child(_session)
	expect(_session.open(config, _viewpoint, PlayerBody.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	var world := _session.world
	var hadvar := world.find_actor_of(world.find_npc("Hadvar"))
	var own: Array = _alias_packages(QUEST)
	var higher: Array = _alias_packages(DRAGON_ATTACK)
	expect(INTO_KEEP in own and not higher.is_empty(), "both aliases carry packages (%d, %d)" % [own.size(), higher.size()])
	expect(_session.enter_world("Tamriel", SkydotWorld.skyrim_position(BLOCK) + Vector3(0, GameSession.EYE_HEIGHT, 0), null),
		"Tamriel entered by the block")
	expect(_session.papyrus.start_quest(QUEST), "MQ101 starts")
	await _steps(120)
	expect(_session.papyrus.set_stage(QUEST, 180), "MQ101 is at stage 180")
	await _steps(180)
	expect(_session.papyrus.get_alias_ref(QUEST, ALIAS) == hadvar and _session.papyrus.get_alias_ref(DRAGON_ATTACK, ALIAS) == hadvar,
		"Hadvar 0x%X fills both aliases" % hadvar)
	var state: Dictionary = _session.ai.get_actor_state(hadvar)
	expect(int(state.get("package", 0)) in higher, "he runs MQ101DragonAttack's alias package first (%s)" % state.get("package_editor_id"))
	_session.papyrus.set_stage(QUEST, 200)
	await _steps(30)
	state = _session.ai.get_actor_state(hadvar)
	expect(int(state.get("package", 0)) == INTO_KEEP, "stage 200: he runs MQ101HadvarIntoKeep (%s)" % state.get("package_editor_id"))
	var from := _at(hadvar)
	var farthest := 0.0
	var door := 0
	var elapsed := 0
	while _at(hadvar) != Vector3.INF and elapsed < 60 * 60:
		await _steps(1)
		elapsed += 1
		farthest = maxf(farthest, _at(hadvar).distance_to(from) if _at(hadvar) != Vector3.INF else 0.0)
		door = maxi(door, int(_session.ai.get_actor_state(hadvar).get("door", 0)))
	expect(farthest > 250.0 and door != 0, "he walks %.0f units to the keep door 0x%X" % [farthest, door])
	state = _session.ai.get_actor_state(hadvar)
	expect(_at(hadvar) == Vector3.INF and int(state.get("space", 0)) == KEEP,
		"he goes into the keep (%.0f s; space 0x%X)" % [elapsed / 60.0, int(state.get("space", 0))])
	_session.papyrus.stop_quest(QUEST)
	await _steps(60)
	state = _session.ai.get_actor_state(hadvar)
	expect(_session.papyrus.get_alias_ref(QUEST, ALIAS) == 0 and not int(state.get("package", 0)) in own,
		"MQ101 stopped, none of its alias packages runs (%s)" % state.get("package_editor_id"))
	print("smoke_quest_alias_packages_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)
