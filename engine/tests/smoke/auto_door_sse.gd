# SPDX-License-Identifier: GPL-3.0-or-later
#
# Automatic load doors on the real SE pack (SKYDOT_SSE_PACK): Greywater Grotto's mouth in Tamriel is the
# AutoLoadDoor01 0x15DF1 (DOOR flag automatic, no collision); 420 units away nothing happens, within
# fAutoDoorActivateDistance (350) the player is taken into its interior 0x1526B. Headless; skips without the pack.
extends SceneTree

const DOOR := 0x15DF1
const INTERIOR := 0x1526B
const DOOR_AT := Vector3(15389.5, -91253.7, 10801.9)
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _viewpoint: Camera3D


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_auto_door_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


func _steps(count: int, feet: Vector3) -> void:
	_viewpoint.position = SkydotWorld.skyrim_position(feet) + Vector3(0, GameSession.EYE_HEIGHT, 0)  # the streamer follows it
	for i in count:
		_session.step(STEP, SkydotWorld.skyrim_position(feet))
		await process_frame


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
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
	var outside := Vector3(15252.4, -90880.7, 10800.0)  # where the player stopped, 420 units out
	expect(_session.enter_world("Tamriel", SkydotWorld.skyrim_position(outside) + Vector3(0, GameSession.EYE_HEIGHT, 0), null),
		"Tamriel entered at the grotto")
	var frames := 0
	while not _session.streamer.get_load_doors().has(DOOR) and frames < 1200:
		await _steps(1, outside)
		frames += 1
	expect(_session.streamer.get_load_doors().has(DOOR), "the auto door 0x%X is a load door of the place" % DOOR)
	await _steps(30, outside)
	expect(_session.place.cell_id != INTERIOR and not _session.transition.is_fading(), "420 units out: still outside")
	var inside := DOOR_AT + (outside - DOOR_AT).normalized() * 300.0
	await _steps(10, inside)
	var waited := 0
	while _session.place.cell_id != INTERIOR and waited < 600:
		await _steps(1, inside)
		waited += 1
	expect(_session.place.cell_id == INTERIOR, "300 units from it: taken into 0x%X (in 0x%X)" % [INTERIOR, _session.place.cell_id])
	print("smoke_auto_door_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)
