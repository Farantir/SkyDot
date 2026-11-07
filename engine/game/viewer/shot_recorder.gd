# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shots. F12 saves the frame as a PNG and, next to it, a JSON file with what
# is needed to get the same view again (--from-shot reads it back): place,
# camera in engine and game terms, time, weather, viewer options, pack hashes,
# GPU and the game console commands for the same spot. Shift+F12 leaves out
# the text overlay. After the capture the viewer pauses and asks what is
# wrong; Enter stores the text as the JSON's "note" and, for skycompare, a
# "capture" spec: kind, seconds, trigger and category from the dialog's row
# (the category is remembered for the session) and an optional region dragged
# on the frozen frame (right click clears it). The trigger list has the
# player's actions (cast:/attack:, with a hold: auto, tap or seconds; see
# CaptureTrigger), which use what the JSON's "player" block says the hands
# and the voice slot held at F12 (`player_block`).
# --dump FILE writes the state dump (state_dump.gd) just before the first shot.
# --screenshot FILE takes its shots on its own (FILE_0.png, ...) and quits.
# With --shot-frames N (one view) it saves N frames instead, every
# --shot-every rendered frames (FILE_0000.png, ...), to follow an effect over
# time; run godot with --fixed-fps so a frame is the same time step as the
# game's capture (SkyrimRemote's @screenshot NAME every K count N).
class_name ShotRecorder
extends RefCounted

const SETTLE_FRAMES := 30  # for shaders to compile and textures to stream in
const LOOK_PITCH := -0.15  # the four views of a cell, slightly down
const CAPTURE_KINDS: Array[String] = ["still", "anim", "sound", "anim+sound"]
const CAPTURE_CATEGORIES: Array[String] = ["lighting", "geometry", "fx", "physics", "actors", "ui", "audio", "other"]
## Trigger presets of skycompare; one ending in ":" takes the text of the field next to the list. The
## player's actions (cast:, attack:) use what the hands and the voice slot hold (the shot's "player" block).
const CAPTURE_TRIGGERS: Array[String] = ["none", "cast:right", "cast:left", "cast:dual", "cast:shout", "attack:right",
	"attack:left", "attack:dual", "attack:bash", "idle:", "activate:nearest-door", "time:", "weather:"]
## How long a player trigger's button is held ("hold" of the capture spec: auto, tap or seconds).
const CAPTURE_HOLDS: Array[String] = ["auto", "tap", "0.5", "1", "1.5", "2", "3", "5"]

static var _last_category := CAPTURE_CATEGORIES.size() - 1  # remembered for the session

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
var session: GameSession  # set by the viewer: the --trigger acts through it
var _dumped := false
var _triggered := false
var _player_trigger: CaptureTrigger  # --trigger cast:/attack:, made at the first frame of the series
var _pending: Array[int] = []  # --shot-frames: PNG writes still running
# F12
var _busy := false
var _dialog: PanelContainer  # asks for a shot's note
var _note_text: TextEdit
var _note_json := ""  # the shot the note goes to
var _note_mouse := Input.MOUSE_MODE_VISIBLE  # restored afterwards
var _kind: OptionButton
var _seconds: SpinBox
var _trigger: OptionButton
var _trigger_arg: LineEdit
var _hold: OptionButton
var _category: OptionButton
var _region_layer: Control  # covers the frozen frame; the region is dragged on it
var _region := Rect2()  # in viewport pixels; size zero = whole frame
var _drag_from := Vector2.ZERO
var _dragging := false


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
		if _frames == SETTLE_FRAMES - 1:
			_debug.refresh_perf()  # drawn by the time of the shot, even if under a second passed
		if _frames == SETTLE_FRAMES - 1 and not _settings.hide_refs.is_empty():
			_hide_refs(_host)
		return
	if not _settings.dump.is_empty() and not _dumped:
		_dumped = true
		var dump := StateDump.build(_host, _world, _rig.camera.global_position, _settings.dump_radius)
		if StateDump.write(_settings.dump, dump):
			print("dump: ", _settings.dump, " ", dump["refs"].size(), " refs ", dump["lights"].size(), " lights ",
				dump["effects"].size(), " effects")
		else:
			printerr("cannot write the dump ", _settings.dump)
	if _views.size() == 1 and _settings.shot_frames > 1:
		_step_series()
		return
	var index := _index
	_index += 1
	print("perf: ", JSON.stringify(_debug.perf))
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


## --shot-frames: one frame of the series every --shot-every frames, then quit.
## The PNGs are written on worker threads so the frame rate stays even.
func _step_series() -> void:
	if _index == 0 and not _triggered:
		if not _trigger_ready():
			return
		_triggered = true
		_fire_trigger()
	if _player_trigger != null:
		_player_trigger.step(_host.get_process_delta_time())
	if (_frames - SETTLE_FRAMES) % _settings.shot_every != 0:
		return
	var image := _host.get_viewport().get_texture().get_image()
	var path := _path.get_basename() + "_%04d.png" % _index
	_index += 1
	_pending.append(WorkerThreadPool.add_task(func() -> void: image.save_png(path)))
	if _index < _settings.shot_frames:
		return
	for task in _pending:
		WorkerThreadPool.wait_for_task_completion(task)
	print("screenshots: %d frames, %s_0000.png .. %s" % [_index, _path.get_basename(), path.get_file()])
	_host.get_tree().quit(0)


## A player trigger (CaptureTrigger) waits for the hands to be drawn (--ready-weapon draws them from the
## start); the series starts then.
func _trigger_ready() -> bool:
	if not CaptureTrigger.handles(_settings.trigger) or session == null:
		return true
	if _player_trigger == null:
		_player_trigger = CaptureTrigger.new(session, _settings.trigger, _settings.hold, _settings.capture_seconds)
	return _player_trigger.is_ready()


func _write_frames_json(trigger_frame: int) -> void:
	var file := FileAccess.open(_path.get_base_dir().path_join("frames.json"), FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify({"trigger_frame": trigger_frame, "fps": int(ViewerSettings.CAPTURE_FPS),
			"movie_frame": Engine.get_frames_drawn(), "trigger": _settings.trigger}) + "\n")
		file.close()


## --trigger, at the first frame of a --capture-seconds series; frames.json (next to the frames) says
## which frame it was (the series starts there) and the frame rate.
func _fire_trigger() -> void:
	var spec := _settings.trigger
	if _player_trigger != null:
		print("--trigger: ", _player_trigger.fire())
	elif spec.begins_with("activate:") and session != null:
		var ref := 0
		if spec != "activate:nearest-door":
			ref = spec.trim_prefix("activate:").hex_to_int()
		else:
			ref = StateDump.nearest_door(StateDump.build(_host, _world, _rig.camera.global_position, _settings.dump_radius))
		print("--trigger: activate 0x%08X: %s" % [ref, session.activate_ref(ref, true)])
	elif spec != "none":
		printerr("--trigger: unknown or unsupported preset ", spec)
	_write_frames_json(0)


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
		box.add_child(_capture_row())
		var hint := Label.new()
		hint.text = "Drag on the picture to mark the part to compare, right click clears it (none = whole frame)."
		box.add_child(hint)
		_dialog.add_child(box)
		_region_layer = Control.new()
		_region_layer.process_mode = Node.PROCESS_MODE_ALWAYS
		_region_layer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		_region_layer.mouse_filter = Control.MOUSE_FILTER_STOP
		_region_layer.gui_input.connect(_region_input)
		_region_layer.draw.connect(_region_draw)
		_debug.add_child(_region_layer)
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
	_region = Rect2()
	_dragging = false
	_category.select(_last_category)
	_dialog.visible = true
	_region_layer.visible = true
	_region_layer.queue_redraw()
	_note_text.grab_focus()


## kind, seconds, trigger and category of the capture spec.
func _capture_row() -> HBoxContainer:
	var row := HBoxContainer.new()
	_kind = _options(CAPTURE_KINDS, "what skycompare captures")
	_seconds = SpinBox.new()
	_seconds.min_value = 1
	_seconds.max_value = 30
	_seconds.value = 3
	_seconds.suffix = " s"
	_seconds.tooltip_text = "seconds of an animation or sound"
	_trigger = _options(CAPTURE_TRIGGERS, "what starts the animation or sound")
	_trigger_arg = LineEdit.new()
	_trigger_arg.placeholder_text = "idle, hour or weather"
	_trigger_arg.custom_minimum_size = Vector2(150, 0)
	_trigger.item_selected.connect(func(index: int) -> void: _trigger_arg.editable = CAPTURE_TRIGGERS[index].ends_with(":"))
	_trigger_arg.editable = false
	_hold = _options(CAPTURE_HOLDS, "how long a cast or attack button is held: auto (a concentration spell and a "
		+ "block for the capture, a charged spell until charged, else a tap), tap, or seconds (a charge, a power "
		+ "attack, more words of a shout)")
	_category = _options(CAPTURE_CATEGORIES, "what the comparison is about")
	for control in [_kind, _seconds, _trigger, _trigger_arg, _hold, _category]:
		row.add_child(control)
		control.focus_mode = Control.FOCUS_CLICK  # Tab stays in the note
	return row


func _options(items: Array[String], tip: String) -> OptionButton:
	var button := OptionButton.new()
	for item in items:
		button.add_item(item)
	button.tooltip_text = tip
	return button


## The "capture" key of the shot's JSON, from the dialog.
func _capture_spec() -> Dictionary:
	var trigger := CAPTURE_TRIGGERS[_trigger.selected]
	if trigger.ends_with(":"):
		var arg := _trigger_arg.text.strip_edges()
		trigger = trigger + arg if not arg.is_empty() else "none"
	var spec := {"kind": CAPTURE_KINDS[_kind.selected], "seconds": int(_seconds.value), "trigger": trigger,
		"category": CAPTURE_CATEGORIES[_category.selected]}
	if CaptureTrigger.handles(trigger):
		var hold := CAPTURE_HOLDS[_hold.selected]
		spec["hold"] = hold if hold in ["auto", "tap"] else hold.to_float()
	if _region.has_area():
		spec["region"] = [int(_region.position.x), int(_region.position.y), int(_region.end.x), int(_region.end.y)]
	return spec


func _region_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT:
			_dragging = event.pressed
			if event.pressed:
				_drag_from = event.position
				_region = Rect2(event.position, Vector2.ZERO)
		elif event.button_index == MOUSE_BUTTON_RIGHT and event.pressed:
			_region = Rect2()
			_dragging = false
		_region_layer.queue_redraw()
	elif event is InputEventMouseMotion and _dragging:
		var size := _host.get_viewport().get_visible_rect().size
		var to: Vector2 = event.position.clamp(Vector2.ZERO, size)
		_region = Rect2(_drag_from.min(to), (to - _drag_from).abs())
		_region_layer.queue_redraw()


func _region_draw() -> void:
	if _region.has_area():
		_region_layer.draw_rect(_region, Color(1, 0.85, 0.2, 0.15), true)
		_region_layer.draw_rect(_region, Color(1, 0.85, 0.2), false, 2.0)


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
	if save:
		_last_category = _category.selected
		var meta = JSON.parse_string(FileAccess.get_file_as_string(_note_json))
		var file := FileAccess.open(_note_json, FileAccess.WRITE) if meta is Dictionary else null
		if file != null:
			if not text.is_empty():
				meta["note"] = text
			meta["capture"] = _capture_spec()
			file.store_string(JSON.stringify(meta, "  ") + "\n")
			file.close()
			_debug.note("note saved")
		else:
			_debug.note("note not saved: cannot write " + ProjectSettings.globalize_path(_note_json))
	_dialog.visible = false
	_region_layer.visible = false
	_note_text.release_focus()
	_host.get_tree().paused = false
	Input.mouse_mode = _note_mouse
	_busy = false


## Everything needed to come back to this view, in the viewer or the game.
func _perf_block() -> Dictionary:
	var p: Dictionary = _debug.perf
	return {"fps": p.get("fps", 0.0), "frame_ms_mean": p.get("frame_ms_mean", 0.0),
		"frame_ms_max": p.get("frame_ms_max", 0.0), "draw_calls": p.get("draw_calls", 0),
		"primitives": p.get("primitives", 0), "render_cpu_ms": p.get("render_cpu_ms", 0.0),
		"gpu_ms": p.get("gpu_ms", 0.0), "physics_ms": p.get("physics_ms", 0.0)}


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
			"foliage_bias": _world.foliage_bias,
			"render_scale": _host.get_viewport().scaling_3d_scale,
			"aa": ViewerSettings.AA_NAMES[maxi(ViewerSettings.AA_STEPS.find(_host.get_viewport().screen_space_aa), 0)],
			"quests": _settings.quests,
			"overlay_hidden": overlay_hidden,
			"perf_overlay": _debug.show_perf,
			"perf": _perf_block(),
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
		"player": player_block(session),
	}


## A shot's "player" block: what the hands and the voice slot hold ({form, kind}; kind spell, weapon, shield,
## torch or empty; the voice: shout with its unlocked words, power or empty), whether the weapon or the
## hands are drawn, and what the player is doing. skycompare makes the same in both engines before a
## trigger (ViewerSettings reads it back as start options). {} without a game.
static func player_block(game: GameSession) -> Dictionary:
	if game == null:
		return {}
	var magic := game.magic
	var gear := game.equipment()
	var block := {}
	for hand in [Magic.RIGHT, Magic.LEFT]:
		var side := "right" if hand == Magic.RIGHT else "left"
		var spell := magic.spell_in(hand)
		var item := int(gear.get(side, 0))
		var kind := "empty"
		if spell != 0:
			kind = "spell"
			item = spell
		elif item != 0:
			match String(game.papyrus.item_info(item).get("category", "")):
				"weapon":
					kind = "weapon"
				"armor":
					kind = "shield"
				"light":
					kind = "torch"
				_:
					kind = "other"
		block[side] = {"form": "0x%08X" % item, "kind": kind}
	var shout := int(game.papyrus.equipped_shout(SkydotPapyrus.PLAYER_REF))
	var power := magic.spell_in(Magic.POWER)
	if shout != 0:
		var words: Array[String] = []
		for word: Dictionary in game.papyrus.shout_info(SkydotPapyrus.PLAYER_REF, shout).get("words", []):
			if word.get("known", false) and word.get("unlocked", false):
				words.append("0x%08X" % int(word["word"]))
		block["voice"] = {"form": "0x%08X" % shout, "kind": "shout", "words": words}
	elif power != 0:
		block["voice"] = {"form": "0x%08X" % power, "kind": "power"}
	else:
		block["voice"] = {"form": "0x00000000", "kind": "empty"}
	block["drawn"] = game.weapon_state() != Combat.State.SHEATHED or magic.is_ready()
	var action := "idle"
	if not game.combat.current_attack().is_empty():
		action = "attacking"
	elif game.combat.is_blocking():
		action = "blocking"
	elif magic.is_casting(Magic.LEFT) or magic.is_casting(Magic.RIGHT):
		action = "casting"
	elif magic.is_shouting():
		action = "shouting"
	block["action"] = action
	return block


## --hide-refs: hides the references with those form IDs. Lights carry no
## skydot_ref; they are named after their reference ("0x00082887 NAME light").
func _hide_refs(node: Node) -> void:
	if node is Node3D and _settings.hide_refs.has(node.get_meta("skydot_ref", 0)):
		node.visible = false
		return
	if node is Light3D and _settings.hide_refs.has(String(node.name).get_slice(" ", 0).hex_to_int()):
		node.visible = false
		return
	for child in node.get_children():
		_hide_refs(child)
