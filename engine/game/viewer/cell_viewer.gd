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
# cells around the camera (2 is the game's uGridsToLoad 5). Between them and
# the LOD the worldspace's large references (cliffs, rocks, big buildings; pack
# format 11) show as models out to --large-ref-radius cells (default 5, the
# game's uLargeRefLODGridSize 11); --large-refs off shows only the LOD there
# (SkydotLargeRefs, engine/docs/lod.md). --msaa off|2|4|8 smooths edges (multisampling;
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
# behind it is built ahead, held hidden in the scene (SkydotStreamer): an interior, or
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
# --screenshot, --benchmark, --activate and --no-input (a flag: it takes no value) runs ignore the keyboard
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
#
# This script only puts the viewer together and runs its frame; the parts are
# next to it: ViewerSettings (the options), PlayerRig and PlayerInput, Place
# and PlaceTransition, ScriptBridge, SaveService, ShotRecorder, DebugOverlay
# and BenchmarkRun. The time of day (SkydotClock) and the streaming of the
# world with its door preloading (SkydotStreamer) are the engine's.
extends Node3D

var _settings: ViewerSettings
var _pack: SkydotPack
var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _ai: SkydotAi  # null with --ai off
var _image_space: SkydotImageSpace
var _clock: SkydotClock
var _rig: PlayerRig
var _streamer: SkydotStreamer
var _place: Place
var _transition: PlaceTransition
var _bridge: ScriptBridge
var _controls: PlayerInput
var _debug: DebugOverlay  # notes, journal, navmesh, path; Shift+F12 hides it for a shot
var _saves: SaveService
var _shots: ShotRecorder
var _benchmark := BenchmarkRun.new()
var _quit_in := -1  # frames until quitting after --activate

## Off for runs that drive themselves (tools set it): the keyboard and mouse
## are ignored and doors travel without a fade.
var _input := true:
	set(value):
		_input = value
		if _transition != null:
			_transition.interactive = value
		if _controls != null:
			_controls.enabled = value


func _ready() -> void:
	_settings = ViewerSettings.from_arguments(OS.get_cmdline_user_args())
	if not _settings.shot_note.is_empty():
		print("shot note: ", _settings.shot_note)
	if not _settings.error.is_empty():
		_fail(_settings.error)
		return
	_clock = SkydotClock.new()
	_clock.setup(_settings.time, _settings.time_scale, _settings.captures)
	if not _open_world():
		return
	_create_components()
	_start_game()


## Open the pack and its world and set what is built and who runs it.
func _open_world() -> bool:
	_pack = SkydotPack.new()
	if _pack.open(_settings.pack) != OK:
		_fail(_pack.get_error())
		return false
	if _settings.has_pck:
		push_warning("--pck is ignored: packs load directly, there is no bake any more")
	_world = _pack.open_world()
	if _world == null:
		_fail(_pack.get_error())
		return false
	_papyrus = SkydotPapyrus.new()
	_papyrus.setup(_pack, _world)
	_world.skyrim_materials = _settings.materials
	_world.effects = _settings.effects
	_world.grass = _settings.grass
	_world.all_light_shadows = _settings.all_light_shadows
	_world.collision = _settings.collision
	_world.navigation = _settings.navigation
	_world.actors = _settings.actors
	_world.actor_wander = _settings.wander
	if _settings.ai:
		_ai = SkydotAi.new()
		if _ai.setup(_world, _papyrus) == OK:
			_clock.attach_ai(_ai)
			_ai.drive = _world.actor_wander
			_ai.actor_left.connect(_on_actor_left)
		else:
			_ai = null
	get_viewport().msaa_3d = ViewerSettings.MSAA_STEPS[_settings.msaa_index]
	if _settings.has_tiling:
		_world.terrain_tiling = _settings.tiling
	# Runs that capture, measure or activate on their own ignore the keyboard
	# and mouse, so a stray touch cannot move the view.
	_input = _settings.interactive
	return true


## Make the parts and connect them. They are added to the scene in the order
## the frame needs: camera and player, then what draws over them.
func _create_components() -> void:
	_image_space = SkydotImageSpace.new()
	_rig = PlayerRig.new(_settings, _image_space)
	_rig.fell_through.connect(func() -> void: _debug.note("fell through the world: flying (V walks)"))
	add_child(_rig.camera)
	add_child(_rig.player)
	_make_streamer()
	_debug = DebugOverlay.new()
	_debug.setup(self, _world, _papyrus, _ai, _rig.camera, _rig.player)
	add_child(_debug)
	_place = Place.new(_world, self, _settings, _rig, _ai, _clock, _streamer)
	_place.built.connect(_on_interior_built)
	_place.left.connect(_image_space.reset_adaptation)
	_place.left.connect(_debug.clear_path)
	_place.message.connect(_debug.note)
	_place.failed.connect(_fail)
	if _ai != null:
		_ai.actor_arrived.connect(_place.on_actor_arrived)
	_transition = PlaceTransition.new(_place, _streamer)
	_transition.interactive = _input
	add_child(_transition)
	_controls = PlayerInput.new(_rig)
	_controls.enabled = _input
	_controls.command.connect(_on_command)
	add_child(_controls)
	_bridge = ScriptBridge.new(_world, _papyrus, self, _rig.camera, _settings, _place, _streamer, _debug,
		_transition)
	_bridge.activations_finished.connect(func() -> void: _quit_in = 5)  # let scripts run first
	_bridge.failed.connect(_fail)
	_saves = SaveService.new(_papyrus, _place, _streamer, _rig)
	_shots = ShotRecorder.new(self, _settings, _world, _rig, _place, _streamer, _clock, _debug)
	_controls.capture_mouse()


## The streaming of exterior cells and the building ahead behind load doors,
## as the options say.
func _make_streamer() -> void:
	_streamer = SkydotStreamer.new()
	_streamer.setup(_world, _pack, self, _rig.camera, _ai)
	_streamer.radius = _settings.radius
	_streamer.build_budget_usec = _settings.build_budget_usec
	_streamer.lod_enabled = _settings.lod
	_streamer.lod_split = _settings.lod_split
	_streamer.large_refs = _settings.large_refs
	_streamer.large_ref_radius = _settings.large_ref_radius
	if _settings.has_tree_distance:
		_streamer.set_tree_distance(_settings.tree_distance)
	_streamer.eye_height = PlayerRig.EYE_HEIGHT
	_streamer.preload_enabled = _settings.preload_doors
	_streamer.preload_distance = _settings.preload_distance
	_streamer.cell_finished.connect(_on_cell_built)


## Start the quests and enter the place the options name.
func _start_game() -> void:
	var settings := _settings
	if settings.quests:
		var started := _papyrus.start_game_enabled_quests()
		print("quests: %d of %d started with the game" % [started, _world.get_quest_count()])
	_bridge.quests_ready = true
	if not settings.set_stage.is_empty():
		for item in settings.set_stage.split(","):
			var parts: PackedStringArray = item.split(":")
			var quest := _world.find_quest(parts[0])
			if quest == 0 or parts.size() != 2:
				_fail("--set-stage wants EDID:STAGE, got " + item)
				return
			if not _papyrus.set_stage(quest, int(parts[1])):
				print("%s: stage %s refused" % [parts[0], parts[1]])

	if not settings.world.is_empty():
		var world_id := _world.find_world(settings.world)
		if world_id == 0:
			_fail("no worldspace named " + settings.world)
			return
		if not settings.has_at:
			_fail("--world needs --at X,Y,Z")
			return
		var started := Time.get_ticks_usec()
		var variants := _world.warm_up()
		print("warm_up: %d shader variants in %.0f ms" % [variants, (Time.get_ticks_usec() - started) / 1000.0])
		var at := SkydotWorld.skyrim_position(settings.at)
		var target = SkydotWorld.skyrim_position(settings.target) if settings.has_target else null
		if not _place.enter_exterior(world_id, at, target):
			return
		if settings.has_look:
			_rig.apply_look(settings.look_yaw, settings.look_pitch)
		_controls.fly_speed = 10.0
		if settings.has_benchmark:
			_benchmark.begin(settings.benchmark, settings.fly_speed)
			_streamer.max_usec = 0
			_streamer.load_everything()
		if settings.has_screenshot:
			# Everything in range first, so the capture is complete.
			_streamer.load_everything()
			if _streamer.lod != null:
				print("lod: ", _streamer.lod.get_stats())
			_shots.begin_run(settings.screenshot, [NAN])
		return

	var cell_id := _world.find_cell(settings.cell)
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


func _process(delta: float) -> void:
	if _rig == null:
		return
	_sync_image_space()
	if _shots.is_running():
		_shots.step(delta)
		return
	_streamer.hold_player(_rig.player, _transition.is_fading())
	_rig.track()
	_rig.follow()
	_streamer.update()
	_streamer.preload_step(_rig.player.global_position)
	if _transition.is_fading():
		_transition.step(delta)
	_papyrus.update_actor(SkydotPapyrus.PLAYER_REF,
		SkydotWorld.godot_to_skyrim(_rig.player.global_position))
	_papyrus.update(delta)
	if _ai != null:
		_clock.sync(_benchmark.is_running())
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
	if _benchmark.is_running():
		if _benchmark.frame(delta, _rig.player, _streamer.max_usec):
			get_tree().quit(0)
		return
	_controls.apply()


## The image space the camera grades with: the weather's outside, the cell's
## inside, none with --image-space off. Our shaders compute the fog
## themselves.
func _sync_image_space() -> void:
	SkydotMaterials.sync_fog(get_viewport().find_world_3d().environment)
	if not _settings.image_space:
		_image_space.clear()
	elif _clock.has_weather():
		_image_space.set_image_space(_clock.weather.get_image_space())
	else:
		_image_space.set_image_space(_place.interior_image_space)


## A cell was built and shown (streamed or an interior): its scripts attach and
## the navmesh overlay is brought up to date.
func _on_cell_built(cell: Node3D, cell_id: int) -> void:
	_bridge.scripts_loaded(cell, cell_id)
	_debug.navmesh_overlay(cell)


## The streamer registers the load doors of the cells it builds; an interior is
## built by the place.
func _on_interior_built(cell: Node3D, cell_id: int) -> void:
	_on_cell_built(cell, cell_id)
	_streamer.register_doors(cell)


func _on_actor_left(ref: int, _door: int) -> void:
	print("0x%08X leaves through a door" % ref)


## A key that is not about the player's body.
func _on_command(action: StringName, shift: bool) -> void:
	match action:
		PlayerInput.SHOW_POSITION:
			_debug.note(_debug.position_text(_rig.yaw, _rig.pitch))
		PlayerInput.TOGGLE_PRELOAD:
			_debug.note("load doors preload what is behind them" if _streamer.toggle_preload() else "load doors load on use")
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
			_debug.note(_lod_text(_streamer.scale_lod_split(0.8)))
		PlayerInput.LOD_DETAIL_UP:
			_debug.note(_lod_text(_streamer.scale_lod_split(1.25)))
		PlayerInput.RADIUS_DOWN:
			if _streamer.world_id != 0:
				_debug.note(_radius_text(_streamer.change_radius(-1)))
		PlayerInput.RADIUS_UP:
			if _streamer.world_id != 0:
				_debug.note(_radius_text(_streamer.change_radius(1)))
		PlayerInput.CYCLE_MSAA:
			var next: int = (ViewerSettings.MSAA_STEPS.find(get_viewport().msaa_3d) + 1) % ViewerSettings.MSAA_STEPS.size()
			get_viewport().msaa_3d = ViewerSettings.MSAA_STEPS[next]
			_debug.note("MSAA " + ("off" if next == 0 else ViewerSettings.MSAA_NAMES[next] + "x"))
		PlayerInput.TOGGLE_FLY:
			_debug.note("flying" if _rig.toggle_fly() else "walking")


## [ and ]: what to tell the user about the LOD's detail. A level-8 quad splits
## into level-4 ones within split times 8 cells.
func _lod_text(split: float) -> String:
	return "LOD detail %.2f (finest LOD within %.0f m)" % [split, split * 8 * SkydotWorld.CELL_UNITS * SkydotWorld.unit_scale()]


## - and =: what to tell the user about the cells in full detail.
func _radius_text(radius: int) -> String:
	return "full detail within %d cells (%d x %d)" % [radius, 2 * radius + 1, 2 * radius + 1]


func _fail(message: String) -> void:
	push_error(message)
	printerr(message)
	get_tree().quit(1)
