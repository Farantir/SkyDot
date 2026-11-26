# SPDX-License-Identifier: GPL-3.0-or-later
#
# Patrol waits on the real SE pack (SKYDOT_SSE_PACK): Skuli 0x199D0 in OldHroldanInn patrols four points
# whose REFR XPRD give each an idle time. He reaches one, stands there for its wait, then walks on. Headless;
# skips without the pack.
extends SceneTree

const SKULI := 0x199D0
const CELL := "OldHroldanInn"
const SECONDS := 120.0

var failures := 0
var _moved := 0  # frames he moved while waiting


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_patrol_wait_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


func _actor(root: Node, ref: int) -> Node3D:
	for actor in root.find_children("*", "SkydotActor", true, false):
		if actor.get_meta("skydot_ref", 0) == ref:
			return actor
	return null


func _run(pack_dir: String) -> void:
	await process_frame
	var pack := SkydotPack.new()
	if pack.open(pack_dir) != OK:
		printerr("smoke_patrol_wait_sse: ", pack.get_error())
		quit(1)
		return
	var world := pack.open_world()
	var papyrus := SkydotPapyrus.new()
	papyrus.setup(pack, world)
	papyrus.start_game_enabled_quests()
	var ai := SkydotAi.new()
	ai.setup(world, papyrus)
	ai.days = 12.0 / 24.0
	ai.time_scale = 20.0
	var space := world.find_cell(CELL)
	expect(space != 0, "the cell %s is found" % CELL)
	ai.set_space(space)
	ai.place_actors()
	var root3d := Node3D.new()
	root.add_child(root3d)
	var cell := world.build_cell(space)
	root3d.add_child(cell)
	ai.attach_built(root3d)
	var skuli := _actor(root3d, SKULI)
	expect(skuli != null, "Skuli 0x%X is built" % SKULI)
	if skuli == null:
		quit(1)
		return
	var dt := 1.0 / 60.0
	var elapsed := 0.0
	var waited := 0.0       # the longest stand at one point
	var stand := 0.0
	var stand_at := Vector3()
	var walked_after := false
	var procedure := ""
	while elapsed < SECONDS and not walked_after:
		ai.update(dt)
		papyrus.update(dt)
		await physics_frame
		elapsed += dt
		var state := ai.get_actor_state(SKULI)
		procedure = state.get("procedure", "")
		if procedure == "patrol" and float(state.get("patrol_wait", 0.0)) > 0.0:
			if stand == 0.0:
				stand_at = skuli.global_position
			stand += dt
			if skuli.global_position.distance_to(stand_at) > 0.1:
				_moved += 1
		elif stand > 0.0:
			waited = maxf(waited, stand)
			stand = 0.0
		elif waited > 1.0:
			walked_after = skuli.global_position.distance_to(stand_at) > 0.5
	expect(procedure == "patrol", "Skuli patrols (%s)" % procedure)
	expect(_moved == 0, "he stands still while he waits (%d frames moved)" % _moved)
	expect(waited > 1.0, "he waits at a patrol point (%.1f s)" % waited)
	expect(walked_after, "he walks on after the wait (%.0f s, %.1f m)" % [elapsed, skuli.global_position.distance_to(stand_at)])
	print("smoke_patrol_wait_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)
