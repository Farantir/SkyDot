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

var _rig: PlayerRig
var _camera: Camera3D  # _rig's
var _player: SkydotPlayer  # _rig's; the camera follows its eyes
var _controls: PlayerInput
var _shots: ShotRecorder

var _settings: ViewerSettings
var _world: SkydotWorld
var _streamer: WorldStreamer
var _place: Place
var _benchmark := 0.0
var _fly_speed := 20.0
var _frame_times: Array[float] = []
## Off for runs that drive themselves (tools set it): the keyboard and mouse
## are ignored and doors travel without a fade.
var _input := true:
	set(value):
		_input = value
		if _transition != null:
			_transition.interactive = value
		if _controls != null:
			_controls.enabled = value
var _quit_in := -1  # frames until quitting after --activate
var _papyrus: SkydotPapyrus
var _ai: SkydotAi  # null with --ai off
var _pack: SkydotPack
var _preloader: DoorPreloader
var _transition: PlaceTransition
var _bridge: ScriptBridge
var _saves: SaveService
const NOTE_SECONDS := 8.0
var _clock: GameClock
var _debug: DebugOverlay  # notes, journal, navmesh, path; Shift+F12 hides it for a shot
var _image_space: SkydotImageSpace


func _ready() -> void:
	_settings = ViewerSettings.from_arguments(OS.get_cmdline_user_args())
	var settings := _settings
	if not settings.shot_note.is_empty():
		print("shot note: ", settings.shot_note)
	if not settings.error.is_empty():
		_fail(settings.error)
		return
	_clock = GameClock.new(settings.time, settings.time_scale, settings.captures)

	var pack := SkydotPack.new()
	if pack.open(settings.pack) != OK:
		_fail(pack.get_error())
		return
	if settings.has_pck:
		push_warning("--pck is ignored: packs load directly, there is no bake any more")
	var world := pack.open_world()
	if world == null:
		_fail(pack.get_error())
		return
	_world = world
	_pack = pack
	_papyrus = SkydotPapyrus.new()
	_papyrus.setup(pack, world)
	world.skyrim_materials = settings.materials
	world.effects = settings.effects
	world.grass = settings.grass
	world.all_light_shadows = settings.all_light_shadows
	world.collision = settings.collision
	world.navigation = settings.navigation
	world.actors = settings.actors
	world.actor_wander = settings.wander
	if settings.ai:
		_ai = SkydotAi.new()
		if _ai.setup(world, _papyrus) == OK:
			_clock.attach_ai(_ai)
			_ai.drive = world.actor_wander
			_ai.actor_left.connect(func(ref: int, _door: int) -> void:
				print("0x%08X leaves through a door" % ref))
		else:
			_ai = null
	get_viewport().msaa_3d = ViewerSettings.MSAA_STEPS[settings.msaa_index]
	if settings.has_tiling:
		world.terrain_tiling = settings.tiling
	# Runs that capture, measure or activate on their own ignore the keyboard
	# and mouse, so a stray touch cannot move the view.
	_input = settings.interactive

	_image_space = SkydotImageSpace.new()
	_rig = PlayerRig.new(settings, _image_space)
	_rig.fell_through.connect(func() -> void: _debug.note("fell through the world: flying (V walks)"))
	_camera = _rig.camera
	_player = _rig.player
	add_child(_camera)
	add_child(_player)
	_streamer = WorldStreamer.new(world, pack, self, _camera, _ai, settings)
	_streamer.cell_finished.connect(_on_cell_finished)
	_preloader = DoorPreloader.new(world, self, _streamer, _ai, settings)
	_debug = DebugOverlay.new()
	_debug.setup(self, world, _papyrus, _ai, _camera, _player)
	add_child(_debug)
	_place = Place.new(world, self, settings, _rig, _ai, _clock, _streamer, _preloader)
	_place.built.connect(_on_cell_finished)
	_place.left.connect(_image_space.reset_adaptation)
	_place.left.connect(_debug.clear_path)
	_place.message.connect(_debug.note)
	_place.failed.connect(_fail)
	if _ai != null:
		_ai.actor_arrived.connect(_place.on_actor_arrived)
	_transition = PlaceTransition.new(_place, _preloader, _streamer)
	_transition.interactive = _input
	add_child(_transition)
	_controls = PlayerInput.new(_rig)
	_controls.enabled = _input
	_controls.command.connect(_on_command)
	add_child(_controls)
	_bridge = ScriptBridge.new(world, _papyrus, self, _camera, settings, _place, _streamer, _debug,
		_transition)
	_bridge.activations_finished.connect(func() -> void: _quit_in = 5)  # let scripts run first
	_bridge.failed.connect(_fail)
	_saves = SaveService.new(_papyrus, _place, _streamer, _rig)
	_shots = ShotRecorder.new(self, settings, world, _rig, _place, _streamer, _clock, _debug)
	_controls.capture_mouse()

	if settings.quests:
		var started := _papyrus.start_game_enabled_quests()
		print("quests: %d of %d started with the game" % [started, world.get_quest_count()])
	_bridge.quests_ready = true
	if not settings.set_stage.is_empty():
		for item in settings.set_stage.split(","):
			var parts: PackedStringArray = item.split(":")
			var quest := world.find_quest(parts[0])
			if quest == 0 or parts.size() != 2:
				_fail("--set-stage wants EDID:STAGE, got " + item)
				return
			if not _papyrus.set_stage(quest, int(parts[1])):
				print("%s: stage %s refused" % [parts[0], parts[1]])

	if not settings.world.is_empty():
		var world_id := world.find_world(settings.world)
		if world_id == 0:
			_fail("no worldspace named " + settings.world)
			return
		if not settings.has_at:
			_fail("--world needs --at X,Y,Z")
			return
		var started := Time.get_ticks_usec()
		var variants := world.warm_up()
		print("warm_up: %d shader variants in %.0f ms" % [variants, (Time.get_ticks_usec() - started) / 1000.0])
		var at := SkydotWorld.skyrim_position(settings.at)
		var target = SkydotWorld.skyrim_position(settings.target) if settings.has_target else null
		if not _place.enter_exterior(world_id, at, target):
			return
		if settings.has_look:
			_rig.apply_look(settings.look_yaw, settings.look_pitch)
		_controls.fly_speed = 10.0
		if settings.has_benchmark:
			_benchmark = settings.benchmark
			_streamer.max_usec = 0
			_fly_speed = settings.fly_speed
			_streamer.load_everything()
		if settings.has_screenshot:
			# Everything in range first, so the capture is complete.
			_streamer.load_everything()
			if _streamer.lod != null:
				print("lod: ", _streamer.lod.get_stats())
			_shots.begin_run(settings.screenshot, [NAN])
		return

	var cell_id := world.find_cell(settings.cell)
	if cell_id == 0:
		_fail("no cell named " + settings.cell)
		return
	var fixed_view := settings.has_at and settings.has_target
	if fixed_view:
		_place.enter_interior(cell_id, SkydotWorld.skyrim_position(settings.at),
			SkydotWorld.skyrim_position(settings.target))
	else:
		_place.enter_interior(cell_id, SkydotWorld.skyrim_position(settings.at) if settings.has_at else null, null)
	if settings.has_look:
		_rig.apply_look(settings.look_yaw, settings.look_pitch)
	if not settings.load_path.is_empty():
		_saves.load_game(settings.load_path)

	if settings.has_screenshot:
		if fixed_view or (settings.has_at and settings.has_look):  # --look: a shot's view
			_shots.begin_run(settings.screenshot, [NAN])
		else:
			var yaws: Array[float] = []
			for i in 4:
				yaws.append(i * PI / 2.0)
			_shots.begin_run(settings.screenshot, yaws)


## Hold the player while the ground under it is still being built, keep its
## water level, and catch it if it falls through the world.
func _update_player() -> void:
	_streamer.hold_player(_player, _transition.is_fading())
	_rig.track()


## A streamed cell is built and shown: its scripts attach, the navmesh
## overlay and the load doors are brought up to date.
func _on_cell_finished(cell: Node3D, cell_id: int) -> void:
	_bridge.scripts_loaded(cell, cell_id)
	_debug.navmesh_overlay(cell)
	_preloader.register(Place.ref_nodes(cell))


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
		   sorted.filter(func(t: float) -> bool: return t > 33.0).size(), _streamer.max_usec / 1000.0])
	get_tree().quit(0)


func _process(delta: float) -> void:
	if _camera == null:
		return
	# Our shaders compute the fog themselves (SkydotMaterials.sync_fog).
	SkydotMaterials.sync_fog(get_viewport().find_world_3d().environment)
	if not _settings.image_space:
		_image_space.clear()
	elif _clock.has_weather():
		_image_space.set_image_space(_clock.weather.get_image_space())
	else:
		_image_space.set_image_space(_place.interior_image_space)
	if _shots.is_running():
		_shots.step(delta)
		return
	_update_player()
	_rig.follow()
	_streamer.update()
	if _preloader.enabled:
		_preloader.step(_player.global_position)
	if _transition.is_fading():
		_transition.step(delta)
	_papyrus.update_actor(SkydotPapyrus.PLAYER_REF,
		SkydotWorld.godot_to_skyrim(_player.global_position))
	_papyrus.update(delta)
	if _ai != null:
		_clock.sync(_benchmark > 0.0)
		_ai.update(delta)
	_debug.age(delta)
	if _quit_in >= 0:
		_quit_in -= 1
		if _quit_in < 0:
			if not _settings.save_to.is_empty():
				_saves.save(_settings.save_to)
			get_tree().quit(0)
		return
	if _bridge.has_activations():
		_bridge.run_activation()
		return
	if _benchmark > 0.0:
		_benchmark_frame(delta)
		return
	_controls.apply()


## A key that is not about the player's body.
func _on_command(action: StringName, shift: bool) -> void:
	match action:
		PlayerInput.SHOW_POSITION:
			_debug.note(_debug.position_text(_rig.yaw, _rig.pitch))
		PlayerInput.TOGGLE_PRELOAD:
			_debug.note("load doors preload what is behind them" if _preloader.toggle() else "load doors load on use")
		PlayerInput.TAKE_SHOT:
			_shots.capture(shift)
		PlayerInput.ACTIVATE:
			if not _transition.is_fading():
				_bridge.activate_in_view(shift)  # Shift opens locks
		PlayerInput.QUICK_SAVE:
			_saves.save(SaveService.QUICKSAVE)
		PlayerInput.QUICK_LOAD:
			_saves.load_game(SaveService.QUICKSAVE)
		PlayerInput.SHOW_JOURNAL:
			_debug.toggle_journal()
		PlayerInput.CHANGE_TIME:
			var time_text := _clock.shift(-1.0 if shift else 1.0)
			if not time_text.is_empty():
				_debug.note(time_text)
		PlayerInput.INSPECT_ACTOR:
			_debug.inspect_actor()
		PlayerInput.NEXT_WEATHER:
			if _clock.has_weather():
				_clock.weather.next_weather()
		PlayerInput.TOGGLE_NAVMESH:
			_debug.toggle_navmesh()
		PlayerInput.SHOW_PATH:
			_debug.path_to_view()
		PlayerInput.LOD_DETAIL_DOWN:
			_debug.note(_streamer.scale_lod_split(0.8))
		PlayerInput.LOD_DETAIL_UP:
			_debug.note(_streamer.scale_lod_split(1.25))
		PlayerInput.RADIUS_DOWN:
			if _streamer.world_id != 0:
				_debug.note(_streamer.change_radius(-1))
		PlayerInput.RADIUS_UP:
			if _streamer.world_id != 0:
				_debug.note(_streamer.change_radius(1))
		PlayerInput.CYCLE_MSAA:
			var next: int = (ViewerSettings.MSAA_STEPS.find(get_viewport().msaa_3d) + 1) % ViewerSettings.MSAA_STEPS.size()
			get_viewport().msaa_3d = ViewerSettings.MSAA_STEPS[next]
			_debug.note("MSAA " + ("off" if next == 0 else ViewerSettings.MSAA_NAMES[next] + "x"))
		PlayerInput.TOGGLE_FLY:
			_debug.note("flying" if _rig.toggle_fly() else "walking")


func _fail(message: String) -> void:
	push_error(message)
	printerr(message)
	get_tree().quit(1)
