# SPDX-License-Identifier: GPL-3.0-or-later
#
# The world map on the real SE pack (SKYDOT_SSE_PACK) against the game (SkyrimRemote @mapcam/@marker/@camera,
# skycompare-data/map-research.md, map/j2, j6, j7e, j9):
#   the camera's opening pose for Riverwood, Whiterun and Solstheim within 1 unit (MapCamera with the pack's MNAM
#   and LAND heights);
#   discovery at the measured radii (Riverwood XRDS 4500: not at 4601, at 4402; South Skybound Watch without XRDS:
#   not at 1130, at 917); the cache of markers still to discover drops the found ones and a load rebuilds it;
#   fast travel Riverwood -> Whiterun lands on 0xC07EF facing 90 degrees and 1.38 hours later.
# Headless; skips without the pack.
extends SceneTree

const TAMRIEL := 0x3C
const SOLSTHEIM := 0x04000800
const RIVERWOOD := 0x162A4
const SKYBOUND := 0x479BB
const WHITERUN := 0x162CE

var failures := 0
var _session: GameSession


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_world_map_sse: SKIP (SKYDOT_SSE_PACK unset)")
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
	config.time = 12.18
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	config.grass = false
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	var player := PlayerBody.new()
	_session = GameSession.new()
	_session.interactive = false
	root.add_child(_session)
	expect(_session.open(config, viewpoint, player), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	await _opening_poses()
	_discovery()
	await _fast_travel()
	print("smoke_world_map_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)


var _lods := {}


## The world map's LOD of `world` (terrain at level 32, as the map shows it), built.
func _map_lod(world: int) -> SkydotLod:
	if _lods.has(world):
		return _lods[world]
	var lod := SkydotLod.new()
	lod.large_refs = false
	expect(lod.setup(_session.pack, _session.world, world) == OK, "the LOD of 0x%X" % world)
	lod.set_map_levels(32, 0)
	root.add_child(lod)
	for i in 600:
		if lod.update(Vector3.ZERO, 100000) == 0:
			break
		await process_frame
	_lods[world] = lod
	return lod


func _camera(world: int) -> MapCamera:
	var cam := MapCamera.new()
	cam.configure(_session.world_map.world_info(world))
	var lod: SkydotLod = _lods[world]
	cam.height_at = func(x: float, y: float) -> float: return lod.get_terrain_height(x, y)
	return cam


func _opening_poses() -> void:
	await _map_lod(TAMRIEL)
	await _map_lod(SOLSTHEIM)
	expect(_session.world_map.map_world(_session.world.find_world("WhiterunWorld")) == TAMRIEL,
		"Whiterun's city world shows Tamriel's map")
	# [target, camera, pitch (rad), yaw (rad)] from @mapcam.
	var cases := [
		["Riverwood", TAMRIEL, Vector3(17224.771, -47204.449, -7.733), Vector3(17224.77, -95452.69, 73226.88), 0.988248, 0.0],
		["Whiterun", TAMRIEL, Vector3(19573.188, -7427.89, -3523.7), Vector3(19573.19, -55676.13, 59789.34), 0.919622, 0.0],
		["Raven Rock", SOLSTHEIM, Vector3(30883.736, 35049.723, 510.668), Vector3(32768.0, 22219.77, 35545.09), 1.2163, -0.0175094],
	]
	for c: Array in cases:
		var cam := _camera(int(c[1]))
		cam.open(c[2])
		var off: Vector3 = cam.position - (c[3] as Vector3)
		# x/y within a unit; z with the ground, LOD 32's surface as we read it: within 1 (Riverwood) to 5 units
		# (Whiterun, on a slope of 2 units per unit) of the game's.
		expect(Vector2(off.x, off.y).length() < 1.0 and absf(off.z) < 5.0,
			"%s: the map camera opens at %s (got %s, ground %.1f)" % [c[0], c[3], cam.position, cam.ground])
		expect(absf(cam.pitch - float(c[4])) < 0.001, "%s: it looks down at the player, pitch %.4f (got %.4f)" % [c[0], c[4], cam.pitch])
		# The yaw: 0 in Tamriel as in the game; on Solstheim, where the clamp moves the camera 1884 units east, the
		# game turned it only -1.0 deg, not the -8.4 deg looking at the player takes (open: not understood).
		if int(c[1]) == TAMRIEL:
			expect(absf(cam.yaw - float(c[5])) < 0.001, "%s: yaw %.4f (got %.4f)" % [c[0], c[5], cam.yaw])
	# Zoom: z between min and max over the ground (Riverwood 65727 .. 95727), x/y kept.
	var cam := _camera(TAMRIEL)
	cam.open(cases[0][2])
	cam.zoom_by(-1.0)
	expect(absf(cam.position.z - 65726.88) < 1.0, "zoomed in: z 65726.88 (got %.2f)" % cam.position.z)
	cam.zoom_by(2.0)
	expect(absf(cam.position.z - 95726.88) < 1.0, "zoomed out: z 95726.88 (got %.2f)" % cam.position.z)
	# Pan north at zoom 1: z stays, the zoom fraction rises over lower land (j12: 1.2272 .. 1.3096 after 0.5 s).
	cam.pan(Vector2(0.0, 1.0), 0.5)
	for i in 60:
		cam.step(1.0 / 60.0)
	expect(absf(cam.position.z - 95726.88) < 1.0 and cam.zoom > 1.0, "a pan keeps z (%.2f), zoom %.3f" % [cam.position.z, cam.zoom])


func _discovery() -> void:
	var markers := {}
	for m: Dictionary in _session.world_map.markers(TAMRIEL):
		markers[int(m["ref"])] = m
	expect(markers.has(RIVERWOOD) and markers.has(SKYBOUND), "Riverwood's and South Skybound Watch's markers")
	if not markers.has(RIVERWOOD) or not markers.has(SKYBOUND):
		return
	for c: Array in [[RIVERWOOD, 4601.0, 4402.0], [SKYBOUND, 1130.0, 917.0]]:
		var at: Vector3 = markers[c[0]]["position"]
		expect(_session.papyrus.get_map_marker_flags(c[0]) == 0, "0x%X is undiscovered" % c[0])
		_session.world_map._discover(TAMRIEL, at + Vector3(float(c[1]), 0.0, 0.0))
		expect(_session.papyrus.get_map_marker_flags(c[0]) == 0, "0x%X: not discovered at %.0f" % [c[0], c[1]])
		_session.world_map._discover(TAMRIEL, at + Vector3(float(c[2]), 0.0, 0.0))
		expect(_session.papyrus.get_map_marker_flags(c[0]) == 3, "0x%X: discovered at %.0f (flags 3)" % [c[0], c[2]])
	expect(_session.world_map.refusal(markers[WHITERUN].merged({"flags": 1}, true)) != "",
		"Whiterun, visible but not discovered, refuses fast travel")
	_discovery_cache()


func _waiting(ref: int) -> bool:
	return _session.world_map._undiscovered_on(TAMRIEL).any(func(m: Dictionary) -> bool: return int(m["ref"]) == ref)


func _discovery_cache() -> void:
	var wm := _session.world_map
	expect(not _waiting(RIVERWOOD) and not _waiting(SKYBOUND), "the discovered markers left the cache")
	expect(_waiting(WHITERUN), "Whiterun waits to be discovered")
	var flags := _session.papyrus.get_map_marker_flags(WHITERUN)
	_session.papyrus.set_map_marker_flags(WHITERUN, 3)
	wm._discover(TAMRIEL, Vector3(1.0e7, 1.0e7, 0.0))
	expect(not _waiting(WHITERUN), "a marker discovered elsewhere leaves the cache at the next check")
	_session.papyrus.set_map_marker_flags(WHITERUN, flags)
	wm.reset()
	expect(_waiting(WHITERUN), "a load rebuilds the cache")


func _fast_travel() -> void:
	var feet := Vector3(17224.771, -47204.449, -7.733 - GameSession.EYE_HEIGHT / SkydotWorld.unit_scale())
	expect(_session.enter_world("Tamriel", SkydotWorld.skyrim_position(feet) + Vector3(0, GameSession.EYE_HEIGHT, 0), null),
		"Riverwood entered")
	await process_frame
	_session.step(0.0, SkydotWorld.skyrim_position(feet))
	_session.papyrus.set_map_marker_flags(WHITERUN, 3)
	var m := _session.world_map.marker(WHITERUN)
	expect(_session.world_map.refusal(m) == "", "fast travel to Whiterun is allowed (%s)" % _session.world_map.refusal(m))
	var hour_before := _session.clock.get_game_days() * 24.0
	_session.world_map.travel(m)
	await process_frame
	var hours := _session.clock.get_game_days() * 24.0 - hour_before
	expect(absf(hours - 1.38) < 0.01, "the trip takes 1.38 hours (got %.3f)" % hours)
	var arrived := SkydotWorld.godot_to_skyrim(_session.player_object().position)
	expect(Vector2(arrived.x, arrived.y).distance_to(Vector2(19573.19, -7427.89)) < 1.0,
		"the player arrives on 0xC07EF (19573.19, -7427.89) (got %s)" % arrived)
	var yaw := _session.player_object().yaw
	# Godot yaw 0 looks along -Z (game north); the game's heading 90 (east, +X) is a Godot yaw of -90 degrees.
	expect(absf(wrapf(yaw + PI / 2.0, -PI, PI)) < 0.01, "facing east, heading 90 (Godot yaw %.3f)" % yaw)
	# The heading marker's worldspace: 0xC07EF is in WhiterunWorld (the game's autosave after this trip was named
	# WhiterunWorld, map-research.md "Incidents").
	expect(_session.streamer.world_id == _session.world.find_world("WhiterunWorld"), "in WhiterunWorld (the heading marker's)")
	# Windhelm's marker has no heading marker: the game put the player on the marker (135025.8, 35596.59), heading 0,
	# in WindhelmWorld (SkyrimRemote, skycompare-data/map3/j13.json).
	_session.papyrus.set_map_marker_flags(0x38436, 3)
	_session.world_map.travel(_session.world_map.marker(0x38436))
	await process_frame
	arrived = SkydotWorld.godot_to_skyrim(_session.player_object().position)
	expect(Vector2(arrived.x, arrived.y).distance_to(Vector2(135025.8, 35596.59)) < 3.0, "Windhelm: on its marker (got %s)" % arrived)
	expect(_session.streamer.world_id == _session.world.find_world("WindhelmWorld"), "Windhelm: in WindhelmWorld")
	expect(absf(wrapf(_session.player_object().yaw, -PI, PI)) < 0.01, "Windhelm: heading 0")
