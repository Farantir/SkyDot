# SPDX-License-Identifier: GPL-3.0-or-later
#
# The HUD drawn from the game's own hudmenu.swf (UiAssets, UiDraw), doing what its ActionScript and the game do
# with it; HudView uses it when the pack has the menus.
#
# Placement (HUDMenu.InitExtensions, decompiled from hudmenu.swf): the meters are locked to the screen's edges
# with GlobalFunc.Lock, Health "B", Magicka "BL", Stamina "BR", the compass "T": their origin moves to the
# visible rectangle's edge inset by the safe rectangle. The movie is scaled to fit (show-all, 1280x720 stage).
# The safe zone is fSafeZoneX/Y:Interface, 15 in the game (read with getini, SkyrimRemote, 2026-11-15).
#
# Meters: the Health/Magica/Stamina sprites fade their meter in over their first 40 frames (alpha 0 to 1, label
# "Pause"), played forward when the value changes (RunMeterAnim) and backward to fade out (FadeOutMeter), at the
# movie's 24 fps. The fill is the meter sprite's frame between its Full and Empty labels (Meter.SetPercent).
# When the game fades a full meter out is the engine's: measured, a full Health meter starts fading 13 s after
# its last change (SkyrimRemote, 1100x800, shots every 30 frames); a meter that is not full stays.
#
# Compass (HUDMenu.SetCompassAngle, UpdateCompassMarkers): the DirectionRect strip moves to
# lerp(ZeroX, ThreeSixtyX, heading / 360), ZeroX and ThreeSixtyX its x at the Compass sprite's "Zero" and
# "ThreeSixty" frames; a marker at bearing b shows when b is within half the mask's width in degrees of the
# heading, at the mask's matching x. Markers are the map markers of the current worldspace within
# fHUDCompassLocationMaxDist, drawn as "Compass Marker" at its LocationMarkers frame plus the marker type minus
# one (checked against the game: Riverwood, a town, shows the town icon). The
# engine also sends each marker's alpha and scale; their rule is not known yet, so they are 100.
class_name HudGame
extends RefCounted

const SWF := "hudmenu"
const STAGE := Vector2(1280.0, 720.0)
const SAFE_ZONE := Vector2(15.0, 15.0)
const FPS := 24.0
const FULL_LINGER := 13.0
const METERS := [
	{"av": "Magicka", "anim": "HUDMovieBaseInstance/Magica", "lock": "BL", "meter": "MagickaMeter_mc"},
	{"av": "Health", "anim": "HUDMovieBaseInstance/Health", "lock": "B", "meter": "HealthLeft"},
	{"av": "Stamina", "anim": "HUDMovieBaseInstance/Stamina", "lock": "BR", "meter": "StaminaMeter_mc"},
]
const CROSSHAIR := "HUDMovieBaseInstance/Crosshair"
const COMPASS_HOLDER := "HUDMovieBaseInstance/CompassShoutMeterHolder"

var _fade := {}  # actor value -> the anim sprite's frame (float)
var _last := {}  # actor value -> value seen last
var _changed := {}  # actor value -> msec of the last change
var _markers := []  # the worldspace's map markers
var _markers_world := -1


## Whether the pack has hudmenu.swf.
static func available() -> bool:
	return UiAssets.available() and not UiAssets.instance(SWF, CROSSHAIR).is_empty()


## Steps the meters' fades: `values` is actor value -> [value, maximum].
func step(values: Dictionary, delta: float) -> void:
	var now := Time.get_ticks_msec()
	for meter: Dictionary in METERS:
		var av: String = meter["av"]
		var pair: Array = values.get(av, [0.0, 0.0])
		if not _last.has(av):
			_changed[av] = -1000000  # the first look is no change: a full meter starts hidden
		elif not is_equal_approx(_last[av], pair[0]):
			_changed[av] = now
		_last[av] = pair[0]
		var pause := _pause_frame(meter)
		var since := (now - int(_changed[av])) / 1000.0
		var shown: bool = pair[1] > 0.0 and (pair[0] < pair[1] or since < FULL_LINGER)
		var frame: float = _fade.get(av, 0.0)
		frame = minf(frame + delta * FPS, pause) if shown else maxf(frame - delta * FPS, 0.0)
		_fade[av] = frame


func _pause_frame(meter: Dictionary) -> float:
	var anim := UiAssets.instance(SWF, meter["anim"])
	var pause := UiAssets.label_frame(SWF, int(anim.get("id", -1)), "Pause")
	return float(pause) if pause > 0 else 0.0


## The stage-to-view transform and the visible rectangle in stage pixels.
static func _fit(area: Vector2) -> Array:
	var scale := minf(area.x / STAGE.x, area.y / STAGE.y)
	var visible := Rect2((STAGE - area / scale) * 0.5, area / scale)
	return [Transform2D(0.0, Vector2(scale, scale), 0.0, -visible.position * scale), visible]


## An instance's transform in the view, its origin moved as GlobalFunc.Lock(`lock`) moves it.
static func placed(area: Vector2, path: String, lock := "") -> Transform2D:
	var found := UiAssets.instance(SWF, path)
	if found.is_empty():
		return Transform2D()
	var fit := _fit(area)
	var visible: Rect2 = fit[1]
	var xform: Transform2D = found["xform"]
	if lock.contains("T"):
		xform.origin.y = visible.position.y + SAFE_ZONE.y
	if lock.contains("B"):
		xform.origin.y = visible.end.y - SAFE_ZONE.y
	if lock.contains("L"):
		xform.origin.x = visible.position.x + SAFE_ZONE.x
	if lock.contains("R"):
		xform.origin.x = visible.end.x - SAFE_ZONE.x
	return (fit[0] as Transform2D) * xform


func draw_meters(canvas: CanvasItem, area: Vector2, values: Dictionary) -> void:
	for meter: Dictionary in METERS:
		var av: String = meter["av"]
		var frame := int(_fade.get(av, 0.0))
		if frame <= 0:
			continue
		var pair: Array = values.get(av, [0.0, 0.0])
		var anim := UiAssets.instance(SWF, meter["anim"])
		var look := UiDraw.Look.new()
		look.frames[int(anim["id"])] = frame
		# The meters HUDMenu makes Components.Meter of (HealthMeterLeft is HealthMeter_mc.HealthLeft).
		look.fills[meter["meter"]] = clampf(pair[0] / pair[1], 0.0, 1.0) if pair[1] > 0.0 else 0.0
		UiDraw.character(canvas, SWF, int(anim["id"]), placed(area, meter["anim"], meter["lock"]), Color.WHITE, look)


func draw_crosshair(canvas: CanvasItem, area: Vector2) -> void:
	var found := UiAssets.instance(SWF, CROSSHAIR)
	UiDraw.character(canvas, SWF, int(found["id"]), placed(area, CROSSHAIR))


## The compass for a player at `position` (game units) facing `heading` (degrees, 0 north, clockwise), with the
## map markers of `world` (0 in an interior: no markers).
func draw_compass(canvas: CanvasItem, area: Vector2, heading: float, position: Vector3, world: int,
		world_data: SkydotWorld) -> void:
	var holder := UiAssets.instance(SWF, COMPASS_HOLDER)
	if holder.is_empty():
		return
	var compass_id := -1
	for placed_entry: Dictionary in UiAssets.character(SWF, int(holder["id"])).get("display", []):
		if placed_entry.get("name", "") == "Compass":
			compass_id = int(placed_entry["character"])
	var rect_zero := _named(compass_id, UiAssets.label_frame(SWF, compass_id, "Zero"), "DirectionRect")
	var rect_full := _named(compass_id, UiAssets.label_frame(SWF, compass_id, "ThreeSixty"), "DirectionRect")
	var mask := _named(compass_id, 0, "CompassMask_mc")
	if rect_zero.is_empty() or rect_full.is_empty() or mask.is_empty():
		return
	heading = fposmod(heading, 360.0)
	var zero_x: float = rect_zero["matrix"][4]
	var full_x: float = rect_full["matrix"][4]
	var rect_x := lerpf(zero_x, full_x, heading / 360.0)
	var look := UiDraw.Look.new()
	look.frames[compass_id] = 0
	look.moves["DirectionRect"] = Vector2(rect_x, rect_zero["matrix"][5])
	# The holder holds the shout meter too; it is the voice's, drawn by HudView.
	var holder_xform := placed(area, COMPASS_HOLDER, "T")
	var compass_local := Transform2D()
	for placed_entry: Dictionary in UiAssets.character(SWF, int(holder["id"])).get("display", []):
		if placed_entry.get("name", "") == "Compass":
			compass_local = UiDraw.matrix(placed_entry["matrix"])
	var compass_xform := holder_xform * compass_local
	UiDraw.character(canvas, SWF, compass_id, compass_xform, Color.WHITE, look)
	if world <= 0 or world_data == null:
		return
	_draw_markers(canvas, compass_xform, rect_x, absf(full_x - zero_x), rect_zero, mask, heading, position, world,
		world_data)


func _draw_markers(canvas: CanvasItem, compass_xform: Transform2D, rect_x: float, span: float,
		rect: Dictionary, mask: Dictionary, heading: float, position: Vector3, world: int, world_data: SkydotWorld) -> void:
	if _markers_world != world:
		_markers = world_data.get_map_markers(world)
		_markers_world = world
	var marker_id := UiAssets.exported(SWF, "Compass Marker")
	var locations := UiAssets.label_frame(SWF, marker_id, "LocationMarkers")
	var mask_rect := UiDraw.matrix(mask["matrix"]) * UiAssets.bounds(SWF, int(mask["character"]))
	if span <= 0.0 or marker_id < 0 or locations < 0:
		return
	var half_degrees := mask_rect.size.x * 0.5 * 360.0 / span
	var holder := Transform2D()
	for entry: Dictionary in UiAssets.character(SWF, int(rect["character"])).get("display", []):
		if entry.get("name", "") == "MarkerHolder":
			holder = UiDraw.matrix(entry["matrix"])
	var rect_xform := UiDraw.matrix(rect["matrix"])
	rect_xform.origin.x = rect_x
	var in_compass := rect_xform * holder  # the marker holder in the compass
	var max_distance := world_data.get_gmst_float("fHUDCompassLocationMaxDist", 0.0)
	for marker: Dictionary in _markers:
		var at: Vector3 = marker["position"]
		var offset := Vector2(at.x - position.x, at.y - position.y)
		if max_distance > 0.0 and offset.length() > max_distance:
			continue
		var bearing := fposmod(rad_to_deg(atan2(offset.x, offset.y)), 360.0)
		var delta := wrapf(bearing - heading, -180.0, 180.0)
		if absf(delta) > half_degrees:
			continue
		# x 0 of the compass (UpdateCompassMarkers centres there) plus the bearing's share of the mask's width.
		var x_compass := delta / half_degrees * mask_rect.size.x * 0.5
		var x_holder := (in_compass.affine_inverse() * Vector2(x_compass, in_compass.origin.y)).x
		var marker_local := Transform2D(0.0, Vector2(x_holder, 0.0))
		var clip := ((in_compass * marker_local).affine_inverse() * mask_rect).abs()
		UiDraw.character(canvas, SWF, marker_id, compass_xform * in_compass * marker_local, Color.WHITE, null,
			clip, locations + int(marker["type"]) - 1)


## A named instance of `sprite` at `frame`: its display entry, empty if none.
func _named(sprite: int, frame: int, name: String) -> Dictionary:
	for entry: Dictionary in UiAssets.display_list(SWF, sprite, maxi(frame, 0)):
		if entry.get("name", "") == name:
			return entry
	return {}
