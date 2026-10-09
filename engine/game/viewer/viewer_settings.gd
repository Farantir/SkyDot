# SPDX-License-Identifier: GPL-3.0-or-later
#
# The viewer's options (see the header of cell_viewer.gd), read once from the
# command line, or from a shot's JSON with --from-shot, into typed fields.
# Nothing here changes while the viewer runs; what keys change (radius, LOD
# detail, preloading) is copied into the components that own it.
class_name ViewerSettings
extends RefCounted

const SHOT_FORMAT := 1  # "format" of the JSON a shot writes (ShotRecorder)
const MSAA_STEPS := [Viewport.MSAA_DISABLED, Viewport.MSAA_2X, Viewport.MSAA_4X, Viewport.MSAA_8X]
## Options without a value: the next token is another option, not theirs.
const FLAGS := ["no-input"]
const MSAA_NAMES := ["off", "2", "4", "8"]
## --aa: screen-space anti-aliasing after the frame is drawn (not temporal).
const AA_STEPS := [Viewport.SCREEN_SPACE_AA_DISABLED, Viewport.SCREEN_SPACE_AA_FXAA, Viewport.SCREEN_SPACE_AA_SMAA]
const AA_NAMES := ["off", "fxaa", "smaa"]

## Why the options cannot be used; empty when they can.
var error := ""
## --from-shot: the note stored with the shot, to print.
var shot_note := ""

# Where to start.
var pack := ""
var cell := ""  # an interior, by editor id
var world := ""  # a worldspace, by editor id
var has_at := false
var at := Vector3.ZERO  # game units, as `bethconv cell` prints them
var has_target := false
var target := Vector3.ZERO  # game units
var has_look := false
var look_yaw := 0.0  # radians, engine terms
var look_pitch := 0.0

# What is built.
var materials := true
var effects := true
var grass := true
var all_light_shadows := false  # every placed light casts shadows
var collision := true
var navigation := true
var actors := true
var wander := true
var ai := true
var has_tiling := false
var tiling := 0.0  # land texture repeats per cell
var lod := true
var large_refs := true  # large references as models beyond the cells in full
var large_ref_radius := 5  # cells (the game's uLargeRefLODGridSize 11 is radius 5)
var has_tree_distance := false
var tree_distance := 0.0
var shadows := true  # the sun's, outside
var image_space := true
var neutral_grade := false  # --image-space neutral
var volumetric := true  # the weather's volumetric lighting haze
var quests := true
var set_stage := ""  # EDID:STAGE[,EDID:STAGE...]

# Time and weather.
var time := 12.0  # hour to start at
var time_scale := 20.0  # game seconds per second
var weather := ""  # keep this one; empty: they take turns

# The view.
var radius := 2  # cells around the camera built in full
var lod_split := 1.5
var msaa_index := 2  # into MSAA_STEPS: 4x
var aa_index := 0  # into AA_STEPS
var foliage_bias := 0.5  # mip levels blurrier for alpha-tested trees and grass
var render_scale := 1.0  # of the window; above 1 draws bigger and averages down (supersampling)
var has_fov := false
var fov := 75.0  # degrees, vertical
var fly := false  # starts flying through everything
var build_budget_usec := 8000  # per frame for streaming cells in

# Doors.
var pick_locks := false
var preload_doors := true  # build the place behind a near load door ahead
var preload_distance := 15.0  # metres

# Runs that drive themselves.
var activate: Array[int] = []  # references to activate in turn, then quit
var has_activate := false
var save_to := ""  # saves when the --activate run ends
var load_path := ""  # loads at start
var has_screenshot := false
var screenshot := ""
var shot_delay := 0.0  # seconds the world runs before --screenshot captures
var shot_frames := 1  # --screenshot of one view: consecutive frames to save (an effect over time)
var shot_every := 1  # of those, every this many rendered frames
var hide_refs: Array[int] = []  # --screenshot: references (form IDs) hidden in the shots
var has_benchmark := false
var benchmark := 0.0  # seconds
var fly_speed := 20.0  # m/s during the benchmark
var captures := false  # screenshot or benchmark: still time, still actors
var interactive := true  # false: ignore the keyboard and mouse

# F12.
var shot_dir := "user://screenshots"
var shot_notes := true  # ask what is wrong after a shot

var has_pck := false  # given, though ignored


## The settings for `list` (the user arguments). `error` says if they cannot
## be used.
static func from_arguments(list: PackedStringArray) -> ViewerSettings:
	var settings := ViewerSettings.new()
	var args := _split(list)
	if args.has("from-shot"):
		var shot := _arguments_from_shot(args["from-shot"])
		if shot.is_empty():
			settings.error = "cannot read the shot " + args["from-shot"]
			return settings
		settings.shot_note = shot.get("note", "")
		shot.erase("note")
		shot.merge(args, true)  # the command line wins
		args = shot
	settings._read(args)
	return settings


func _read(args: Dictionary) -> void:
	time = args.get("time", "12").to_float()
	if not args.has("pack") or not (args.has("cell") or args.has("world")):
		error = "usage: -- --pack DIR (--cell EDITOR_ID | --world EDITOR_ID --at X,Y,Z)" \
			+ " [--screenshot out.png]"
		return
	pack = args["pack"]
	cell = args.get("cell", "")
	world = args.get("world", "")
	has_pck = args.has("pck")
	has_screenshot = args.has("screenshot")
	screenshot = args.get("screenshot", "")
	has_benchmark = args.has("benchmark")
	benchmark = args.get("benchmark", "0").to_float()
	captures = has_screenshot or has_benchmark
	has_activate = args.has("activate")
	if has_activate:
		for id in args["activate"].split(","):
			activate.append(id.hex_to_int() if id.begins_with("0x") else id.to_int())
	interactive = not (captures or has_activate or args.has("no-input"))

	materials = args.get("materials", "on") != "off"
	effects = args.get("effects", "on") != "off"
	grass = args.get("grass", "on") != "off"
	all_light_shadows = args.get("light-shadows", "game") == "all"
	collision = args.get("collision", "on") != "off"
	navigation = args.get("navigation", "on") != "off"
	actors = args.get("actors", "on") != "off"
	wander = args.get("wander", "off" if captures else "on") != "off"
	ai = args.get("ai", "on") != "off"
	has_tiling = args.has("tiling")
	tiling = args.get("tiling", "0").to_float()
	lod = args.get("lod", "on") != "off"
	large_refs = args.get("large-refs", "on") != "off"
	large_ref_radius = args.get("large-ref-radius", "5").to_int()
	has_tree_distance = args.has("tree-distance")
	tree_distance = args.get("tree-distance", "0").to_float()
	shadows = args.get("shadows", "on") != "off"
	image_space = args.get("image-space", "on") != "off"
	neutral_grade = args.get("image-space", "on") == "neutral"
	volumetric = args.get("volumetric", "on") != "off"
	quests = args.get("quests", "on") != "off"
	set_stage = args.get("set-stage", "")

	time_scale = args.get("time-scale", "20").to_float()
	weather = args.get("weather", "")

	radius = args.get("radius", "2").to_int()
	lod_split = args.get("lod-split", "1.5").to_float()
	msaa_index = maxi(MSAA_NAMES.find(args.get("msaa", "4")), 0)
	aa_index = maxi(AA_NAMES.find(args.get("aa", "off")), 0)
	foliage_bias = clampf(args.get("foliage-bias", "0.5").to_float(), 0.0, 4.0)
	render_scale = clampf(args.get("render-scale", "1").to_float(), 1.0, 2.0)
	has_fov = args.has("fov")
	fov = args.get("fov", "75").to_float()
	fly = args.get("walk", "on") == "off" or not collision or captures
	build_budget_usec = args.get("build-budget", "8000").to_int()

	pick_locks = args.get("pick-locks", "off") != "off"
	preload_doors = args.get("preload-doors", "off" if captures else "on") != "off"
	preload_distance = args.get("preload-distance", "15").to_float()

	save_to = args.get("save-to", "")
	load_path = args.get("load", "")
	shot_delay = args.get("shot-delay", "0").to_float()
	shot_frames = maxi(args.get("shot-frames", "1").to_int(), 1)
	shot_every = maxi(args.get("shot-every", "1").to_int(), 1)
	for id in args.get("hide-refs", "").split(",", false):
		hide_refs.append(id.hex_to_int())
	fly_speed = args.get("fly-speed", "20").to_float()
	shot_dir = args.get("shot-dir", "user://screenshots")
	shot_notes = args.get("shot-notes", "on") != "off"

	has_at = args.has("at")
	if has_at:
		var value = _vec3(args["at"])
		if value == null:
			error = "--at wants X,Y,Z, got " + args["at"]
			return
		at = value
	has_target = args.has("target")
	if has_target:
		var value = _vec3(args["target"])
		if value == null:
			error = "--target wants X,Y,Z, got " + args["target"]
			return
		target = value
	if args.has("look"):
		has_look = true
		var parts: PackedStringArray = args["look"].split(",")
		look_yaw = -deg_to_rad(parts[0].to_float())
		look_pitch = -deg_to_rad(parts[1].to_float()) if parts.size() > 1 else 0.0


## "X,Y,Z" as a Vector3, or null if it is not three numbers.
static func _vec3(text: String) -> Variant:
	var parts := text.split(",")
	if parts.size() < 3:
		return null
	return Vector3(parts[0].to_float(), parts[1].to_float(), parts[2].to_float())


## `--key value` pairs; a key without a value is skipped.
static func _split(list: PackedStringArray) -> Dictionary:
	var out := {}
	var i := 0
	while i < list.size():
		var key := list[i]
		if key.begins_with("--") and key.substr(2) in FLAGS:
			out[key.substr(2)] = ""
			i += 1
		elif key.begins_with("--") and i + 1 < list.size():
			out[key.substr(2)] = list[i + 1]
			i += 2
		else:
			i += 1
	return out


## Viewer arguments that come back to a shot's view (--from-shot), with time
## stopped there. {} if the file is not a shot.
static func _arguments_from_shot(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {}
	var shot = JSON.parse_string(FileAccess.get_file_as_string(path))
	if not shot is Dictionary or int(shot.get("format", 0)) != SHOT_FORMAT:
		return {}
	var out := {}
	var pack_path: String = shot.get("pack", {}).get("path", "")
	if not pack_path.is_empty():
		out["pack"] = pack_path
	var place: Dictionary = shot.get("place", {})
	var game: Dictionary = shot.get("camera", {}).get("game", {})
	if place.get("kind") == "exterior":
		out["world"] = place.get("world", "")
	else:
		out["cell"] = place.get("cell", "")
	if not game.is_empty():
		out["at"] = "%f,%f,%f" % [game["x"], game["y"], game["z"]]
		out["look"] = "%f,%f" % [game["heading"], game["tilt"]]
	var time_state: Dictionary = shot.get("time", {})
	if time_state.has("hour"):
		out["time"] = str(time_state["hour"])
	out["time-scale"] = "0"
	var weather_state: Dictionary = shot.get("weather", {})
	if not str(weather_state.get("editor_id", "")).is_empty():
		out["weather"] = weather_state["editor_id"]
	var viewer: Dictionary = shot.get("viewer", {})
	if viewer.get("flying", false):
		out["walk"] = "off"
	if viewer.has("radius"):
		out["radius"] = str(viewer["radius"])
	if viewer.has("large_refs"):
		out["large-refs"] = "on" if viewer["large_refs"] else "off"
	if viewer.has("large_ref_radius"):
		out["large-ref-radius"] = str(viewer["large_ref_radius"])
	if viewer.has("lod_split"):
		out["lod-split"] = str(viewer["lod_split"])
	if viewer.has("msaa"):
		out["msaa"] = str(viewer["msaa"])
	if viewer.has("aa"):
		out["aa"] = str(viewer["aa"])
	if viewer.has("foliage_bias"):
		out["foliage-bias"] = str(viewer["foliage_bias"])
	if viewer.has("render_scale"):
		out["render-scale"] = str(viewer["render_scale"])
	if viewer.get("quests", true) == false:
		out["quests"] = "off"
	if viewer.get("materials", true) == false:
		out["materials"] = "off"
	if not str(shot.get("note", "")).is_empty():
		out["note"] = shot["note"]
	return out
