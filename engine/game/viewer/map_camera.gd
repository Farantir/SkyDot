# SPDX-License-Identifier: GPL-3.0-or-later
#
# The world map's camera as the game moves it (MapMenu's MapCamera, world state), in game units. Measured with
# SkyrimRemote @mapcam (skycompare-data/map-research.md section 4, skycompare-data/map3/j12.json, 2026-11-22):
#   open    the target is the player (or the last exterior position); the camera stands at target + (0, -D),
#           D = (min + 0.25 (max - min)) / tan(MNAM initial pitch), x/y clamped to the MNAM cells x 4096; zoom 0.25,
#           z = min + zoom (max - min) + ground, ground the land's height under the camera; then it looks at the
#           target (yaw and pitch fixed from here: 56.6 deg at Riverwood, not MNAM's 50). The offset kept is the
#           clamped one (Solstheim: (1884, -12830)).
#   zoom    the zoom fraction moves 0..1 at ZOOM_RATE a second while a trigger (or key) is held; z follows it,
#           z = min + zoom (max - min) + ground; x, y, yaw and pitch stay.
#   pan     the offset moves at PAN_KEY_FACTOR x lerp(fMapWorldMinPanSpeed, MaxPanSpeed, zoom) units a second
#           (zoom clamped 0..1) along the map's north/east (the camera's yaw is 0); x/y clamped; z stays where it
#           is (95727 kept while panning at zoom 1), only pushed up so it never sinks below min + ground; the zoom
#           fraction is then what z is above the ground (above 1 over low land: 1.82 measured).
#   ground  follows the land under the camera at fMapWorldHeightAdjustmentForce (4) a second, not at once (it
#           kept changing a second after a pan stopped).
#   E       back to the opening state.
#   look    the right mouse button or the right stick turn the camera in place (LOOK_*, STICK_*), within the
#           [MapMenu] yaw range and pitch limits.
class_name MapCamera
extends RefCounted

const OPEN_ZOOM := 0.25
## [MapMenu] exe defaults (formats/pack-format.md, read with @setting).
const MIN_PAN_SPEED := 60000.0
const MAX_PAN_SPEED := 75000.0
const HEIGHT_ADJUSTMENT_FORCE := 4.0
const NEAR_CLIP := 128.0
## Measured (j12): the zoom fraction changes 1.31 a second while RT/LT is held (0.1 s: 0.134, 0.2 s: 0.262,
## 0.3 s: 0.393; fMapWorldZoomSpeed is 2).
const ZOOM_RATE := 1.31
## Measured (j12): a held W moved the offset 0.813 x lerp(Min, Max, zoom) a second (zoom 0: 24628 in 0.5 s,
## 0.25: 26144, 1: 30688; 1 s and 2 s holds 51966 and 51826 a second at 0.25), no ramp after the first frames.
const PAN_KEY_FACTOR := 0.813
## Measured (map3/j15, SkyrimRemote @menukey mouse=8/9): a wheel notch moves the zoom fraction 0.2 (0.25 -> 0.051,
## 0 -> 0.589 after three).
const WHEEL_STEP := 0.2
## Look mode (map3/j15): the right mouse button held, a mouse move turns the camera in place, 0.00039 rad of yaw and
## 0.00027 of pitch per count (100 and 300 counts a frame; small moves of 10 turned about twice that), down to the
## right; the right stick turns it 0.0129 rad of yaw and 0.0092 of pitch a second at full deflection (up looks up).
## The limits are the [MapMenu] settings, fMapWorldYawRange 80 (+-40 deg) and fMapWorldMin/MaxPitch 15..75; the
## measured turns (yaw -38 .. +30 deg, pitch 22 .. 73 deg) stayed inside them.
const LOOK_YAW_PER_COUNT := 0.00039
const LOOK_PITCH_PER_COUNT := 0.00027
const STICK_YAW_RATE := 0.0129
const STICK_PITCH_RATE := 0.0092
const YAW_RANGE := deg_to_rad(80.0)
const MIN_PITCH := deg_to_rad(15.0)
const MAX_PITCH := deg_to_rad(75.0)
## Edge panning (map3/j15): the cursor in the outer 5% of the screen (fMapWorldCursorMoveArea 0.9) pans that way at
## up to EDGE_FACTOR x lerp(Min, Max, zoom) a second, growing with how far into the band it is (at the edge 65600
## units in 2 s, 0.36 into the band 22500).
const CURSOR_AREA := 0.9
const EDGE_FACTOR := 0.51

var min_height := 50000.0
var max_height := 80000.0
var initial_pitch := deg_to_rad(50.0)
## The camera's x/y limits (MNAM cells x 4096): the NW..SE rectangle.
var low := Vector2(-122880.0, -163840.0)
var high := Vector2(163840.0, 61440.0)
## What the camera looked at when the map opened (game units).
var target := Vector3.ZERO
var offset := Vector2.ZERO
var zoom := OPEN_ZOOM
## The camera (game units) and its fixed orientation.
var position := Vector3.ZERO
var yaw := 0.0
var pitch := 0.0
## The smoothed land height under the camera.
var ground := 0.0
## (x, y) -> land height in game units, NAN where there is none (SkydotWorld.get_land_height).
var height_at: Callable = func(_x: float, _y: float) -> float: return 0.0


## The map data of a worldspace (SkydotWorld.list_worlds entry with map_nw, map_se, map_min_height, ...).
func configure(world: Dictionary) -> void:
	if world.has("map_nw"):
		var nw: Vector2i = world["map_nw"]
		var se: Vector2i = world["map_se"]
		low = Vector2(nw.x, se.y) * SkydotWorld.CELL_UNITS
		high = Vector2(se.x, nw.y) * SkydotWorld.CELL_UNITS
		min_height = float(world["map_min_height"])
		max_height = float(world["map_max_height"])
		initial_pitch = deg_to_rad(float(world["map_initial_pitch"]))


## Open on `at` (game units): the game's opening state.
func open(at: Vector3) -> void:
	target = at
	var d := (min_height + OPEN_ZOOM * (max_height - min_height)) / tan(initial_pitch)
	var xy := _clamped(Vector2(at.x, at.y - d))
	offset = xy - Vector2(at.x, at.y)
	zoom = OPEN_ZOOM
	ground = _land(xy)
	position = Vector3(xy.x, xy.y, _z_for(zoom))
	var to := at - position
	yaw = atan2(to.x, to.y)  # 0 north, clockwise
	pitch = atan2(-to.z, Vector2(to.x, to.y).length())  # down positive


## E (PlayerPosition): the opening state again.
func recenter() -> void:
	open(target)


## Zoom by `amount` of the fraction (positive out): z follows, x/y stay.
func zoom_by(amount: float) -> void:
	zoom = clampf(minf(zoom, 1.0) + amount if amount > 0.0 else maxf(zoom, 0.0) + amount, 0.0, 1.0)
	position.z = _z_for(zoom)


## Look mode: turn by `yaw_by`, `pitch_by` radians (pitch positive looks further down), within the limits.
func look(yaw_by: float, pitch_by: float) -> void:
	yaw = clampf(yaw + yaw_by, -YAW_RANGE * 0.5, YAW_RANGE * 0.5)
	pitch = clampf(pitch + pitch_by, MIN_PITCH, MAX_PITCH)


## Edge panning with the cursor at `at` of a view `size`: the pan speed's share for each axis (x east, y north).
static func edge_pan(at: Vector2, size: Vector2) -> Vector2:
	var margin := (1.0 - CURSOR_AREA) * 0.5
	var f := at / size
	var out := Vector2.ZERO
	out.x = clampf((f.x - (1.0 - margin)) / margin, 0.0, 1.0) - clampf((margin - f.x) / margin, 0.0, 1.0)
	out.y = clampf((margin - f.y) / margin, 0.0, 1.0) - clampf((f.y - (1.0 - margin)) / margin, 0.0, 1.0)
	return out


## Pan by `direction` (x east, y north; length up to 1 for full speed) for `delta` seconds of a held key.
func pan(direction: Vector2, delta: float, factor := PAN_KEY_FACTOR) -> void:
	var speed := factor * lerpf(MIN_PAN_SPEED, MAX_PAN_SPEED, clampf(zoom, 0.0, 1.0))
	var xy := _clamped(Vector2(position.x, position.y) + direction.rotated(-yaw) * speed * delta)
	offset = xy - Vector2(target.x, target.y)
	position.x = xy.x
	position.y = xy.y


## Move the camera to `xy` (game units) as a pan gets there: clamped, z kept.
func move_to(xy: Vector2) -> void:
	xy = _clamped(xy)
	offset = xy - Vector2(target.x, target.y)
	position.x = xy.x
	position.y = xy.y


## Each frame: the ground follows the land under the camera; z is pushed up over rising land; the zoom fraction
## is what z stands above the ground.
func step(delta: float) -> void:
	var land := _land(Vector2(position.x, position.y))
	ground += (land - ground) * minf(1.0, HEIGHT_ADJUSTMENT_FORCE * delta)
	position.z = maxf(position.z, min_height + ground)
	zoom = (position.z - min_height - ground) / (max_height - min_height)


## The camera in Godot space (metres, -Z forward).
func transform() -> Transform3D:
	var forward := Vector3(sin(yaw) * cos(pitch), cos(yaw) * cos(pitch), -sin(pitch))  # game axes
	var eye := SkydotWorld.skyrim_position(position)
	var ahead := SkydotWorld.skyrim_position(position + forward * 1000.0)
	return Transform3D(Basis.looking_at(ahead - eye, Vector3.UP), eye)


func _z_for(fraction: float) -> float:
	return min_height + fraction * (max_height - min_height) + ground


func _clamped(xy: Vector2) -> Vector2:
	return Vector2(clampf(xy.x, low.x, high.x), clampf(xy.y, low.y, high.y))


func _land(xy: Vector2) -> float:
	var h: float = height_at.call(xy.x, xy.y)
	return h if not is_nan(h) else ground
