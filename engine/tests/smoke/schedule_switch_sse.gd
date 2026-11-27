# SPDX-License-Identifier: GPL-3.0-or-later
#
# Hourly package switches without a reload, on the real SE pack (SKYDOT_SSE_PACK). Faendal hunts south of
# Riverwood until 07:00 (FaendalHunt, 04:00 for 4 hours, below FaendalCarryFirewood7x2 in his list): loaded
# at 06:55 with the clock running, he takes up the firewood package once the hour is past and walks
# towards it. Then in the Sleeping Giant Inn at 20:00 an actor seated by its package stays seated, in the
# same furniture, across several thinks. Headless; skips without the pack.
extends SceneTree

const FAENDAL := 0x1348C
const HUNT := 0x1CB11            # FaendalHunt
const FIREWOOD := 0x10C702       # FaendalCarryFirewood7x2
const INN := "RiverwoodSleepingGiantInn"

var failures := 0
var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _pack: SkydotPack


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_schedule_switch_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


func _flat(a: Vector3, b: Vector3) -> float:
	return Vector2(a.x, a.y).distance_to(Vector2(b.x, b.y))


func _actor(root3d: Node, ref: int) -> Node3D:
	for actor in root3d.find_children("*", "SkydotActor", true, false):
		if actor.get_meta("skydot_ref", 0) == ref:
			return actor
	return null


func _step(ai: SkydotAi) -> void:
	ai.update(1.0 / 60.0)
	_papyrus.update(1.0 / 60.0)
	await physics_frame


func _run(pack_dir: String) -> void:
	await process_frame
	_pack = SkydotPack.new()
	if _pack.open(pack_dir) != OK:
		printerr("smoke_schedule_switch_sse: ", _pack.get_error())
		quit(1)
		return
	_world = _pack.open_world()
	_papyrus = SkydotPapyrus.new()
	_papyrus.setup(_pack, _world)
	_papyrus.start_game_enabled_quests()
	await _faendal()
	await _seated()
	print("smoke_schedule_switch_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)


func _faendal() -> void:
	var ai := SkydotAi.new()
	ai.setup(_world, _papyrus)
	ai.set_combat(false)
	ai.days = (6.0 + 55.0 / 60.0) / 24.0
	ai.time_scale = 30.0  # five game minutes in ten seconds
	var space := _world.find_world("Tamriel")
	ai.set_space(space)
	ai.place_actors()
	var place: Dictionary = _world.get_actor_place(FAENDAL)
	expect(place.get("space", 0) == space, "at 06:55 Faendal is placed outside (%s)" % place)
	var start: Vector3 = place.get("position", Vector3())
	var root3d := Node3D.new()
	root.add_child(root3d)
	var gx := floori(start.x / SkydotWorld.CELL_UNITS)
	var gy := floori(start.y / SkydotWorld.CELL_UNITS)
	for dx in range(-1, 2):
		for dy in range(-1, 2):
			var cell := _world.build_exterior(space, gx + dx, gy + dy)
			if cell != null:
				root3d.add_child(cell)
	ai.attach_built(root3d)
	var faendal := _actor(root3d, FAENDAL)
	expect(faendal != null, "Faendal 0x%X is built" % FAENDAL)
	if faendal == null:
		return
	var state := ai.get_actor_state(FAENDAL)
	expect(state.get("package", 0) == HUNT, "at %05.2f he hunts (%s)" % [ai.get_hour(), state.get("package_editor_id")])
	var elapsed := 0.0
	while ai.get_actor_state(FAENDAL).get("package", 0) == HUNT and elapsed < 30.0:
		await _step(ai)
		elapsed += 1.0 / 60.0
	state = ai.get_actor_state(FAENDAL)
	expect(state.get("package", 0) == FIREWOOD, "at %05.2f he carries firewood (%s, %.1f s)" % [ai.get_hour(),
		state.get("package_editor_id"), elapsed])
	expect(ai.get_hour() >= 7.0 and ai.get_hour() < 7.05, "the switch comes within a think of 07:00 (%05.3f)" % ai.get_hour())
	for i in 10:
		await _step(ai)
	state = ai.get_actor_state(FAENDAL)  # the step's target is found as it starts
	if not state.has("target"):
		expect(false, "he has somewhere to go (%s)" % state)
		return
	var goal: Vector3 = state["target"]["position"]
	var from := SkydotWorld.godot_to_skyrim(faendal.global_position)
	for i in 60 * 15:
		await _step(ai)
	# The way there leads round the hill: what counts is that he walks a path that ends there.
	var path: PackedVector3Array = faendal.call("get_path")
	var end := SkydotWorld.godot_to_skyrim(path[path.size() - 1]) if path.size() > 0 else Vector3()
	var moved := _flat(SkydotWorld.godot_to_skyrim(faendal.global_position), from)
	state = ai.get_actor_state(FAENDAL)
	expect(state.get("package", 0) == FIREWOOD and faendal.is_walking() and _flat(end, goal) < 150.0 and moved > 300.0,
		"he walks towards the woodpile (%s, moved %.0f units, path ends %.0f units from it)" % [state.get("procedure"),
		moved, _flat(end, goal)])
	root3d.queue_free()
	await process_frame


func _seated() -> void:
	var ai := SkydotAi.new()
	ai.setup(_world, _papyrus)
	ai.set_combat(false)
	ai.days = 1.0 + 20.0 / 24.0
	ai.time_scale = 20.0
	var space := _world.find_cell(INN)
	ai.set_space(space)
	ai.place_actors()
	var root3d := Node3D.new()
	root.add_child(root3d)
	root3d.add_child(_world.build_cell(space))
	ai.attach_built(root3d)
	for i in 60 * 2:
		await _step(ai)
	var seated := 0
	var furniture := 0
	var package := 0
	var who := ""
	for actor in root3d.find_children("*", "SkydotActor", true, false):
		var ref: int = actor.get_meta("skydot_ref", 0)
		var f := ai.get_furniture_state(ref)
		var proc: String = ai.get_actor_state(ref).get("procedure", "")
		if int(f.get("state", 0)) == 3 and proc in ["sit", "eat"]:
			seated = ref
			furniture = f["ref"]
			package = ai.get_actor_state(ref).get("package", 0)
			who = "%s 0x%X (%s)" % [actor.name, ref, ai.get_actor_state(ref).get("package_editor_id")]
			break
	expect(seated != 0, "someone sits in the inn at 20:00 (%s)" % who)
	if seated == 0:
		return
	# Ten seconds: six thinks or more, and over three game minutes.
	var stood := 0
	for i in 60 * 10:
		await _step(ai)
		var f := ai.get_furniture_state(seated)
		if int(f.get("state", 0)) != 3 or f.get("ref", 0) != furniture:
			stood += 1
	var state := ai.get_actor_state(seated)
	expect(state.get("package", 0) == package, "%s keeps the package" % who)
	expect(stood == 0, "%s stays seated in 0x%X across the thinks (%d frames not)" % [who, furniture, stood])
	root3d.queue_free()
	await process_frame
