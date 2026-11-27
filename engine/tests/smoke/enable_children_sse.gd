# SPDX-License-Identifier: GPL-3.0-or-later
#
# Enable children of a reference a script disabled before their cell was built, on the real SE pack
# (SKYDOT_SSE_PACK). The civil war setup disables the XMarker 0x1B754 at game start; Riverwood's Imperial
# guards follow it, so a Riverwood built afterwards shows none of them (they stood in the road and walked
# about). Headless; skips without the pack.
extends SceneTree

const PARENT := 0x1B754  # XMarker
const GUARDS := [0x1B743, 0x1B744, 0x1B747]  # GuardRiverwoodImperial01, 03, 02
const AT := Vector3(19344.6, -46707.4, -100.0)  # the Riverwood map marker

var failures := 0
var _session: GameSession
var _viewpoint: Camera3D


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_enable_children_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


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
	expect(_session.papyrus.is_ref_disabled(PARENT), "the setup disabled the XMarker 0x%X" % PARENT)
	var eye := SkydotWorld.skyrim_position(AT) + Vector3(0, GameSession.EYE_HEIGHT, 0)
	expect(_session.enter_world("Tamriel", eye, null), "Tamriel entered in Riverwood")
	_viewpoint.position = eye
	for i in 120:
		_session.step(1.0 / 60.0, SkydotWorld.skyrim_position(AT))
		await process_frame
	for guard in GUARDS:
		var node := _session.world.find_ref_node(guard) as Node3D
		expect(_session.papyrus.is_ref_disabled(guard), "guard 0x%X follows its enable parent" % guard)
		expect(node == null or not node.is_visible_in_tree(), "guard 0x%X is not shown" % guard)
	print("smoke_enable_children_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)
