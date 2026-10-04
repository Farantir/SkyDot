# SPDX-License-Identifier: GPL-3.0-or-later
#
# Loads one cell from a pack and shows it with a fly camera, or streams a
# worldspace's exterior cells around the camera. Load doors lead between them.
#
#   godot4.7 --path game res://viewer/cell_viewer.tscn -- \
#       --pack DIR --cell EDITOR_ID [--screenshot out.png] [--materials off]
#   godot4.7 --path game res://viewer/cell_viewer.tscn -- \
#       --pack DIR --world Tamriel --at X,Y,Z [--target X,Y,Z] [--radius 2]
#
# Exteriors keep the cells within --radius of the camera's cell loaded, nearest
# first, and drop those further than one more. Models and textures load from
# the pack on the asset cache's threads; only the build runs here, in steps
# within a frame budget (--build-budget USEC, default 8000).
# --tiling sets land texture repeats per cell. --benchmark SECONDS flies the
# camera east at --fly-speed m/s (default 20) and prints frame times.
# Beyond the loaded cells the worldspace's LOD shows (terrain, objects, tree
# billboards, from the pack's converted LOD); --lod off turns
# it off, --lod-split and --tree-distance tune it (SkydotLod). [ and ] lower
# and raise the LOD's detail while running (--lod-split by a quarter each:
# finer LOD further out); - and = shrink and grow --radius, the full-detail
# cells around the camera (2 is the game's uGridsToLoad 5). --msaa off|2|4|8 smooths edges (multisampling;
# default off); M switches between them.
# --fov DEGREES sets the camera's vertical field of view (default 75; the
# game's is 50.4 at 16:9). --shadows off disables the sun's shadows.
# --light-shadows all gives every placed light shadows, not only those whose
# record asks for them (game, the default). --grass off grows no grass.
# --image-space off shows the scene's numbers ungraded (no image space).
# Outside, SkydotWeather runs time and weather: --time HOURS (default 12) to
# start at, --time-scale (game seconds per second, default 20, 0 stops time),
# --weather EDITOR_ID to keep one weather; otherwise the region's or
# climate's weathers take turns. T and Shift+T move the time by an hour, K
# changes the weather.
#
# Controls: the mouse looks (Esc releases it, a click captures it again; the
# right button looks while released), WASD to move, Shift sprints, Ctrl walks,
# Space jumps (and swims up). The player walks with collision (SkydotPlayer):
# it falls, climbs steps, swims in exterior water and pushes clutter. V
# toggles flying through everything, with Q/E down/up and Shift faster
# (--walk off starts flying; --benchmark and --screenshot always fly;
# --collision off builds no physics bodies and flies). Without
# --at, an interior is entered where its door from outside leads.
# F activates what the camera looks at: its scripts run (SkydotPapyrus), a load
# door leads to its destination, a plain door opens or closes, and references
# whose activate parent it is are activated in turn. Locked doors stay shut;
# both doors of a load door share the lock a plugin stores on one of them.
# Shift+F (or --pick-locks) opens them anyway, and they stay unlocked. Scripts
# attach as cells are built; what they enable, disable, open and animate shows
# up here, and the camera's feet walk through their trigger volumes. F5 saves
# the scripts' state and the place, F9 loads it (user://quicksave.skydot).
# --save-to FILE saves when an --activate run ends; --load FILE loads at start.
# Near a load door (within --preload-distance metres, default 15) the place
# behind it is built ahead, held hidden in the scene (_hold): an interior, or
# the cells and LOD around the arrival spot outside. Going through then takes
# a fraction of the time. Actors are added on arrival, where the AI has them
# then. It costs the memory of a second place; --preload-doors off (or O)
# turns it off. --screenshot and --benchmark runs do not preload.
# Going through a load door fades to black, travels, waits until the new
# place is built (outside: the cells in range and the LOD) and fades back.
# Start-game-enabled quests start with the viewer (--quests off to skip);
# --set-stage EDID:STAGE[,EDID:STAGE...] then sets stages. Quest stages,
# objectives and script notifications show at the top left; J prints the
# running quests with their stage and shown objectives.
# F12 saves a screenshot and, next to it, a JSON file with what is needed to
# get the same view again: place, camera (in engine and game terms), time,
# weather, viewer options, pack, Godot and GPU, and the game console commands
# for the same spot (GAME-COMPARISON.md). Shift+F12 leaves out the text
# overlay. They go to --shot-dir (default user://screenshots). After the
# capture the viewer pauses and asks what is wrong; Enter stores the text as
# the JSON's "note" (Shift+Enter for a new line), Escape keeps the shot
# without one. --shot-notes off skips the question.
# --from-shot FILE.json starts where such a shot was taken, with time
# stopped; arguments given as well win (--pack, or --screenshot to render it
# again and exit).
# --screenshot, --benchmark, --activate and --no-input runs ignore the keyboard
# and mouse.
# --activate 0xREF[,0xREF...] activates those references in turn, each in the
# place the previous one led to, then quits (for tests; works headless).
# N shows the navmeshes (green, water triangles included) of the built cells;
# G asks the navigation map for a path from the feet to the navmesh point the
# camera looks at and draws it (--navigation off builds no navmeshes).
# Placed actors stand dressed in their cells (--actors off builds none) and
# follow their AI packages (SkydotAi): where they are when a place is built
# depends on the time, and they walk to work, home, the inn and to bed,
# through load doors, locking and unlocking them (--ai off: they stay where
# they were placed and wander about it). --wander off keeps them in their
# idle where their packages put them; --screenshot and --benchmark runs keep
# them still unless --wander on; --shot-delay SECONDS lets the world run that
# long before a --screenshot is taken. Inside, time runs too (--time-scale);
# T and Shift+T move it there as well. I tells what the nearest actor's
# package is.
# --at X,Y,Z --target X,Y,Z (Skyrim game units, as `bethconv cell` prints)
# places the camera. With --screenshot, renders four views from the cell's centre
# (out_0.png .. out_3.png, one per 90 degrees of yaw; one view with --at) and
# quits. --materials off
# keeps the importer's standard materials, --effects off leaves models and
# lights still, for comparison.
extends Node3D

var _camera: Camera3D
var _player: SkydotPlayer  # the camera follows its eyes
var _last_ground := Vector3.ZERO  # where the player last stood
var _ground_check := 0  # frames until checking the player is not under the land
var _yaw := 0.0
var _pitch := 0.0
var _speed := 3.0
var _shots: Array = []
var _shot_path := ""
var _shot_delay := 0.0  # seconds the world runs before --screenshot captures
var _frames := 0
var _shot_index := 0

const CELL_UNITS := SkydotWorld.CELL_UNITS  # game units along a cell, as the converter uses
var _unit_scale := SkydotWorld.unit_scale()  # metres per game unit
var _build_budget_usec := 8000  # per frame for streaming cells in
const LOD_BUDGET_USEC := 3000
const EYE_HEIGHT := 1.7
const REACH := 2.6  # metres the camera can activate from
var _args: Dictionary
var _world: SkydotWorld
var _world_id := 0  # the worldspace being streamed, or 0 inside
var _radius := 2
var _loaded := {}  # Vector2i -> Node3D (null if nothing is there)
var _building := {}  # Vector2i -> Node3D built in steps, hidden until done
var _place: Array[Node] = []  # everything the current cell or worldspace added
var _benchmark := 0.0
var _fly_speed := 20.0
var _frame_times: Array[float] = []
var _stream_max_usec := 0  # the slowest streaming step during a benchmark
var _pick_locks := false
var _input := true  # false for runs that drive themselves (_ready)
var _script_activations: Array = []  # --activate: refs still to activate
var _script_wait := 0
var _quit_in := -1  # frames until quitting after --activate
var _papyrus: SkydotPapyrus
var _ai: SkydotAi  # null with --ai off
var _pack: SkydotPack
var _lod: SkydotLod  # the worldspace's LOD, or null
var _lod_split := 1.5  # SkydotLod.split_distance for every LOD made
const MSAA_STEPS := [Viewport.MSAA_DISABLED, Viewport.MSAA_2X, Viewport.MSAA_4X, Viewport.MSAA_8X]
const MSAA_NAMES := ["off", "2", "4", "8"]
var _preload_doors := true  # build the place behind a near load door ahead
var _preload_distance := 15.0  # metres
const PREPARE_BUDGET_USEC := 4000
var _load_doors := {}  # door ref -> its node, in the place shown
var _prepared := {}  # the place behind the nearest load door (_prepare_step)
var _prepare_scan := 0  # frames until looking for the nearest door again
var _streaming := false  # exterior cells in range still loading
var _lod_busy := false  # the LOD still has work queued
const FADE_SECONDS := 0.35
const FADE_SETTLE_FRAMES := 3  # drawn black after the place is built
const FADE_TIMEOUT := 15.0  # seconds; fades in even if streaming never ends
var _fade: ColorRect  # black over everything during a door transition
var _fade_door := {}  # the door being gone through
var _fade_phase := 0  # FADE_*
var _fade_wait := 0.0  # seconds in FADE_WAIT
var _fade_frames := 0  # frames since the place was built
enum { FADE_NONE, FADE_OUT, FADE_TRAVEL, FADE_WAIT, FADE_IN }
var _cell_id := 0  # the interior being shown, or 0 outside
var _notes: Label  # recent quest and script messages
var _journal: Label  # J toggles it
var _note_lines: Array = []  # [text, seconds left]
var _quests_ready := false  # after the start-game quests have started
const NOTE_SECONDS := 8.0
const QUICKSAVE := "user://quicksave.skydot"
var _weather: SkydotWeather  # outside only
var _hour := 12.0  # kept while inside
var _day := 0
var _show_navmesh := false  # N toggles the navmesh overlay
var _path_line: MeshInstance3D  # G draws a path here
var _navmesh_fill: StandardMaterial3D
var _navmesh_lines: StandardMaterial3D
var _overlay: CanvasLayer  # notes and journal; Shift+F12 hides it for a shot
var _shot_busy := false
var _shot_note: PanelContainer  # asks for a shot's note
var _shot_note_text: TextEdit
var _shot_note_json := ""  # the shot the note goes to
var _shot_note_mouse := Input.MOUSE_MODE_VISIBLE  # restored afterwards
var _image_space: SkydotImageSpace
var _cell_image_space := {}  # inside: the cell's IMGS, if it has one
const SHOT_FORMAT := 1


func _ready() -> void:
	_args = _parse_args(OS.get_cmdline_user_args())
	if _args.has("from-shot"):
		var from := _args_from_shot(_args["from-shot"])
		if from.is_empty():
			_fail("cannot read the shot " + _args["from-shot"])
			return
		var shot_note: String = from.get("note", "")
		from.erase("note")
		from.merge(_args, true)  # the command line wins
		_args = from
		if not shot_note.is_empty():
			print("shot note: ", shot_note)
	var args := _args
	_hour = float(args.get("time", "12"))
	if not args.has("pack") or not (args.has("cell") or args.has("world")):
		_fail("usage: -- --pack DIR (--cell EDITOR_ID | --world EDITOR_ID --at X,Y,Z)"
			+ " [--screenshot out.png]")
		return

	var pack := SkydotPack.new()
	if pack.open(args["pack"]) != OK:
		_fail(pack.get_error())
		return
	if args.has("pck"):
		push_warning("--pck is ignored: packs load directly, there is no bake any more")
	var world := pack.open_world()
	if world == null:
		_fail(pack.get_error())
		return
	_world = world
	_pack = pack
	_papyrus = SkydotPapyrus.new()
	_papyrus.setup(pack, world)
	_papyrus.enable_changed.connect(_on_enable_changed)
	_papyrus.play_animation.connect(_on_play_animation)
	_papyrus.havok_impulse.connect(_on_havok_impulse)
	_papyrus.motion_type_changed.connect(_on_motion_type)
	_papyrus.activate_requested.connect(func(ref: int, _activator: int, _default_only: bool) -> void:
		var node := _find_ref_node(ref)
		call_deferred("_activate", _world.get_ref_cell(ref), ref, node, true))
	_papyrus.open_changed.connect(func(ref: int, open: bool) -> void:
		var node := _find_ref_node(ref)
		print("0x%08X %s by script" % [ref, "opened" if open else "closed"])
		if node != null:
			node.set_meta("skydot_open", open)
			_on_play_animation(ref, "Open" if open else "Close"))
	_papyrus.lock_changed.connect(func(ref: int, locked: bool) -> void:
		print("0x%08X %s by script" % [ref, "locked" if locked else "unlocked"]))
	_papyrus.message.connect(func(text: String, _box: bool) -> void: _note(text))
	_papyrus.quest_started.connect(func(quest: int) -> void:
		if _quests_ready:
			print("quest started: ", _quest_name(quest)))
	# Only stages with journal text reach the screen, as in the game.
	_papyrus.quest_stage.connect(func(quest: int, stage: int, text: String) -> void:
		if text != "":
			_note("%s: %s" % [_quest_name(quest), text])
		elif _quests_ready:
			print("%s: stage %d" % [_quest_name(quest), stage]))
	_papyrus.objective_changed.connect(func(quest: int, index: int, state: String, text: String) -> void:
		_note("%s objective %d %s: %s" % [_quest_name(quest), index, state, text]))
	_papyrus.effect_shader.connect(func(shader: int, ref: int, playing: bool) -> void:
		print("effect shader 0x%08X %s on 0x%08X (not drawn yet)" % [shader, "plays" if playing else "stops", ref]))
	_papyrus.trigger.connect(func(ref: int, _actor: int, entered: bool) -> void:
		print("%s trigger 0x%08X" % ["entered" if entered else "left", ref]))
	world.skyrim_materials = args.get("materials", "on") != "off"
	world.effects = args.get("effects", "on") != "off"
	world.grass = args.get("grass", "on") != "off"
	world.all_light_shadows = args.get("light-shadows", "game") == "all"
	world.collision = args.get("collision", "on") != "off"
	world.navigation = args.get("navigation", "on") != "off"
	world.actors = args.get("actors", "on") != "off"
	_shot_delay = float(args.get("shot-delay", "0"))
	var captures := args.has("screenshot") or args.has("benchmark")
	world.actor_wander = args.get("wander", "off" if captures else "on") != "off"
	if args.get("ai", "on") != "off":
		_ai = SkydotAi.new()
		if _ai.setup(world, _papyrus) == OK:
			_ai.days = _day + _hour / 24.0
			_ai.drive = world.actor_wander
			_ai.time_scale = 0.0 if captures else float(args.get("time-scale", "20"))
			_ai.actor_arrived.connect(_on_actor_arrived)
			_ai.actor_left.connect(func(ref: int, _door: int) -> void:
				print("0x%08X leaves through a door" % ref))
		else:
			_ai = null
	_radius = int(args.get("radius", "2"))
	_lod_split = float(args.get("lod-split", "1.5"))
	var msaa: int = MSAA_NAMES.find(args.get("msaa", "off"))
	get_viewport().msaa_3d = MSAA_STEPS[maxi(msaa, 0)]
	_build_budget_usec = int(args.get("build-budget", "8000"))
	if args.has("tiling"):
		world.terrain_tiling = float(args["tiling"])
	_pick_locks = args.get("pick-locks", "off") != "off"
	_preload_doors = args.get("preload-doors", "off" if captures else "on") != "off"
	_preload_distance = float(args.get("preload-distance", "15"))
	# Runs that capture, measure or activate on their own ignore the keyboard
	# and mouse, so a stray touch cannot move the view.
	_input = not (args.has("screenshot") or args.has("benchmark") or args.has("activate")
		or args.has("no-input"))
	if args.has("activate"):
		for id in args["activate"].split(","):
			_script_activations.append(id.hex_to_int() if id.begins_with("0x") else int(id))

	_camera = Camera3D.new()
	_camera.near = 0.05
	# The scene is drawn in the game's gamma space; this grades it as the
	# game's image space does and hands Godot linear colour (always on).
	_image_space = SkydotImageSpace.new()
	_camera.compositor = Compositor.new()
	_camera.compositor.compositor_effects = [_image_space]
	if args.has("fov"):  # vertical, degrees
		_camera.fov = float(args["fov"])
	add_child(_camera)
	_player = SkydotPlayer.new()
	_player.name = "player"
	_player.eye_height = EYE_HEIGHT
	_player.fly = args.get("walk", "on") == "off" or args.get("collision", "on") == "off" \
		or args.has("benchmark") or args.has("screenshot")
	add_child(_player)
	var overlay := CanvasLayer.new()
	_overlay = overlay
	_notes = Label.new()
	_notes.position = Vector2(16, 16)
	_notes.add_theme_color_override("font_outline_color", Color.BLACK)
	_notes.add_theme_constant_override("outline_size", 4)
	overlay.add_child(_notes)
	_journal = Label.new()
	_journal.position = Vector2(16, 16)
	_journal.visible = false
	_journal.add_theme_color_override("font_outline_color", Color.BLACK)
	_journal.add_theme_constant_override("outline_size", 4)
	overlay.add_child(_journal)
	add_child(overlay)
	var fade_layer := CanvasLayer.new()
	fade_layer.layer = 100  # over the notes
	_fade = ColorRect.new()
	_fade.color = Color(0, 0, 0, 0)
	_fade.set_anchors_preset(Control.PRESET_FULL_RECT)
	_fade.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_fade.visible = false
	fade_layer.add_child(_fade)
	add_child(fade_layer)
	if DisplayServer.get_name() != "headless" and _input:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

	if args.get("quests", "on") != "off":
		var started := _papyrus.start_game_enabled_quests()
		print("quests: %d of %d started with the game" % [started, world.get_quest_count()])
	_quests_ready = true
	if args.has("set-stage"):
		for item in args["set-stage"].split(","):
			var parts: PackedStringArray = item.split(":")
			var quest := world.find_quest(parts[0])
			if quest == 0 or parts.size() != 2:
				_fail("--set-stage wants EDID:STAGE, got " + item)
				return
			if not _papyrus.set_stage(quest, int(parts[1])):
				print("%s: stage %s refused" % [parts[0], parts[1]])

	if args.has("world"):
		var world_id := world.find_world(args["world"])
		if world_id == 0:
			_fail("no worldspace named " + args["world"])
			return
		if not args.has("at"):
			_fail("--world needs --at X,Y,Z")
			return
		var started := Time.get_ticks_usec()
		var variants := world.warm_up()
		print("warm_up: %d shader variants in %.0f ms" % [variants, (Time.get_ticks_usec() - started) / 1000.0])
		var at := SkydotWorld.skyrim_position(_vec3(args["at"]))
		var target = SkydotWorld.skyrim_position(_vec3(args["target"])) if args.has("target") else null
		if not _enter_exterior(world_id, at, target):
			return
		if args.has("look"):
			_look_game(args["look"])
		_speed = 10.0
		if args.has("benchmark"):
			_benchmark = float(args["benchmark"])
			_stream_max_usec = 0
			_fly_speed = float(args.get("fly-speed", "20"))
			while _stream_step():
				OS.delay_msec(5)
			while _lod != null and _lod.update(_camera.global_position, 1000000) > 0:
				OS.delay_msec(5)
		if args.has("screenshot"):
			# Everything in range first, so the capture is complete.
			while _stream_step():
				OS.delay_msec(5)
			while _lod != null and _lod.update(_camera.global_position, 1000000) > 0:
				OS.delay_msec(5)
			if _lod != null:
				print("lod: ", _lod.get_stats())
			_shot_path = args["screenshot"]
			_shots.append(null)
		return

	var cell_id := world.find_cell(args["cell"])
	if cell_id == 0:
		_fail("no cell named " + args["cell"])
		return
	var fixed_view := args.has("at") and args.has("target")
	if fixed_view:
		_enter_interior(cell_id, SkydotWorld.skyrim_position(_vec3(args["at"])),
			SkydotWorld.skyrim_position(_vec3(args["target"])))
	else:
		_enter_interior(cell_id, SkydotWorld.skyrim_position(_vec3(args["at"])) if args.has("at") else null, null)
	if args.has("look"):
		_look_game(args["look"])
	if args.has("load"):
		_load_game(args["load"])

	if args.has("screenshot"):
		_shot_path = args["screenshot"]
		if fixed_view or (args.has("at") and args.has("look")):  # --look: a shot's view
			_shots.append(null)
		else:
			for i in 4:
				_shots.append(i * PI / 2.0)
			_apply_look(_shots[0], -0.15)


## Remove the current cell or worldspace.
func _leave() -> void:
	_cell_image_space = {}
	if _image_space != null:
		_image_space.reset_adaptation()
	if _weather != null and is_instance_valid(_weather):
		_hour = _weather.hour
		_day = _weather.day
	_weather = null
	for node in _place:
		if is_instance_valid(node):
			node.queue_free()
	_place.clear()
	for key in _loaded:
		if _loaded[key] != null:
			_loaded[key].queue_free()
	_loaded.clear()
	for key in _building:
		_building[key].queue_free()
	_building.clear()
	_world_id = 0
	_lod = null  # freed with _place
	_load_doors.clear()
	_streaming = false
	_lod_busy = false
	if _path_line != null:
		_path_line.mesh = null
	_world.call_deferred("trim_cache")


func _add_to_place(node: Node) -> void:
	if node.get_parent() == null:
		add_child(node)
	_place.append(node)


## Build an interior and put the camera at `at` (a camera position) looking at
## `target`, or at eye height in the middle of the cell when `at` is null.
## `prepared` is the cell built ahead without its actors (_prepare_step).
func _enter_interior(cell_id: int, at, target, prepared: Node3D = null) -> void:
	_leave()
	_cell_id = cell_id
	var cell := _world.get_cell(cell_id)
	if _ai != null:
		_ai.set_space(cell_id)
		_ai.settle_actors()  # a pass begun when preparing, or a full one
	var root := prepared
	if root != null:
		_release(root)
		_world.continue_build(root, 1 << 62)  # the actors, where they are now
	else:
		root = _world.build_cell(cell_id)
	_add_to_place(root)
	_collect_doors(root)
	if _ai != null:
		_ai.attach_built(root)
	print("cell %s: %s" % [cell["editor_id"], root.get_meta("skydot_stats")])
	_scripts_loaded(root, cell_id)
	_navmesh_overlay(root)
	_add_environment(cell)
	_camera.far = 500.0
	var spawn = _interior_spawn(cell_id) if at == null and not _player.fly else null
	if spawn != null:
		var eye: Vector3 = spawn.origin + Vector3(0, EYE_HEIGHT, 0)
		var forward: Vector3 = -spawn.basis.z
		forward.y = 0.0
		_place_camera(eye, eye + (forward.normalized() if forward.length() > 0.001 else Vector3.FORWARD))
	elif at == null:
		if not _player.fly:
			_player.fly = true
			print("no door leads into %s: flying (V walks)" % cell["editor_id"])
		var bounds := _mesh_bounds(root)
		print("bounds: ", bounds)
		var eye := bounds.get_center()
		eye.y = bounds.position.y + min(EYE_HEIGHT, bounds.size.y * 0.5)
		_place_camera(eye, null)
		_apply_look(0.0, 0.0)
	else:
		_place_camera(at, target)
	print("entered ", cell["editor_id"])


## Stream worldspace `world_id` around `at`, looking at `target` (null keeps
## the current direction). `prepared` holds cells and LOD built ahead
## (_prepare_step); they finish with their actors as streamed cells do.
## Returns false if it failed.
func _enter_exterior(world_id: int, at: Vector3, target, prepared := {}) -> bool:
	_leave()
	_world_id = world_id
	_cell_id = 0
	if _ai != null:
		_ai.set_space(world_id)
		_ai.settle_actors()  # a pass begun when preparing, or a full one
	var weather := 0
	if _args.has("weather"):
		weather = _world.find_weather(_args["weather"])
		if weather == 0:
			_fail("no weather named " + _args["weather"])
			return false
	if not _start_weather(weather):
		_add_sky(_world.get_sky(_world_id, _hour, weather), _args.get("shadows", "on") != "off")
	_camera.far = (_radius + 1) * CELL_UNITS * _unit_scale * 1.5
	_place_camera(at, target)
	var lod: SkydotLod = prepared.get("lod")
	if lod == null:
		lod = _make_lod(world_id)
	if lod != null:
		_release(lod)
		_add_to_place(lod)
		_lod = lod
		_camera.far = 40000.0
	# Prepared cells join the streaming ones; each is released when it is
	# finished (_finish_cell), so their cost is spread over frames, but the
	# one under the camera at once, for the ground.
	var cells: Dictionary = prepared.get("cells", {})
	var centre := _camera_cell()
	for key in cells:
		var cell: Node3D = cells[key]
		if cell == null:
			_loaded[key] = null
			continue
		if key == centre:
			_release(cell)
			cell.visible = false  # until _finish_cell
		_building[key] = cell
	for w in _world.list_worlds():
		if w["id"] == world_id:
			print("entered ", w["editor_id"])
	return true


## The worldspace's LOD, set up as the options say; null if it has none or
## --lod off.
func _make_lod(world_id: int) -> SkydotLod:
	if _args.get("lod", "on") == "off":
		return null
	var lod := SkydotLod.new()
	if lod.setup(_pack, _world, world_id) != OK:
		print("no LOD: ", lod.get_error())
		lod.free()
		return null
	lod.name = "lod"
	lod.split_distance = _lod_split
	if _args.has("tree-distance"):
		lod.tree_distance = float(_args["tree-distance"])
	return lod


func _place_camera(at: Vector3, target) -> void:
	_camera.position = at
	if target != null:
		_camera.look_at(target)
	_yaw = _camera.rotation.y
	_pitch = _camera.rotation.x
	# A little above the spot, so feet placed on a floor do not start in it.
	_player.teleport(at - Vector3(0, EYE_HEIGHT - 0.05, 0))
	_last_ground = _player.global_position
	_ground_check = 3


## Where the game puts the player entering interior `cell_id`: the arrival
## spot of a door elsewhere that leads to one of its doors, or null.
func _interior_spawn(cell_id: int):
	for ref in _world.get_refs(cell_id):
		var door := _world.get_door(ref["id"])
		if door.is_empty():
			continue
		var back := _world.get_door(door["destination"])
		if not back.is_empty() and back["destination_cell"] == cell_id:
			return back["arrival"]
	return null


## Hold the player while the ground under it is still being built, keep its
## water level, and catch it if it falls through the world.
func _update_player() -> void:
	if _world_id != 0:
		var key := _camera_cell()
		var cell = _loaded.get(key)
		_player.hold = (not _loaded.has(key) and not _player.fly) or _fade_phase != FADE_NONE
		var water: Node3D = cell.get_node_or_null("Water") if cell != null else null
		if water != null:
			_player.water_height = water.global_position.y
		else:
			_player.clear_water()
	else:
		_player.hold = _fade_phase != FADE_NONE
		_player.clear_water()
	if _player.hold or _player.fly:
		return
	if _ground_check > 0:
		_ground_check -= 1
		if _ground_check == 0:
			_lift_onto_land()
	if _player.is_on_floor():
		_last_ground = _player.global_position
	elif _last_ground.y - _player.global_position.y > 200.0:
		_player.teleport(_last_ground)
		_player.fly = true
		_note("fell through the world: flying (V walks)")


## A spot given in game units may be under the terrain; put the feet on it.
func _lift_onto_land() -> void:
	var feet := _player.global_position
	var query := PhysicsRayQueryParameters3D.create(feet + Vector3(0, 2000, 0), feet,
		SkydotPlayer.LAYER_TERRAIN)
	var hit := get_world_3d().direct_space_state.intersect_ray(query)
	if not hit.is_empty():
		_player.teleport(hit["position"] + Vector3(0, 0.05, 0))
		_last_ground = _player.global_position


## Arrive through a load door: its XTEL gives the spot and the facing.
## What was built ahead for the place behind it is used.
func _travel(door: Dictionary) -> void:
	var arrival: Transform3D = door["arrival"]
	var eye := arrival.origin + Vector3(0, EYE_HEIGHT, 0)
	var forward := -arrival.basis.z
	forward.y = 0.0
	var target := eye + (forward.normalized() if forward.length() > 0.001 else Vector3.FORWARD)
	var prepared := {}
	if not _prepared.is_empty() and _prepared["info"]["destination"] == door["destination"]:
		prepared = _prepared
		_prepared = {}
		print("using what was prepared (%s)" % ("complete" if prepared["done"] else "in part"))
	_drop_prepared()
	if door["destination_interior"]:
		_enter_interior(door["destination_cell"], eye, target, prepared.get("root"))
	elif door["destination_world"] != 0:
		_enter_exterior(door["destination_world"], eye, target, prepared)
	else:
		print("door 0x%08X leads nowhere this pack knows" % door["ref"])


## Attach the scripts of what was just built (OnInit once, OnLoad each time),
## including model-less ones such as triggers, and show script-made changes to
## what is enabled.
func _scripts_loaded(root: Node, cell_id: int) -> void:
	if cell_id != 0:
		_papyrus.attach_cell(cell_id)
	var scripted := _papyrus.attach_built(root)
	if scripted > 0:
		print("scripts: %d references in %s" % [scripted, root.name])
	# Every change scripts have made so far: apply those to this cell's
	# references in one walk over it, not a search of the scene for each.
	var changes := _papyrus.get_disabled_changes()
	if not changes.is_empty():
		for node in _ref_nodes(root):
			var ref: int = node.get_meta("skydot_ref")
			if changes.has(ref):
				_show_ref(node, not changes[ref])


func _on_enable_changed(ref: int, enabled: bool) -> void:
	print("0x%08X %s by script" % [ref, "enabled" if enabled else "disabled"])
	var node := _find_ref_node(ref)
	if node != null:
		if not enabled:
			_wake_around(node)
		_show_ref(node, enabled)
	elif enabled:
		# Initially disabled references are not built with their cell.
		var holder := _world.build_ref(_world.get_ref_cell(ref), ref)
		if holder != null:
			_add_to_place(holder)


## Enable or disable a built reference: shown and solid, or neither (a
## disabled node's bodies leave the physics space).
func _show_ref(node: Node, enabled: bool) -> void:
	node.visible = enabled
	node.process_mode = Node.PROCESS_MODE_INHERIT if enabled else Node.PROCESS_MODE_DISABLED


## Clutter resting on or against `node` falls once it is gone or moves.
func _wake_around(node: Node) -> void:
	var bounds := _mesh_bounds(node)
	if bounds.size != Vector3.ZERO:
		SkydotWorld.wake_clutter(self, bounds.get_center(), bounds.size.length() / 2 + 0.5)


func _clutter_body(ref: int) -> SkydotDynamicBody:
	var node := _find_ref_node(ref)
	if node == null:
		return null
	var bodies := node.find_children("*", "SkydotDynamicBody", true, false)
	return bodies[0] if not bodies.is_empty() else null


## ApplyHavokImpulse: Skyrim's direction, Havok's magnitude (Havok units are
## metres here, so it applies as is).
func _on_havok_impulse(ref: int, direction: Vector3, magnitude: float) -> void:
	var body := _clutter_body(ref)
	if body == null:
		print("0x%08X has no movable body for an impulse" % ref)
		return
	body.wake()
	var godot_direction := SkydotWorld.skyrim_position(direction).normalized()
	body.apply_central_impulse(godot_direction * magnitude)


## SetMotionType: the moving types release clutter, keyframed and fixed hold
## it. Static models cannot be made to move.
func _on_motion_type(ref: int, motion_type: int) -> void:
	var body := _clutter_body(ref)
	if body == null:
		print("0x%08X has no movable body for motion type %d" % [ref, motion_type])
		return
	if motion_type in [4, 5]:
		body.freeze = true
	else:
		body.wake()


func _on_play_animation(ref: int, animation: String) -> void:
	var node := _find_ref_node(ref)
	var animator := node.get_node_or_null("SkydotAnimator") if node != null else null
	if animator == null or not animator.play(animation):
		print("0x%08X has no %s animation" % [ref, animation])
		return
	_wake_around(node)
	# PlayAnimationAndWait waits for a text key or the clip's end.
	if not animator.has_meta("skydot_notifies"):
		animator.set_meta("skydot_notifies", true)
		animator.text_key.connect(func(_clip: String, key: String) -> void:
			_papyrus.notify_animation_event(ref, key))
		animator.finished.connect(func(clip: String) -> void:
			_papyrus.notify_animation_event(ref, clip))


## What activating reference `ref` in `cell` does. `node` is its model, if
## built. `parent` activations ignore the parent-only flag. Returns false if
## the reference could not be found.
func _activate(cell: int, ref: int, node: Node, force: bool, parent := false) -> bool:
	var info := _world.get_ref_info(cell, ref)
	if info.is_empty():
		return false
	var label := "0x%08X %s (%s)" % [ref, info["editor_id"], info["type"]]
	if info["parent_activate_only"] and not parent:
		_note(label + " only responds to its activate parents")
		return true
	var level := _papyrus.get_lock_level(ref)  # -1: not locked; 0 is Novice
	if level >= 0 and not force and not _pick_locks:
		_note(label + " is locked (level %d, %s); Shift+F opens it anyway" % [level, _lock_name(level)])
		return true
	if level >= 0:
		_papyrus.set_locked(ref, false)
	if _papyrus.activate(ref) > 0:
		print(label, " runs ", ", ".join(_papyrus.get_scripts(ref).map(func(s): return s["name"])))
	for child in _world.get_activate_children(ref):
		get_tree().create_timer(child["delay"]).timeout.connect(func() -> void:
			_activate(child["cell"], child["ref"], _find_ref_node(child["ref"]), true, true))
	# A script that blocks activation handles it alone.
	if _papyrus.is_activation_blocked(ref):
		_note(label + ": activation blocked, only its scripts ran")
		return true
	if info["door"] != null:
		print(label, " leads to 0x%08X" % info["door"]["destination"])
		if _input and DisplayServer.get_name() != "headless":
			_begin_fade(info["door"])
		else:
			_travel(info["door"])
		return true
	if info["type"] == "DOOR" and node != null:
		var animator := node.get_node_or_null("SkydotAnimator")
		if animator != null:
			var open: bool = not node.get_meta("skydot_open", false)
			if animator.play("Open" if open else "Close"):
				node.set_meta("skydot_open", open)
				node.remove_meta("skydot_opened_by")  # an actor no longer closes it
				_papyrus.set_open_state(ref, 1 if open else 3)
				print(label, " opens" if open else " closes")
				return true
	if _papyrus.get_scripts(ref).is_empty():
		_note(label + ": nothing happens")
	return true


## XLOC's level as the game names it.
func _lock_name(level: int) -> String:
	if level >= 255:
		return "requires a key"
	for step in [[100, "Master"], [75, "Expert"], [50, "Adept"], [25, "Apprentice"]]:
		if level >= step[0]:
			return step[1]
	return "Novice"


## The model of reference `ref` among what is built, or null.
func _find_ref_node(ref: int) -> Node:
	var pending: Array = _place.duplicate()
	pending.append_array(_loaded.values())
	while not pending.is_empty():
		var node = pending.pop_back()
		if node == null or not is_instance_valid(node) or node.is_queued_for_deletion() or node == _lod:
			continue
		for child in node.get_children():
			if child.get_meta("skydot_ref", 0) == ref:
				return child
			if not child.has_meta("skydot_ref") and child.get_child_count() > 0:
				pending.append(child)
	return null


## The reference nodes under `root` (not those inside another reference).
func _ref_nodes(root: Node) -> Array[Node]:
	var out: Array[Node] = []
	var pending: Array[Node] = [root]
	while not pending.is_empty():
		for child in pending.pop_back().get_children():
			if child.has_meta("skydot_ref"):
				out.append(child)
			elif child.get_child_count() > 0:
				pending.append(child)
	return out


## --activate: activate the next reference once its model is built; quit
## when all are done or one never appears.
func _run_script_activation() -> void:
	if _script_activations.is_empty():
		return
	var ref: int = _script_activations[0]
	var node := _find_ref_node(ref)
	if node == null:
		_script_wait += 1
		if _script_wait > 600:
			_fail("reference 0x%08X never appeared" % ref)
		return
	_script_wait = 0
	_script_activations.pop_front()
	_activate(node.get_meta("skydot_cell"), ref, node, false)
	if _script_activations.is_empty():
		_quit_in = 5  # let scripts run first


func _activate_in_view(force: bool) -> void:
	var from := _camera.global_position
	var to := from - _camera.global_transform.basis.z * REACH
	var hit := _world.pick_ref(self, from, to)
	if hit.is_empty():
		_note("nothing to activate")
		return
	_activate(hit["cell"], hit["ref"], hit["node"], force)


## The camera's cell in the worldspace grid.
func _camera_cell() -> Vector2i:
	var p := _camera.position / _unit_scale
	return Vector2i(floori(p.x / CELL_UNITS), floori(-p.z / CELL_UNITS))


## Drop cells out of range, then load cells in range nearest first: their
## scenes and textures on the loader's threads, the build itself here in steps
## within a frame budget. A cell stays hidden until its build is done, then
## shows, masks its LOD and attaches its scripts at once. Returns false when
## every cell in range is loaded.
func _stream_step() -> bool:
	var centre := _camera_cell()
	var dropped := false
	for key in _loaded.keys():
		var offset: Vector2i = key - centre
		if max(abs(offset.x), abs(offset.y)) > _radius + 1:
			if _loaded[key] != null:
				_loaded[key].queue_free()
				dropped = true
				if _lod != null:
					_lod.set_cell_loaded(key.x, key.y, false)
			_loaded.erase(key)
	for key in _building.keys():
		var offset: Vector2i = key - centre
		if max(abs(offset.x), abs(offset.y)) > _radius + 1:
			_building[key].queue_free()
			_building.erase(key)
	if dropped:
		_world.call_deferred("trim_cache")

	var nearest := func(a: Vector2i, b: Vector2i) -> bool:
		return (a - centre).length_squared() < (b - centre).length_squared()
	var started := Time.get_ticks_usec()
	var building: Array = _building.keys()
	building.sort_custom(nearest)
	for key in building:
		var left := _build_budget_usec - (Time.get_ticks_usec() - started)
		if left <= 0:
			return true
		if _world.continue_build(_building[key], left):
			_finish_cell(key, _building[key])
			_building.erase(key)

	var wanted: Array[Vector2i] = []
	for dy in range(-_radius, _radius + 1):
		for dx in range(-_radius, _radius + 1):
			var key := centre + Vector2i(dx, dy)
			if not _loaded.has(key) and not _building.has(key):
				wanted.append(key)
	if wanted.is_empty():
		return not _building.is_empty()
	wanted.sort_custom(nearest)

	for key in wanted.slice(0, 4):
		if _world.request_exterior(_world_id, key.x, key.y) != 0:
			continue
		var left := _build_budget_usec - (Time.get_ticks_usec() - started)
		if left <= 0:
			break
		var cell := _world.begin_exterior(_world_id, key.x, key.y)
		if cell == null:
			_loaded[key] = null
			continue
		cell.visible = false
		add_child(cell)
		if _world.continue_build(cell, left):
			_finish_cell(key, cell)
		else:
			_building[key] = cell
	return true


func _finish_cell(key: Vector2i, cell: Node3D) -> void:
	if cell.process_mode == Node.PROCESS_MODE_DISABLED:
		_release(cell)  # prepared behind a door (_hold)
	cell.visible = true
	if _ai != null:
		_ai.attach_built(cell)
	print("cell ", key, ": ", cell.get_meta("skydot_stats"))
	_scripts_loaded(cell, _world.get_exterior_cell(_world_id, key.x, key.y))
	_navmesh_overlay(cell)
	_collect_doors(cell)
	if _lod != null:
		_lod.set_cell_loaded(key.x, key.y, true)
	_loaded[key] = cell


## Remember the load doors among what was just built.
func _collect_doors(root: Node) -> void:
	for node in _ref_nodes(root):
		var ref: int = node.get_meta("skydot_ref")
		if not _world.get_door(ref).is_empty():
			_load_doors[ref] = node


## Build the place behind the nearest load door ahead, a little each frame
## while nothing streams here: the destination's resources on the loader's
## threads, then its references (not its actors) in the scene but held
## (_hold). Going through that door then only adds the actors. One place is kept; walking
## away from the door drops it.
func _prepare_step() -> void:
	_prepare_scan -= 1
	if _prepare_scan <= 0:
		_prepare_scan = 15
		var feet := _player.global_position
		var nearest := 0
		var nearest_distance := _preload_distance
		var current: int = _prepared.get("door", 0)
		var current_distance := INF
		for ref in _load_doors.keys():
			var node: Node3D = _load_doors[ref]
			if not is_instance_valid(node) or node.is_queued_for_deletion():
				_load_doors.erase(ref)
				continue
			var distance := node.global_position.distance_to(feet)
			if ref == current:
				current_distance = distance
			if distance < nearest_distance:
				nearest = ref
				nearest_distance = distance
		if nearest != 0 and nearest != current:
			_drop_prepared()
			_prepared = _new_preparation(_world.get_door(nearest))
		elif nearest == 0 and current != 0 and current_distance > _preload_distance * 1.5:
			_drop_prepared()
	if _prepared.is_empty() or _prepared["done"] or _streaming:
		return
	_advance_preparation(PREPARE_BUDGET_USEC)


func _new_preparation(door: Dictionary) -> Dictionary:
	var prepared := {"door": door["ref"], "info": door, "done": false, "started": Time.get_ticks_usec()}
	if _ai != null:
		_ai.begin_placing()  # where actors are, worked out over the next frames
	if door["destination_interior"]:
		prepared["cell"] = door["destination_cell"]
		prepared["root"] = null
		return prepared
	if door["destination_world"] == 0:
		return {}
	var world_id: int = door["destination_world"]
	prepared["world"] = world_id
	var arrival: Transform3D = door["arrival"]
	prepared["eye"] = arrival.origin + Vector3(0, EYE_HEIGHT, 0)
	var p := arrival.origin / _unit_scale
	var centre := Vector2i(floori(p.x / CELL_UNITS), floori(-p.z / CELL_UNITS))
	var keys: Array[Vector2i] = []
	for dy in range(-_radius, _radius + 1):
		for dx in range(-_radius, _radius + 1):
			keys.append(centre + Vector2i(dx, dy))
	keys.sort_custom(func(a: Vector2i, b: Vector2i) -> bool:
		return (a - centre).length_squared() < (b - centre).length_squared())
	prepared["keys"] = keys
	prepared["cells"] = {}  # Vector2i -> Node3D, null where nothing is
	prepared["placed"] = {}  # cells whose references are all placed
	prepared["lod"] = _make_lod(world_id)
	if prepared["lod"] != null:
		_hold(prepared["lod"])
	return prepared


func _advance_preparation(budget_usec: int) -> void:
	var started := Time.get_ticks_usec()
	var p := _prepared
	if p.has("cell"):
		if p["root"] == null:
			if _world.request_cell(p["cell"]) > 0:
				return
			p["root"] = _world.begin_cell(p["cell"])
			if p["root"] != null:
				_hold(p["root"])
		p["done"] = _world.continue_build_static(p["root"], budget_usec)
	else:
		var complete := true
		for key in p["keys"]:
			var left := budget_usec - (Time.get_ticks_usec() - started)
			if left <= 0:
				complete = false
				break
			if p["placed"].has(key):
				continue
			if not p["cells"].has(key):
				if _world.request_exterior(p["world"], key.x, key.y) != 0:
					complete = false
					continue
				p["cells"][key] = _world.begin_exterior(p["world"], key.x, key.y)
				if p["cells"][key] != null:
					_hold(p["cells"][key])
			var cell: Node3D = p["cells"][key]
			if cell == null or _world.continue_build_static(cell, left):
				p["placed"][key] = true
			else:
				complete = false
		if p["lod"] != null and p["lod"].update(p["eye"], LOD_BUDGET_USEC) > 0:
			complete = false
		p["done"] = complete
	if p["done"]:
		print("prepared what is behind 0x%08X in %.0f ms" % [p["door"], (Time.get_ticks_usec() - p["started"]) / 1000.0])


## Go through `door` behind a fade to black (_fade_step).
func _begin_fade(door: Dictionary) -> void:
	if _fade_phase != FADE_NONE:
		return
	_fade_door = door
	_fade_phase = FADE_OUT
	_fade.visible = true


## The door transition, a step per frame: fade out; travel once a black
## frame is on screen; wait until the place is built and a few frames are
## drawn (the first frame of a new place is slow); fade in.
func _fade_step(delta: float) -> void:
	match _fade_phase:
		FADE_OUT:
			_fade.color.a = minf(1.0, _fade.color.a + delta / FADE_SECONDS)
			if _fade.color.a >= 1.0:
				_fade_phase = FADE_TRAVEL  # this frame draws black first
		FADE_TRAVEL:
			_travel(_fade_door)
			_fade_door = {}
			_fade_phase = FADE_WAIT
			_fade_wait = 0.0
			_fade_frames = 0
		FADE_WAIT:
			_fade_wait += delta
			var built := _world_id == 0 or (not _streaming and not _lod_busy)
			if built:
				_fade_frames += 1
			if _fade_frames > FADE_SETTLE_FRAMES or _fade_wait > FADE_TIMEOUT:
				_fade_phase = FADE_IN
		FADE_IN:
			_fade.color.a = maxf(0.0, _fade.color.a - delta / FADE_SECONDS)
			if _fade.color.a <= 0.0:
				_fade_phase = FADE_NONE
				_fade.visible = false


## Free what was built ahead.
func _drop_prepared() -> void:
	var p := _prepared
	_prepared = {}
	if p.get("root") != null:
		p["root"].queue_free()
	for cell in p.get("cells", {}).values():
		if cell != null:
			cell.queue_free()
	if p.get("lod") != null:
		p["lod"].queue_free()


## Put a place being built ahead into the scene, hidden, without physics
## (disabled bodies are not in the space) and off the navigation map. What
## Godot creates for its nodes then happens within the preparation's budget,
## not on arrival.
func _hold(node: Node3D) -> void:
	node.visible = false
	node.process_mode = Node.PROCESS_MODE_DISABLED
	add_child(node)
	_set_regions(node, false)


## Undo _hold on arrival.
func _release(node: Node3D) -> void:
	node.process_mode = Node.PROCESS_MODE_INHERIT
	node.visible = true
	_set_regions(node, true)


func _set_regions(root: Node, enabled: bool) -> void:
	for child in root.get_children():
		if child is NavigationRegion3D:
			child.enabled = enabled


func _benchmark_frame(delta: float) -> void:
	_frame_times.append(delta * 1000.0)
	_player.teleport(_player.global_position + Vector3(_fly_speed * delta, 0, 0))
	_benchmark -= delta
	if _benchmark > 0.0:
		return
	var sorted := _frame_times.duplicate()
	sorted.sort()
	var pick := func(q: float) -> float: return sorted[int(q * (sorted.size() - 1))]
	print("benchmark: %d frames, median %.1f ms, p99 %.1f ms, max %.1f ms, over 33 ms: %d, slowest streaming step %.1f ms"
		% [sorted.size(), pick.call(0.5), pick.call(0.99), sorted[-1],
		   sorted.filter(func(t: float) -> bool: return t > 33.0).size(), _stream_max_usec / 1000.0])
	get_tree().quit(0)


## Sky colours, sun or moon, ambient light and depth fog from get_sky; a
## neutral daylight setup if the worldspace has no climate.
## Time and weather outside (SkydotWeather): the climate's or region's
## weathers in turn, unless --weather fixes one. False if the worldspace has
## no climate.
func _start_weather(weather: int) -> bool:
	var node := SkydotWeather.new()
	node.name = "weather"
	node.hour = _hour
	node.day = _day
	# Screenshots and benchmarks stand still.
	var still := _args.has("screenshot") or _args.has("benchmark")
	node.time_scale = 0.0 if still else float(_args.get("time-scale", "20"))
	node.shadows = _args.get("shadows", "on") != "off"
	if weather != 0:
		node.auto_weather = false
	_add_to_place(node)
	if node.setup(_world, _world_id, _camera) != OK:
		_place.erase(node)
		node.queue_free()
		return false
	if weather != 0:
		node.set_weather(weather, 0.0)
	_weather = node
	node.weather_changed.connect(func(_id: int) -> void:
		_note("weather: " + str(node.get_state()["editor_id"])))
	print("weather: ", node.get_state()["editor_id"])
	return true


func _add_sky(sky_values: Dictionary, shadows: bool) -> void:
	var material := ProceduralSkyMaterial.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = Sky.new()
	env.sky.sky_material = material
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = shadows
	if sky_values.is_empty():
		env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
		sun.rotation_degrees = Vector3(-40, 30, 0)
	else:
		print("sky: ", sky_values["weather"], ", daylight ", sky_values["daylight"])
		material.sky_top_color = sky_values["sky_upper"]
		material.sky_horizon_color = sky_values["horizon"]
		material.ground_horizon_color = sky_values["horizon"]
		material.ground_bottom_color = sky_values["sky_lower"]
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_color = sky_values["ambient"]
		SkydotMaterials.set_game_light(sun, sky_values["sunlight"])
		var towards: Vector3 = sky_values["sun_direction"]
		sun.look_at_from_position(Vector3.ZERO, -towards,
			Vector3.UP if abs(towards.y) < 0.99 else Vector3.FORWARD)
		if sky_values.has("fog_far"):
			env.fog_enabled = true
			env.fog_mode = Environment.FOG_MODE_DEPTH
			env.fog_depth_begin = sky_values["fog_near"]
			env.fog_depth_end = sky_values["fog_far"]
			env.fog_depth_curve = sky_values["fog_power"]
			env.fog_density = sky_values["fog_max"]
			env.fog_light_color = sky_values["fog_far_color"]
			env.set_meta("skydot_fog_near_color", sky_values["fog_near_color"])
			env.fog_sky_affect = 0.0
	var node := WorldEnvironment.new()
	node.environment = env
	_add_to_place(node)
	_add_to_place(sun)


func _add_environment(cell: Dictionary) -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.02, 0.02, 0.02)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR  # see _image_space
	var lighting = cell.get("lighting")
	# Our shaders light with this (SkydotMaterials.sync_fog); XCLL's ambient
	# colour is the fallback.
	if cell.get("directional_ambient", []).size() == 6:
		env.set_meta("skydot_directional_ambient", cell["directional_ambient"])
	_cell_image_space = _world.get_image_space(cell.get("image_space", 0))
	if lighting != null:
		env.ambient_light_color = lighting["ambient"]
		env.ambient_light_energy = 1.0
		if lighting["fog_far"] > lighting["fog_near"]:
			# Our shaders compute the game's fog from these (SkydotMaterials.sync_fog).
			env.fog_enabled = true
			env.fog_mode = Environment.FOG_MODE_DEPTH
			env.fog_depth_begin = lighting["fog_near"]
			env.fog_depth_end = lighting["fog_far"]
			env.fog_depth_curve = lighting["fog_power"] if lighting["fog_power"] > 0.0 else 1.0
			env.fog_density = lighting["fog_max"] if lighting["fog_max"] > 0.0 else 1.0
			env.fog_light_color = lighting["fog_far_color"]
			env.set_meta("skydot_fog_near_color", lighting["fog_near_color"])
	else:
		env.ambient_light_color = Color(0.3, 0.3, 0.3)
	var node := WorldEnvironment.new()
	node.environment = env
	_add_to_place(node)

	if lighting != null and lighting["directional"] != Color(0, 0, 0):
		var sun := DirectionalLight3D.new()
		SkydotMaterials.set_game_light(sun, lighting["directional"])
		sun.rotation_degrees = Vector3(-float(lighting["directional_rotation_z"]),
			float(lighting["directional_rotation_xy"]), 0)
		_add_to_place(sun)


func _mesh_bounds(root: Node) -> AABB:
	var bounds := AABB()
	var first := true
	for node in root.find_children("*", "MeshInstance3D", true, false):
		var mesh := node as MeshInstance3D
		var box := mesh.global_transform * mesh.get_aabb()
		if first:
			bounds = box
			first = false
		else:
			bounds = bounds.merge(box)
	return bounds


func _apply_look(yaw: float, pitch: float) -> void:
	_yaw = yaw
	_pitch = pitch
	_camera.rotation = Vector3(_pitch, _yaw, 0)


func _process(delta: float) -> void:
	if _camera == null:
		return
	# Our shaders compute the fog themselves (SkydotMaterials.sync_fog).
	SkydotMaterials.sync_fog(get_viewport().find_world_3d().environment)
	if _args.get("image-space", "on") == "off":
		_image_space.clear()
	elif _weather != null and is_instance_valid(_weather):
		_image_space.set_image_space(_weather.get_image_space())
	else:
		_image_space.set_image_space(_cell_image_space)
	if _shot_path != "":
		if _shot_delay > 0.0:
			_shot_delay -= delta
			return
		_take_screenshots()
		return
	_update_player()
	_camera.global_position = _player.get_eye_position()
	if _world_id != 0:
		var stream_started := Time.get_ticks_usec()
		_streaming = _stream_step()
		if _lod != null:
			_lod_busy = _lod.update(_camera.global_position, LOD_BUDGET_USEC) > 0
		_stream_max_usec = max(_stream_max_usec, Time.get_ticks_usec() - stream_started)
	if _preload_doors:
		_prepare_step()
	if _fade_phase != FADE_NONE:
		_fade_step(delta)
	_papyrus.update_actor(SkydotPapyrus.PLAYER_REF,
		SkydotWorld.godot_to_skyrim(_player.global_position))
	_papyrus.update(delta)
	if _ai != null:
		# Outside the weather keeps the time; inside the AI's clock does.
		if _weather != null and is_instance_valid(_weather):
			_ai.time_scale = 0.0
			_ai.days = _weather.day + _weather.hour / 24.0
		else:
			_ai.time_scale = float(_args.get("time-scale", "20")) if _shot_path == "" and _benchmark <= 0.0 else 0.0
			_hour = _ai.get_hour()
			_day = int(_ai.days)
		_ai.update(delta)
	_age_notes(delta)
	if _quit_in >= 0:
		_quit_in -= 1
		if _quit_in < 0:
			if _args.has("save-to"):
				_save_game(_args["save-to"])
			get_tree().quit(0)
		return
	if not _script_activations.is_empty():
		_run_script_activation()
		return
	if _benchmark > 0.0:
		_benchmark_frame(delta)
		return
	if not _input:
		_player.set_input(Vector2.ZERO, 0.0, SkydotPlayer.RUN)
		_player.set_look(_yaw, _pitch)
		return
	var move := Vector2.ZERO
	if Input.is_key_pressed(KEY_W): move.y += 1
	if Input.is_key_pressed(KEY_S): move.y -= 1
	if Input.is_key_pressed(KEY_A): move.x -= 1
	if Input.is_key_pressed(KEY_D): move.x += 1
	var vertical := 0.0
	if Input.is_key_pressed(KEY_E) or (Input.is_key_pressed(KEY_SPACE) and _player.is_swimming()):
		vertical += 1
	if Input.is_key_pressed(KEY_Q):
		vertical -= 1
	var gait := SkydotPlayer.RUN
	if Input.is_key_pressed(KEY_SHIFT):
		gait = SkydotPlayer.SPRINT
	elif Input.is_key_pressed(KEY_CTRL):
		gait = SkydotPlayer.WALK
	_player.fly_speed = _speed
	_player.set_input(move, vertical, gait)
	_player.set_look(_yaw, _pitch)


func _unhandled_input(event: InputEvent) -> void:
	if not _input:
		return
	var captured := Input.mouse_mode == Input.MOUSE_MODE_CAPTURED
	if event is InputEventMouseMotion and (captured or Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)):
		_apply_look(_yaw - event.relative.x * 0.004,
			clamp(_pitch - event.relative.y * 0.004, -1.5, 1.5))
	elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT and not captured:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_ESCAPE:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_P:
		_note(_position_text())
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_O:
		_preload_doors = not _preload_doors
		if not _preload_doors:
			_drop_prepared()
		_note("load doors preload what is behind them" if _preload_doors else "load doors load on use")
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F12:
		_capture_shot(event.shift_pressed)
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F:
		if _fade_phase == FADE_NONE:
			_activate_in_view(event.shift_pressed)
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F5:
		_save_game(QUICKSAVE)
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F9:
		_load_game(QUICKSAVE)
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_J:
		_print_journal()
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_SPACE:
		_player.jump()
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_T and _weather != null:
		_weather.hour = fmod(_weather.hour + (-1.0 if event.shift_pressed else 1.0) + 24.0, 24.0)
		_note("%02d:%02d" % [int(_weather.hour), int(fmod(_weather.hour, 1.0) * 60)])
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_T and _ai != null:
		_ai.days = maxf(0.0, _ai.days + (-1.0 if event.shift_pressed else 1.0) / 24.0)
		_hour = _ai.get_hour()
		_note("%02d:%02d" % [int(_hour), int(fmod(_hour, 1.0) * 60)])
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_I:
		_inspect_actor()
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_K and _weather != null:
		_weather.next_weather()
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_N:
		_show_navmesh = not _show_navmesh
		_navmesh_overlay(self)
		_note("navmesh shown" if _show_navmesh else "navmesh hidden")
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_G:
		_path_to_view()
	elif event is InputEventKey and event.pressed and not event.echo and (event.keycode == KEY_BRACKETLEFT
			or event.keycode == KEY_BRACKETRIGHT):
		_lod_split = clampf(_lod_split * (1.25 if event.keycode == KEY_BRACKETRIGHT else 0.8), 0.5, 16.0)
		if _lod != null:
			_lod.split_distance = _lod_split
		# A level-8 quad splits into level-4 ones within split times 8 cells.
		_note("LOD detail %.2f (finest LOD within %.0f m)" % [_lod_split, _lod_split * 8 * CELL_UNITS * _unit_scale])
	elif event is InputEventKey and event.pressed and not event.echo and _world_id != 0 and (event.keycode == KEY_MINUS
			or event.keycode == KEY_EQUAL):
		_radius = clampi(_radius + (1 if event.keycode == KEY_EQUAL else -1), 1, 8)
		if _lod == null:
			_camera.far = (_radius + 1) * CELL_UNITS * _unit_scale * 1.5
		_note("full detail within %d cells (%d x %d)" % [_radius, 2 * _radius + 1, 2 * _radius + 1])
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_M:
		var next: int = (MSAA_STEPS.find(get_viewport().msaa_3d) + 1) % MSAA_STEPS.size()
		get_viewport().msaa_3d = MSAA_STEPS[next]
		_note("MSAA " + ("off" if next == 0 else MSAA_NAMES[next] + "x"))
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_V:
		_player.fly = not _player.fly
		if not _player.fly:
			_last_ground = _player.global_position
		_note("flying" if _player.fly else "walking")


## An actor the AI brought into the place on screen: build it where its
## place now is, if that part of the world is built.
func _on_actor_arrived(ref: int) -> void:
	var place := _world.get_actor_place(ref)
	if place.is_empty():
		return
	var parent: Node = null
	if _world_id == 0:
		if place["space"] == _cell_id and not _place.is_empty():
			parent = _place[0]
	elif place["space"] == _world_id:
		var p: Vector3 = place["position"]
		parent = _loaded.get(Vector2i(floori(p.x / CELL_UNITS), floori(p.y / CELL_UNITS)))
	if parent == null:
		return
	var node := _world.build_actor(ref)
	if node == null:
		return
	parent.add_child(node)
	if _ai != null:
		_ai.attach_built(node)


## Tell what the actor nearest the camera is doing (its AI package).
func _inspect_actor() -> void:
	if _ai == null:
		_note("no AI (--ai off)")
		return
	var best: SkydotActor = null
	var best_d := 8.0
	for actor in get_tree().root.find_children("*", "SkydotActor", true, false):
		var d: float = actor.global_position.distance_to(_camera.global_position)
		if d < best_d:
			best_d = d
			best = actor
	if best == null:
		_note("no actor within 8 m")
		return
	var ref: int = best.get_meta("skydot_ref", 0)
	var state := _ai.get_actor_state(ref)
	var text := "%s: %s (%s), %s, step %d of %s" % [best.name, state.get("package_editor_id", "-"),
		state.get("template", ""), state.get("procedure", "-"), int(state.get("step", 0)) + 1,
		",".join(state.get("steps", []))]
	_note(text)
	print(text, " ", state)


## Show or hide the navmeshes under `root` as a translucent overlay, built
## once per region.
func _navmesh_overlay(root: Node) -> void:
	for region in root.find_children("*", "NavigationRegion3D", true, false):
		var overlay: Node3D = region.get_node_or_null("Overlay")
		if overlay != null:
			overlay.visible = _show_navmesh
		elif _show_navmesh:
			region.add_child(_navmesh_mesh(region.navigation_mesh))


func _navmesh_mesh(nav: NavigationMesh) -> MeshInstance3D:
	if _navmesh_fill == null:
		_navmesh_fill = StandardMaterial3D.new()
		_navmesh_fill.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		_navmesh_fill.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		_navmesh_fill.cull_mode = BaseMaterial3D.CULL_DISABLED
		_navmesh_fill.albedo_color = Color(0.1, 0.9, 0.3, 0.3)
		_navmesh_lines = _navmesh_fill.duplicate()
		_navmesh_lines.albedo_color = Color(0.2, 1.0, 0.4, 0.9)
	var lift := Vector3(0, 0.03, 0)  # above the ground it lies on
	var vertices := nav.get_vertices()
	var faces := PackedVector3Array()
	var edges := PackedVector3Array()
	for i in nav.get_polygon_count():
		var polygon := nav.get_polygon(i)
		for k in polygon.size():
			faces.append(vertices[polygon[k]] + lift)
			edges.append(vertices[polygon[k]] + lift)
			edges.append(vertices[polygon[(k + 1) % polygon.size()]] + lift)
	var mesh := ArrayMesh.new()
	for part in [[faces, Mesh.PRIMITIVE_TRIANGLES, _navmesh_fill], [edges, Mesh.PRIMITIVE_LINES, _navmesh_lines]]:
		var arrays := []
		arrays.resize(Mesh.ARRAY_MAX)
		arrays[Mesh.ARRAY_VERTEX] = part[0]
		mesh.add_surface_from_arrays(part[1], arrays)
		mesh.surface_set_material(mesh.get_surface_count() - 1, part[2])
	var instance := MeshInstance3D.new()
	instance.name = "Overlay"
	instance.mesh = mesh
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return instance


## A path from the feet to the navmesh point closest to the view ray (G).
func _path_to_view() -> void:
	var map := get_world_3d().navigation_map
	var eye := _camera.global_position
	var end := eye - _camera.global_transform.basis.z * 300.0
	# Aim with physics where there is collision; the navigation map's own
	# segment test returns the origin when the segment misses.
	var hit := get_world_3d().direct_space_state.intersect_ray(
		PhysicsRayQueryParameters3D.create(eye, end, 0xFFFFFFFF, [_player.get_rid()]))
	var target := NavigationServer3D.map_get_closest_point(map, hit["position"]) if not hit.is_empty() \
		else NavigationServer3D.map_get_closest_point_to_segment(map, eye, end, false)
	var path := NavigationServer3D.map_get_path(map, _player.global_position, target, true)
	if _path_line == null:
		_path_line = MeshInstance3D.new()
		_path_line.name = "Path"
		_path_line.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		var material := StandardMaterial3D.new()
		material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		material.albedo_color = Color(1.0, 0.8, 0.1)
		material.no_depth_test = true
		_path_line.material_override = material
		add_child(_path_line)
	var line := ImmediateMesh.new()
	if path.size() >= 2:
		line.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
		for point in path:
			line.surface_add_vertex(point + Vector3(0, 0.1, 0))
		line.surface_end()
	_path_line.mesh = line
	if path.size() < 2:
		_note("no path")
		return
	var length := 0.0
	for k in range(1, path.size()):
		length += path[k - 1].distance_to(path[k])
	var short := path[path.size() - 1].distance_to(target)
	_note("path: %d points, %.1f m%s" % [path.size(), length,
		"" if short < 0.5 else ", ends %.1f m short" % short])


func _quest_name(quest: int) -> String:
	var info := _world.get_quest(quest)
	var name: String = info.get("name", "")
	return name if name != "" else info.get("editor_id", "0x%08X" % quest)


## Show a line at the top left for a few seconds, and print it.
func _note(text: String) -> void:
	print(text)
	_note_lines.append([text, NOTE_SECONDS])
	if _note_lines.size() > 6:
		_note_lines.pop_front()
	_notes.text = "\n".join(_note_lines.map(func(l): return l[0]))


func _age_notes(delta: float) -> void:
	if _note_lines.is_empty():
		return
	for line in _note_lines:
		line[1] -= delta
	var before := _note_lines.size()
	_note_lines = _note_lines.filter(func(l): return l[1] > 0.0)
	if _note_lines.size() != before:
		_notes.text = "\n".join(_note_lines.map(func(l): return l[0]))


## Running quests with their stage and the objectives they show.
## Toggle the journal on screen: running quests with displayed objectives, as
## the game's journal lists them. The console gets every running quest.
func _print_journal() -> void:
	if _journal.visible:
		_journal.visible = false
		_notes.visible = true
		return
	var running := _papyrus.get_running_quests()
	print("journal: %d running quests" % running.size())
	var lines: Array[String] = []
	for quest in running:
		var state := _papyrus.get_quest_state(quest)
		var info := _world.get_quest(quest)
		var shown: Array = []
		for o in info.get("objectives", []):
			var s: Dictionary = state["objectives"].get(o["index"], {})
			if s.get("displayed", false) and not s.get("completed", false):
				shown.append(o["text"])
		print("  %s (0x%08X) stage %d%s" % [_quest_name(quest), quest, state["stage"],
			("  -> " + "; ".join(shown)) if not shown.is_empty() else ""])
		if not shown.is_empty():
			lines.append("%s  (stage %d)" % [_quest_name(quest), state["stage"]])
			for text in shown:
				lines.append("    " + text)
	lines.push_front("Journal: %d quests with objectives, %d running (J closes)" % [
		lines.filter(func(l: String) -> bool: return not l.begins_with(" ")).size(), running.size()])
	_journal.text = "\n".join(lines)
	_journal.visible = true
	_notes.visible = false


## The camera in the game's terms: position in game units (as
## `player.getpos`) and angles in degrees (as `player.getangle z` and `x`).
func _position_text() -> String:
	var p := SkydotWorld.godot_to_skyrim(_camera.global_position)
	return "at %.0f,%.0f,%.0f  look %.0f,%.0f" % [p.x, p.y, p.z,
		fposmod(-rad_to_deg(_yaw), 360.0), -rad_to_deg(_pitch)]


## Face the way the game's angles say: "Z,X" in degrees, Z clockwise from
## north, X positive looking down.
func _look_game(text: String) -> void:
	var parts := text.split(",")
	var pitch := -deg_to_rad(float(parts[1])) if parts.size() > 1 else 0.0
	_apply_look(-deg_to_rad(float(parts[0])), pitch)


## The scripts' state and where the camera is.
func _save_game(path: String) -> void:
	var state := {
		"format": 1,
		"papyrus": _papyrus.save_state(),
		"cell": _cell_id,
		"world": _world_id,
		"position": _camera.position,
		"yaw": _yaw,
		"pitch": _pitch,
	}
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		printerr("cannot save to ", path)
		return
	file.store_buffer(var_to_bytes(state))
	print("saved ", path)


func _load_game(path: String) -> bool:
	if not FileAccess.file_exists(path):
		printerr("no save at ", path)
		return false
	var state = bytes_to_var(FileAccess.get_file_as_bytes(path))
	if typeof(state) != TYPE_DICTIONARY or state.get("format", 0) != 1 \
			or typeof(state.get("papyrus")) != TYPE_PACKED_BYTE_ARRAY:
		printerr("not a save: ", path)
		return false
	if _papyrus.load_state(state["papyrus"]) != OK:
		printerr("cannot load the scripts' state: ", _papyrus.get_last_error())
		return false
	if int(state.get("world", 0)) != 0:
		_enter_exterior(int(state["world"]), state["position"], null)
	else:
		_enter_interior(int(state["cell"]), state["position"], null)
	_apply_look(float(state["yaw"]), float(state["pitch"]))
	print("loaded ", path)
	return true


func _take_screenshots() -> void:
	# Let shaders compile and textures stream in before capturing.
	_frames += 1
	if _frames < 30:
		return
	var index := _shot_index
	_shot_index += 1
	var image := get_viewport().get_texture().get_image()
	var path := _shot_path.get_basename() + "_%d.png" % index
	image.save_png(path)
	print("screenshot: ", path)
	_shots.pop_front()
	if _shots.is_empty():
		get_tree().quit(0)
		return
	if _shots[0] != null:
		_apply_look(_shots[0], -0.15)
	_frames = 20


## F12: the frame as a PNG and a JSON file describing it. The PNG is written
## on a worker thread so the frame does not hitch.
func _capture_shot(hide_overlay: bool) -> void:
	if _shot_busy:
		return
	_shot_busy = true
	if hide_overlay:
		_overlay.visible = false
		await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	_overlay.visible = true
	var dir: String = _args.get("shot-dir", "user://screenshots")
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
		_note("screenshot: no image here, wrote " + ProjectSettings.globalize_path(base + ".json"))
		_shot_busy = false
		return
	WorkerThreadPool.add_task(func() -> void: image.save_png(png))
	_note("screenshot: " + ProjectSettings.globalize_path(png))
	if _args.get("shot-notes", "on") == "off" or DisplayServer.get_name() == "headless":
		_shot_busy = false
		return
	_ask_shot_note(base + ".json")


## Pause and ask what the shot shows; `_end_shot_note` stores the answer.
func _ask_shot_note(json_path: String) -> void:
	if _shot_note == null:
		_shot_note = PanelContainer.new()
		_shot_note.process_mode = Node.PROCESS_MODE_ALWAYS
		_shot_note.custom_minimum_size = Vector2(640, 0)
		var box := VBoxContainer.new()
		var label := Label.new()
		label.text = "What is wrong in this shot? Enter saves, Shift+Enter new line, Escape skips."
		box.add_child(label)
		_shot_note_text = TextEdit.new()
		_shot_note_text.custom_minimum_size = Vector2(0, 96)
		_shot_note_text.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
		_shot_note_text.gui_input.connect(_shot_note_input)
		box.add_child(_shot_note_text)
		_shot_note.add_child(box)
		_overlay.add_child(_shot_note)
		_shot_note.set_anchors_and_offsets_preset(Control.PRESET_CENTER_BOTTOM,
			Control.PRESET_MODE_MINSIZE, 24)
		_shot_note.grow_horizontal = Control.GROW_DIRECTION_BOTH
		_shot_note.grow_vertical = Control.GROW_DIRECTION_BEGIN
	_shot_note_json = json_path
	_shot_note_mouse = Input.mouse_mode
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	get_tree().paused = true  # time, weather and the player wait
	_shot_note_text.text = ""
	_shot_note.visible = true
	_shot_note_text.grab_focus()


func _shot_note_input(event: InputEvent) -> void:
	if not (event is InputEventKey and event.pressed and not event.echo):
		return
	if event.keycode == KEY_ESCAPE:
		_end_shot_note(false)
	elif event.keycode in [KEY_ENTER, KEY_KP_ENTER] and not event.shift_pressed:
		_end_shot_note(true)
	else:
		return
	_shot_note_text.accept_event()


func _end_shot_note(save: bool) -> void:
	var text := _shot_note_text.text.strip_edges()
	if save and not text.is_empty():
		var meta = JSON.parse_string(FileAccess.get_file_as_string(_shot_note_json))
		var file := FileAccess.open(_shot_note_json, FileAccess.WRITE) if meta is Dictionary else null
		if file != null:
			meta["note"] = text
			file.store_string(JSON.stringify(meta, "  ") + "\n")
			file.close()
			_note("note saved")
		else:
			_note("note not saved: cannot write " + ProjectSettings.globalize_path(_shot_note_json))
	_shot_note.visible = false
	_shot_note_text.release_focus()
	get_tree().paused = false
	Input.mouse_mode = _shot_note_mouse
	_shot_busy = false


## Everything needed to come back to this view, in the viewer or the game.
func _shot_metadata(overlay_hidden: bool) -> Dictionary:
	var eye := SkydotWorld.godot_to_skyrim(_camera.global_position)
	var heading := fposmod(-rad_to_deg(_yaw), 360.0)
	var tilt := -rad_to_deg(_pitch)
	var place := {}
	var console: Array[String] = []
	if _world_id != 0:
		var world_name := ""
		for w in _world.list_worlds():
			if w["id"] == _world_id:
				world_name = w["editor_id"]
		var grid := Vector2i(floori(eye.x / CELL_UNITS), floori(eye.y / CELL_UNITS))
		place = {"kind": "exterior", "world": world_name, "world_id": "0x%08X" % _world_id,
			"grid": [grid.x, grid.y]}
		console.append("cow %s %d %d" % [world_name, grid.x, grid.y])
	else:
		var cell := _world.get_cell(_cell_id)
		place = {"kind": "interior", "cell": cell.get("editor_id", ""), "cell_id": "0x%08X" % _cell_id}
		console.append("coc " + str(cell.get("editor_id", "")))
	# The game's getpos is at the feet; the first-person eye is about 120
	# units higher (GAME-COMPARISON.md).
	console.append_array(["player.setpos x %.0f" % eye.x, "player.setpos y %.0f" % eye.y,
		"player.setpos z %.0f" % (eye.z - 120.0), "player.setangle z %.0f" % heading,
		"player.setangle x %.0f" % tilt])
	var time := {}
	var weather := {}
	if _weather != null and is_instance_valid(_weather):
		var state := _weather.get_state()
		time = {"hour": _weather.hour, "day": _weather.day, "time_scale": _weather.time_scale}
		weather = {"id": "0x%08X" % int(state.get("weather", 0)), "editor_id": state.get("editor_id", ""),
			"transition": state.get("transition", 1.0), "auto": _weather.auto_weather}
		console.append("set gamehour to %.2f" % _weather.hour)
		if int(state.get("weather", 0)) != 0:
			console.append("sw %X" % int(state["weather"]))
	else:
		time = {"hour": _hour, "day": _day}  # inside: kept for when one leaves
	console.append("tm")
	var pack_dir: String = _args.get("pack", "")
	var manifest = JSON.parse_string(FileAccess.get_file_as_string(pack_dir.path_join("manifest.json")))
	var pack := {"path": ProjectSettings.globalize_path(pack_dir)}
	if manifest is Dictionary:
		pack["converter"] = manifest.get("converter", "")
		pack["records_hash"] = manifest.get("records", {}).get("hash", "")
		pack["world_hash"] = manifest.get("world", {}).get("hash", "")
		pack["input"] = manifest.get("input", {})
	var size := get_viewport().get_visible_rect().size
	return {
		"format": SHOT_FORMAT,
		"taken": Time.get_datetime_string_from_system(),
		"place": place,
		"camera": {
			"game": {"x": eye.x, "y": eye.y, "z": eye.z, "heading": heading, "tilt": tilt},
			"engine": {"position": [_camera.global_position.x, _camera.global_position.y,
				_camera.global_position.z], "yaw": _yaw, "pitch": _pitch},
			"fov": _camera.fov,
			"resolution": [int(size.x), int(size.y)],
		},
		"time": time,
		"weather": weather,
		"viewer": {
			"flying": _player.fly,
			"materials": _world.skyrim_materials,
			"effects": _world.effects,
			"collision": _world.collision,
			"navigation": _world.navigation,
			"actors": _world.actors,
			"wander": _world.actor_wander,
			"navmesh_shown": _show_navmesh,
			"lod": _lod != null,
			"radius": _radius,
			"lod_split": _lod_split,
			"msaa": MSAA_NAMES[maxi(MSAA_STEPS.find(get_viewport().msaa_3d), 0)],
			"quests": _args.get("quests", "on") != "off",
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


## Viewer arguments that come back to a shot's view (--from-shot), with time
## stopped there. {} if the file is not a shot.
func _args_from_shot(path: String) -> Dictionary:
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
	var time: Dictionary = shot.get("time", {})
	if time.has("hour"):
		out["time"] = str(time["hour"])
	out["time-scale"] = "0"
	var weather: Dictionary = shot.get("weather", {})
	if not str(weather.get("editor_id", "")).is_empty():
		out["weather"] = weather["editor_id"]
	var viewer: Dictionary = shot.get("viewer", {})
	if viewer.get("flying", false):
		out["walk"] = "off"
	if viewer.has("radius"):
		out["radius"] = str(viewer["radius"])
	if viewer.has("lod_split"):
		out["lod-split"] = str(viewer["lod_split"])
	if viewer.has("msaa"):
		out["msaa"] = str(viewer["msaa"])
	if viewer.get("quests", true) == false:
		out["quests"] = "off"
	if viewer.get("materials", true) == false:
		out["materials"] = "off"
	if not str(shot.get("note", "")).is_empty():
		out["note"] = shot["note"]
	return out


func _vec3(text: String) -> Vector3:
	var parts := text.split(",")
	return Vector3(float(parts[0]), float(parts[1]), float(parts[2]))


func _parse_args(list: PackedStringArray) -> Dictionary:
	var out := {}
	var i := 0
	while i < list.size():
		var key := list[i]
		if key.begins_with("--") and i + 1 < list.size():
			out[key.substr(2)] = list[i + 1]
			i += 2
		else:
			i += 1
	return out


func _fail(message: String) -> void:
	push_error(message)
	printerr(message)
	get_tree().quit(1)
