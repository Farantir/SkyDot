# SPDX-License-Identifier: GPL-3.0-or-later
#
# MapCamera (viewer/map_camera.gd): the world map's camera as measured in the game, on a made-up land.
extends "gd_test.gd"

const TAMRIEL := {"map_nw": Vector2i(-30, 15), "map_se": Vector2i(40, -40), "map_min_height": 50000.0,
	"map_max_height": 80000.0, "map_initial_pitch": 50.0}


func _camera(height: float) -> MapCamera:
	var cam := MapCamera.new()
	cam.configure(TAMRIEL)
	cam.height_at = func(_x: float, _y: float) -> float: return height
	return cam


func test_opening_pose() -> void:
	var cam := _camera(15726.9)
	cam.open(Vector3(17224.771, -47204.449, -7.733))
	# Riverwood (@mapcam): (17224.77, -95452.69, 73226.88), pitch 0.988248, yaw 0.
	expect(cam.position.distance_to(Vector3(17224.77, -95452.69, 73226.88)) < 0.1, "opening position %s" % cam.position)
	expect(absf(cam.pitch - 0.988248) < 1e-4, "pitch %f" % cam.pitch)
	expect_eq(cam.yaw, 0.0, "yaw")
	expect_eq(cam.zoom, 0.25, "zoom")


func test_the_limits_clamp_the_camera() -> void:
	var cam := _camera(0.0)
	cam.open(Vector3(-200000.0, 200000.0, 0.0))
	expect_eq(Vector2(cam.position.x, cam.position.y), Vector2(-122880.0, 61440.0), "clamped to MNAM's cells x 4096")


func test_zoom_moves_only_z() -> void:
	var cam := _camera(1000.0)
	cam.open(Vector3.ZERO)
	var xy := Vector2(cam.position.x, cam.position.y)
	cam.zoom_by(-1.0)
	expect_eq(cam.position.z, 51000.0, "zoomed in: min + ground")
	cam.zoom_by(5.0)
	expect_eq(cam.position.z, 81000.0, "zoomed out: max + ground")
	expect_eq(Vector2(cam.position.x, cam.position.y), xy, "x/y kept")


func test_a_pan_keeps_z_and_the_zoom_follows_the_ground() -> void:
	var height := [10000.0]
	var cam := MapCamera.new()
	cam.configure(TAMRIEL)
	cam.height_at = func(_x: float, _y: float) -> float: return height[0]
	cam.open(Vector3.ZERO)
	var z := cam.position.z
	cam.pan(Vector2(0.0, 1.0), 0.5)
	# 0.5 s north at zoom 0.25: 0.813 x 63750 x 0.5.
	expect(absf(cam.offset.y - (-cam.min_height - 0.25 * 30000.0) / tan(deg_to_rad(50.0)) - 0.813 * 63750.0 * 0.5) < 1.0,
		"pan distance %f" % cam.offset.y)
	height[0] = 0.0  # lower land: z stays, the zoom fraction rises
	for i in 120:
		cam.step(1.0 / 60.0)
	expect_eq(cam.position.z, z, "z kept over lower land")
	expect(cam.zoom > 0.5, "zoom fraction above the ground %f" % cam.zoom)
	height[0] = 30000.0  # higher land pushes it up
	for i in 120:
		cam.step(1.0 / 60.0)
	expect(cam.position.z >= cam.min_height + 29000.0, "pushed above min + ground %f" % cam.position.z)
	cam.recenter()
	expect_eq(cam.zoom, 0.25, "E: the opening zoom again")


func test_look_mode_stays_in_the_map_menu_limits() -> void:
	var cam := _camera(0.0)
	cam.open(Vector3.ZERO)
	cam.look(10.0, 10.0)
	expect_eq(cam.yaw, deg_to_rad(40.0), "yaw at +fMapWorldYawRange / 2")
	expect_eq(cam.pitch, deg_to_rad(75.0), "pitch at fMapWorldMaxPitch")
	cam.look(-10.0, -10.0)
	expect_eq(cam.yaw, deg_to_rad(-40.0), "yaw at -40")
	expect_eq(cam.pitch, deg_to_rad(15.0), "pitch at fMapWorldMinPitch")


func test_edge_panning_band() -> void:
	var size := Vector2(1100.0, 800.0)
	expect_eq(MapCamera.edge_pan(size * 0.5, size), Vector2.ZERO, "the middle does not pan")
	expect_eq(MapCamera.edge_pan(Vector2(1100.0, 400.0), size), Vector2(1.0, 0.0), "the right edge: full speed east")
	expect(absf(MapCamera.edge_pan(Vector2(1062.4, 400.0), size).x - 0.32) < 0.01, "0.32 into the band")
	expect_eq(MapCamera.edge_pan(Vector2(0.0, 0.0), size), Vector2(-1.0, 1.0), "the top left corner: north-west")
