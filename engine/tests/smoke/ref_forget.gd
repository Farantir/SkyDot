# SPDX-License-Identifier: GPL-3.0-or-later
#
# The forget registry on the real SE pack (SKYDOT_SSE_PACK), headless: in BleakFallsBarrow01 actors are spawned
# at run time, built, given items, hurt and made to fight; once every one of them is forgotten
# (SkydotPapyrus.forget_ref) each per-reference store reports what it held before (forget_store_sizes), AI, magic,
# actor values, items and placement included. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const HUNTING_BOW := 0x13985
const IRON_ARROW := 0x1397D
const COUNT := 8
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_ref_forget: SKIP (SKYDOT_SSE_PACK unset)")
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
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	var node := world.find_ref_node(BANDIT) as SkydotActor
	expect(node != null, "the bandit is built")
	if node == null:
		quit(1)
		return
	_session.ai.attach_built(node.get_parent())
	_feet = node.global_position + Vector3.UP * 0.0
	await _frames(60)
	var base := papyrus.get_ref_base(BANDIT)
	var cell := world.get_ref_cell(BANDIT)
	var here := SkydotWorld.godot_to_skyrim(node.global_position)
	var baseline := papyrus.forget_store_sizes()
	print("baseline: ", baseline)
	expect(baseline.has("ai.minds") and baseline.has("magic.effects") and baseline.has("actor_placement"),
		"the AI, magic and placement stores are registered: %s" % [baseline.keys()])

	var refs: Array[int] = []
	for i in COUNT:
		var ref := papyrus.spawn_actor(base, cell, here + Vector3(60.0 * i, 0.0, 0.0), 0.0)
		expect(ref != 0, "actor %d is spawned" % i)
		refs.append(ref)
	await _frames(90)
	for ref in refs:
		papyrus.add_item(ref, HUNTING_BOW, 1)
		papyrus.add_item(ref, IRON_ARROW, 5)
		papyrus.damage_actor_value(ref, "Health", 10.0)
		papyrus.set_locked(ref, true)
		papyrus.set_animation_state(ref, PackedByteArray([1, 2, 3]))
	await _frames(120)
	var during := papyrus.forget_store_sizes()
	print("during: ", during)
	expect(during != baseline, "the spawned actors show in the stores")

	for ref in refs:
		papyrus.forget_ref(ref)
	await _frames(30)
	var after := papyrus.forget_store_sizes()
	print("after: ", after)
	for store: String in baseline:
		expect(after.get(store, -1) == baseline[store], "%s back to %s (is %s)" % [store, baseline[store], after.get(store, -1)])
	print("smoke_ref_forget: failures=%d" % failures)
	quit(failures)
