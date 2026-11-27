# SPDX-License-Identifier: GPL-3.0-or-later
#
# Currents on the real SE pack (SKYDOT_SSE_PACK): a player swimming without input drifts
# downstream, at least 1 m in 5 s:
#   in the White River at Riverwood, cell water at -250 in (5, -11), whose RiverWaterFlow has no
#   NAM0: the flowmap's texel there points north-north-east (0.6, 0.8);
#   in the placed Water1024RiverFlowNE 0x3AC84 in (7, -6) (no cell water there), its surface at
#   the reference's z, -3106: its XWCU (2.54, 1.35, 0), north-east.
# Headless; skips without the pack.
extends SceneTree

const TAMRIEL := 0x3C

var failures := 0


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_water_drift_sse: SKIP (SKYDOT_SSE_PACK unset)")
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
	var pack := SkydotPack.new()
	if pack.open(pack_dir) != OK:
		expect(false, "the pack opens: " + pack.get_error())
		quit(1)
		return
	var world := pack.open_world()
	var host := Node3D.new()
	root.add_child(host)
	var camera := Camera3D.new()
	root.add_child(camera)
	var streamer := SkydotStreamer.new()
	streamer.setup(world, pack, host, camera, null)
	streamer.radius = 1
	streamer.lod_enabled = false
	var player := SkydotPlayer.new()
	root.add_child(player)
	await _drift(streamer, camera, player, Vector3(22752, -41248, -250), Vector2(0.6, 0.8), "Riverwood's White River")
	await _drift(streamer, camera, player, Vector3(32081, -23517, -3106), Vector2(2.54, 1.35), "the placed 0x3AC84")
	expect(is_equal_approx(player.water_current.x, 2.54) and is_equal_approx(player.water_current.z, -1.35),
		"its current is its XWCU: %s" % player.water_current)
	player.free()
	print("smoke_water_drift_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)


## Swim at `at` (game units, z the surface) without input for 5 s; the drift must be at least
## 1 m and along `downstream` (game x east, y north).
func _drift(streamer: SkydotStreamer, camera: Camera3D, player: SkydotPlayer, at: Vector3,
		downstream: Vector2, what: String) -> void:
	streamer.clear()
	camera.position = SkydotWorld.skyrim_position(at)
	streamer.start(TAMRIEL)
	streamer.load_everything()
	var surface := at.z * SkydotWorld.unit_scale()
	player.teleport(SkydotWorld.skyrim_position(at) - Vector3(0, 1.35, 0))
	player.set_input(Vector2.ZERO, 0, SkydotPlayer.RUN)
	streamer.hold_player(player, false)
	# Water1024's box reaches 0.18 m above the reference.
	expect(player.has_water() and absf(player.water_height - surface) < 0.25,
		"%s: in water at %.2f, surface %.2f" % [what, player.water_height, surface])
	var from := player.global_position
	# Swimming until it runs aground (the river shallows downstream of 0x3AC84) or 5 s pass.
	var hz := Engine.physics_ticks_per_second
	var swam := 0
	for i in hz * 5:
		await physics_frame
		camera.position = player.global_position + Vector3(0, player.eye_height, 0)
		streamer.hold_player(player, false)
		if player.is_swimming():
			swam += 1
	var drift := player.global_position - from
	var flat := Vector2(drift.x, -drift.z)  # game x east, y north
	expect(swam >= hz * 2 and flat.length() > 1.0 and flat.normalized().dot(downstream.normalized()) > 0.95,
		"%s: swims %.1f s, drifts downstream %s (%.1f m, current %s)" % [what, float(swam) / hz, flat, flat.length(), player.water_current])
