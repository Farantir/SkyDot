# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shots. F12 saves the frame as a PNG and, next to it, a JSON file with what
# is needed to get the same view again (--from-shot reads it back): place,
# camera in engine and game terms, time, weather, viewer options, pack hashes,
# GPU and the game console commands for the same spot. Shift+F12 leaves out
# the text overlay. After the capture the viewer pauses and asks what is
# wrong; Enter stores the text as the JSON's "note".
# --screenshot FILE takes its shots on its own (FILE_0.png, ...) and quits.
class_name ShotRecorder
extends RefCounted

const SETTLE_FRAMES := 30  # for shaders to compile and textures to stream in
const LOOK_PITCH := -0.15  # the four views of a cell, slightly down

var _host: Node3D
var _settings: ViewerSettings
var _world: SkydotWorld
var _rig: PlayerRig
var _place: Place
var _streamer: SkydotStreamer
var _clock: SkydotClock
var _debug: DebugOverlay  # notes; the text F12 may hide; the note dialog goes in it
# --screenshot
var _path := ""
var _views: Array[float] = []  # yaw of each shot still to take; NAN keeps the view
var _delay := 0.0  # seconds the world runs before the first shot
var _frames := 0
var _index := 0
# F12
var _busy := false
var _dialog: PanelContainer  # asks for a shot's note
var _note_text: TextEdit
var _note_json := ""  # the shot the note goes to
var _note_mouse := Input.MOUSE_MODE_VISIBLE  # restored afterwards


func _init(host: Node3D, settings: ViewerSettings, world: SkydotWorld, rig: PlayerRig, place: Place,
		streamer: SkydotStreamer, clock: SkydotClock, debug: DebugOverlay) -> void:
	_host = host
	_settings = settings
	_world = world
	_rig = rig
	_place = place
	_streamer = streamer
	_clock = clock
	_debug = debug
	_delay = settings.shot_delay


## --screenshot: take a shot of each yaw (NAN: of the view as it is), then quit.
func begin_run(path: String, yaws: Array[float]) -> void:
	_path = path
	_views = yaws
	if not is_nan(_views[0]):
		_rig.apply_look(_views[0], LOOK_PITCH)


## True while --screenshot shots are being taken; the viewer then does nothing
## but call step.
func is_running() -> bool:
	return _path != ""


## Every frame of a --screenshot run.
func step(delta: float) -> void:
	if _delay > 0.0:
		_delay -= delta
		return
	# Let shaders compile and textures stream in before capturing.
	_frames += 1
	if _frames < SETTLE_FRAMES:
		return
	var index := _index
	_index += 1
	var image := _host.get_viewport().get_texture().get_image()
	var path := _path.get_basename() + "_%d.png" % index
	image.save_png(path)
	print("screenshot: ", path)
	_views.pop_front()
	if _views.is_empty():
		_host.get_tree().quit(0)
		return
	if not is_nan(_views[0]):
		_rig.apply_look(_views[0], LOOK_PITCH)
	_frames = 20  # the next shot waits ten frames


## F12: the frame as a PNG and a JSON file describing it. The PNG is written
## on a worker thread so the frame does not hitch.
func capture(hide_overlay: bool) -> void:
	if _busy:
		return
	_busy = true
	if hide_overlay:
		_debug.visible = false
		await RenderingServer.frame_post_draw
	var image := _host.get_viewport().get_texture().get_image()
	_debug.visible = true
	var dir := _settings.shot_dir
	DirAccess.make_dir_recursive_absolute(dir)
	var stem := dir.path_join("shot_" + Time.get_datetime_string_from_system().replace(":", "").replace("-", "").replace("T", "_"))
	var base := stem
	var n := 2
	while FileAccess.file_exists(base + ".png") or FileAccess.file_exists(base + ".json"):
		base = "%s_%d" % [stem, n]
		n += 1
	var meta := _shot_metadata(hide_overlay)
	meta["image"] = (base + ".png").get_file()
	var file := FileAccess.open(base + ".json", FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(meta, "  ") + "\n")
		file.close()
	var png := base + ".png"
	if image == null:  # headless: nothing is rendered, the JSON still helps
		_debug.note("screenshot: no image here, wrote " + ProjectSettings.globalize_path(base + ".json"))
		_busy = false
		return
	WorkerThreadPool.add_task(func() -> void: image.save_png(png))
	_debug.note("screenshot: " + ProjectSettings.globalize_path(png))
	if not _settings.shot_notes or DisplayServer.get_name() == "headless":
		_busy = false
		return
	_ask_note(base + ".json")


## Pause and ask what the shot shows; `_end_note` stores the answer.
func _ask_note(json_path: String) -> void:
	if _dialog == null:
		_dialog = PanelContainer.new()
		_dialog.process_mode = Node.PROCESS_MODE_ALWAYS
		_dialog.custom_minimum_size = Vector2(640, 0)
		var box := VBoxContainer.new()
		var label := Label.new()
		label.text = "What is wrong in this shot? Enter saves, Shift+Enter new line, Escape skips."
		box.add_child(label)
		_note_text = TextEdit.new()
		_note_text.custom_minimum_size = Vector2(0, 96)
		_note_text.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
		_note_text.gui_input.connect(_note_input)
		box.add_child(_note_text)
		_dialog.add_child(box)
		_debug.add_child(_dialog)
		_dialog.set_anchors_and_offsets_preset(Control.PRESET_CENTER_BOTTOM,
			Control.PRESET_MODE_MINSIZE, 24)
		_dialog.grow_horizontal = Control.GROW_DIRECTION_BOTH
		_dialog.grow_vertical = Control.GROW_DIRECTION_BEGIN
	_note_json = json_path
	_note_mouse = Input.mouse_mode
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	_host.get_tree().paused = true  # time, weather and the player wait
	_note_text.text = ""
	_dialog.visible = true
	_note_text.grab_focus()


func _note_input(event: InputEvent) -> void:
	if not (event is InputEventKey and event.pressed and not event.echo):
		return
	if event.keycode == KEY_ESCAPE:
		_end_note(false)
	elif event.keycode in [KEY_ENTER, KEY_KP_ENTER] and not event.shift_pressed:
		_end_note(true)
	else:
		return
	_note_text.accept_event()


func _end_note(save: bool) -> void:
	var text := _note_text.text.strip_edges()
	if save and not text.is_empty():
		var meta = JSON.parse_string(FileAccess.get_file_as_string(_note_json))
		var file := FileAccess.open(_note_json, FileAccess.WRITE) if meta is Dictionary else null
		if file != null:
			meta["note"] = text
			file.store_string(JSON.stringify(meta, "  ") + "\n")
			file.close()
			_debug.note("note saved")
		else:
			_debug.note("note not saved: cannot write " + ProjectSettings.globalize_path(_note_json))
	_dialog.visible = false
	_note_text.release_focus()
	_host.get_tree().paused = false
	Input.mouse_mode = _note_mouse
	_busy = false


## Everything needed to come back to this view, in the viewer or the game.
func _shot_metadata(overlay_hidden: bool) -> Dictionary:
	var eye := SkydotWorld.godot_to_skyrim(_rig.camera.global_position)
	var heading := fposmod(-rad_to_deg(_rig.yaw), 360.0)
	var tilt := -rad_to_deg(_rig.pitch)
	var place := {}
	var console: Array[String] = []
	if _streamer.world_id != 0:
		var world_name := ""
		for w in _world.list_worlds():
			if w["id"] == _streamer.world_id:
				world_name = w["editor_id"]
		var grid := Vector2i(floori(eye.x / SkydotWorld.CELL_UNITS), floori(eye.y / SkydotWorld.CELL_UNITS))
		place = {"kind": "exterior", "world": world_name, "world_id": "0x%08X" % _streamer.world_id,
			"grid": [grid.x, grid.y]}
		console.append("cow %s %d %d" % [world_name, grid.x, grid.y])
	else:
		var cell := _world.get_cell(_place.cell_id)
		place = {"kind": "interior", "cell": cell.get("editor_id", ""), "cell_id": "0x%08X" % _place.cell_id}
		console.append("coc " + str(cell.get("editor_id", "")))
	# The game's getpos is at the feet; the first-person eye is about 120
	# units higher (GAME-COMPARISON.md).
	console.append_array(["player.setpos x %.0f" % eye.x, "player.setpos y %.0f" % eye.y,
		"player.setpos z %.0f" % (eye.z - 120.0), "player.setangle z %.0f" % heading,
		"player.setangle x %.0f" % tilt])
	var weather := {}
	if _clock.has_weather():
		var running := _clock.weather
		var state := running.get_state()
		weather = {"id": "0x%08X" % int(state.get("weather", 0)), "editor_id": state.get("editor_id", ""),
			"transition": state.get("transition", 1.0), "auto": running.auto_weather}
		console.append("set gamehour to %.2f" % running.hour)
		if int(state.get("weather", 0)) != 0:
			console.append("sw %X" % int(state["weather"]))
	console.append("tm")
	var pack_dir := _settings.pack
	var manifest = JSON.parse_string(FileAccess.get_file_as_string(pack_dir.path_join("manifest.json")))
	var pack := {"path": ProjectSettings.globalize_path(pack_dir)}
	if manifest is Dictionary:
		pack["converter"] = manifest.get("converter", "")
		pack["world_hash"] = manifest.get("world", {}).get("hash", "")
		pack["input"] = manifest.get("input", {})
	var size := _host.get_viewport().get_visible_rect().size
	return {
		"format": ViewerSettings.SHOT_FORMAT,
		"taken": Time.get_datetime_string_from_system(),
		"place": place,
		"camera": {
			"game": {"x": eye.x, "y": eye.y, "z": eye.z, "heading": heading, "tilt": tilt},
			"engine": {"position": [_rig.camera.global_position.x, _rig.camera.global_position.y,
				_rig.camera.global_position.z], "yaw": _rig.yaw, "pitch": _rig.pitch},
			"fov": _rig.camera.fov,
			"resolution": [int(size.x), int(size.y)],
		},
		"time": _clock.describe(),
		"weather": weather,
		"viewer": {
			"flying": _rig.player.fly,
			"materials": _world.skyrim_materials,
			"effects": _world.effects,
			"collision": _world.collision,
			"navigation": _world.navigation,
			"actors": _world.actors,
			"wander": _world.actor_wander,
			"navmesh_shown": _debug.show_navmesh,
			"lod": _streamer.lod != null,
			"radius": _streamer.radius,
			"lod_split": _streamer.lod_split,
			"large_refs": _streamer.large_refs,
			"large_ref_radius": _streamer.large_ref_radius,
			"msaa": ViewerSettings.MSAA_NAMES[maxi(ViewerSettings.MSAA_STEPS.find(_host.get_viewport().msaa_3d), 0)],
			"quests": _settings.quests,
			"overlay_hidden": overlay_hidden,
		},
		"pack": pack,
		"system": {
			"godot": Engine.get_version_info()["string"],
			"os": OS.get_name(),
			"renderer": RenderingServer.get_current_rendering_method(),
			"gpu": RenderingServer.get_video_adapter_name(),
			"gpu_vendor": RenderingServer.get_video_adapter_vendor(),
			"driver": RenderingServer.get_video_adapter_api_version(),
		},
		"game_console": console,
	}
