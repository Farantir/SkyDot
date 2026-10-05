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
var _speed := 3.0
var _shots: Array = []
var _shot_path := ""
var _shot_delay := 0.0  # seconds the world runs before --screenshot captures
var _frames := 0
var _shot_index := 0

const REACH := 2.6  # metres the camera can activate from
var _settings: ViewerSettings
var _world: SkydotWorld
var _streamer: WorldStreamer
var _place: Place
var _benchmark := 0.0
var _fly_speed := 20.0
var _frame_times: Array[float] = []
var _pick_locks := false
## Off for runs that drive themselves (tools set it): the keyboard and mouse
## are ignored and doors travel without a fade.
var _input := true:
	set(value):
		_input = value
		if _transition != null:
			_transition.interactive = value
var _script_activations: Array = []  # --activate: refs still to activate
var _script_wait := 0
var _quit_in := -1  # frames until quitting after --activate
var _papyrus: SkydotPapyrus
var _ai: SkydotAi  # null with --ai off
var _pack: SkydotPack
var _preloader: DoorPreloader
var _transition: PlaceTransition
var _quests_ready := false  # after the start-game quests have started
const NOTE_SECONDS := 8.0
const QUICKSAVE := "user://quicksave.skydot"
var _clock: GameClock
var _debug: DebugOverlay  # notes, journal, navmesh, path; Shift+F12 hides it for a shot
var _shot_busy := false
var _shot_note: PanelContainer  # asks for a shot's note
var _shot_note_text: TextEdit
var _shot_note_json := ""  # the shot the note goes to
var _shot_note_mouse := Input.MOUSE_MODE_VISIBLE  # restored afterwards
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
	_papyrus.message.connect(func(text: String, _box: bool) -> void: _debug.note(text))
	_papyrus.quest_started.connect(func(quest: int) -> void:
		if _quests_ready:
			print("quest started: ", _debug.quest_name(quest)))
	# Only stages with journal text reach the screen, as in the game.
	_papyrus.quest_stage.connect(func(quest: int, stage: int, text: String) -> void:
		if text != "":
			_debug.note("%s: %s" % [_debug.quest_name(quest), text])
		elif _quests_ready:
			print("%s: stage %d" % [_debug.quest_name(quest), stage]))
	_papyrus.objective_changed.connect(func(quest: int, index: int, state: String, text: String) -> void:
		_debug.note("%s objective %d %s: %s" % [_debug.quest_name(quest), index, state, text]))
	_papyrus.effect_shader.connect(func(shader: int, ref: int, playing: bool) -> void:
		print("effect shader 0x%08X %s on 0x%08X (not drawn yet)" % [shader, "plays" if playing else "stops", ref]))
	_papyrus.trigger.connect(func(ref: int, _actor: int, entered: bool) -> void:
		print("%s trigger 0x%08X" % ["entered" if entered else "left", ref]))
	world.skyrim_materials = settings.materials
	world.effects = settings.effects
	world.grass = settings.grass
	world.all_light_shadows = settings.all_light_shadows
	world.collision = settings.collision
	world.navigation = settings.navigation
	world.actors = settings.actors
	_shot_delay = settings.shot_delay
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
	_pick_locks = settings.pick_locks
	# Runs that capture, measure or activate on their own ignore the keyboard
	# and mouse, so a stray touch cannot move the view.
	_input = settings.interactive
	_script_activations = settings.activate.duplicate()

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
	if DisplayServer.get_name() != "headless" and _input:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

	if settings.quests:
		var started := _papyrus.start_game_enabled_quests()
		print("quests: %d of %d started with the game" % [started, world.get_quest_count()])
	_quests_ready = true
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
		_speed = 10.0
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
			_shot_path = settings.screenshot
			_shots.append(null)
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
		_load_game(settings.load_path)

	if settings.has_screenshot:
		_shot_path = settings.screenshot
		if fixed_view or (settings.has_at and settings.has_look):  # --look: a shot's view
			_shots.append(null)
		else:
			for i in 4:
				_shots.append(i * PI / 2.0)
			_rig.apply_look(_shots[0], -0.15)


## Hold the player while the ground under it is still being built, keep its
## water level, and catch it if it falls through the world.
func _update_player() -> void:
	_streamer.hold_player(_player, _transition.is_fading())
	_rig.track()


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
		for node in Place.ref_nodes(root):
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
			_place.add(holder)


## Enable or disable a built reference: shown and solid, or neither (a
## disabled node's bodies leave the physics space).
func _show_ref(node: Node, enabled: bool) -> void:
	node.visible = enabled
	node.process_mode = Node.PROCESS_MODE_INHERIT if enabled else Node.PROCESS_MODE_DISABLED


## Clutter resting on or against `node` falls once it is gone or moves.
func _wake_around(node: Node) -> void:
	var bounds := Place.mesh_bounds(node)
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
		_debug.note(label + " only responds to its activate parents")
		return true
	var level := _papyrus.get_lock_level(ref)  # -1: not locked; 0 is Novice
	if level >= 0 and not force and not _pick_locks:
		_debug.note(label + " is locked (level %d, %s); Shift+F opens it anyway" % [level, _lock_name(level)])
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
		_debug.note(label + ": activation blocked, only its scripts ran")
		return true
	if info["door"] != null:
		print(label, " leads to 0x%08X" % info["door"]["destination"])
		_transition.go(info["door"])
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
		_debug.note(label + ": nothing happens")
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
	var pending: Array = _place.nodes.duplicate()
	pending.append_array(_streamer.loaded.values())
	while not pending.is_empty():
		var node = pending.pop_back()
		if node == null or not is_instance_valid(node) or node.is_queued_for_deletion() or node == _streamer.lod:
			continue
		for child in node.get_children():
			if child.get_meta("skydot_ref", 0) == ref:
				return child
			if not child.has_meta("skydot_ref") and child.get_child_count() > 0:
				pending.append(child)
	return null


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
		_debug.note("nothing to activate")
		return
	_activate(hit["cell"], hit["ref"], hit["node"], force)


## A streamed cell is built and shown: its scripts attach, the navmesh
## overlay and the load doors are brought up to date.
func _on_cell_finished(cell: Node3D, cell_id: int) -> void:
	_scripts_loaded(cell, cell_id)
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
	if _shot_path != "":
		if _shot_delay > 0.0:
			_shot_delay -= delta
			return
		_take_screenshots()
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
				_save_game(_settings.save_to)
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
		_player.set_look(_rig.yaw, _rig.pitch)
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
	_player.set_look(_rig.yaw, _rig.pitch)


func _unhandled_input(event: InputEvent) -> void:
	if not _input:
		return
	var captured := Input.mouse_mode == Input.MOUSE_MODE_CAPTURED
	if event is InputEventMouseMotion and (captured or Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)):
		_rig.apply_look(_rig.yaw - event.relative.x * 0.004,
			clamp(_rig.pitch - event.relative.y * 0.004, -1.5, 1.5))
	elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT and not captured:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_ESCAPE:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_P:
		_debug.note(_debug.position_text(_rig.yaw, _rig.pitch))
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_O:
		_debug.note("load doors preload what is behind them" if _preloader.toggle() else "load doors load on use")
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F12:
		_capture_shot(event.shift_pressed)
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F:
		if not _transition.is_fading():
			_activate_in_view(event.shift_pressed)
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F5:
		_save_game(QUICKSAVE)
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F9:
		_load_game(QUICKSAVE)
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_J:
		_debug.toggle_journal()
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_SPACE:
		_player.jump()
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_T:
		var time_text := _clock.shift(-1.0 if event.shift_pressed else 1.0)
		if not time_text.is_empty():
			_debug.note(time_text)
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_I:
		_debug.inspect_actor()
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_K and _clock.has_weather():
		_clock.weather.next_weather()
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_N:
		_debug.toggle_navmesh()
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_G:
		_debug.path_to_view()
	elif event is InputEventKey and event.pressed and not event.echo and (event.keycode == KEY_BRACKETLEFT
			or event.keycode == KEY_BRACKETRIGHT):
		_debug.note(_streamer.scale_lod_split(1.25 if event.keycode == KEY_BRACKETRIGHT else 0.8))
	elif event is InputEventKey and event.pressed and not event.echo and _streamer.world_id != 0 and (event.keycode == KEY_MINUS
			or event.keycode == KEY_EQUAL):
		_debug.note(_streamer.change_radius(1 if event.keycode == KEY_EQUAL else -1))
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_M:
		var next: int = (ViewerSettings.MSAA_STEPS.find(get_viewport().msaa_3d) + 1) % ViewerSettings.MSAA_STEPS.size()
		get_viewport().msaa_3d = ViewerSettings.MSAA_STEPS[next]
		_debug.note("MSAA " + ("off" if next == 0 else ViewerSettings.MSAA_NAMES[next] + "x"))
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_V:
		_debug.note("flying" if _rig.toggle_fly() else "walking")


## The scripts' state and where the camera is.
func _save_game(path: String) -> void:
	var state := {
		"format": 1,
		"papyrus": _papyrus.save_state(),
		"cell": _place.cell_id,
		"world": _streamer.world_id,
		"position": _camera.position,
		"yaw": _rig.yaw,
		"pitch": _rig.pitch,
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
		_place.enter_exterior(int(state["world"]), state["position"], null)
	else:
		_place.enter_interior(int(state["cell"]), state["position"], null)
	_rig.apply_look(float(state["yaw"]), float(state["pitch"]))
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
		_rig.apply_look(_shots[0], -0.15)
	_frames = 20


## F12: the frame as a PNG and a JSON file describing it. The PNG is written
## on a worker thread so the frame does not hitch.
func _capture_shot(hide_overlay: bool) -> void:
	if _shot_busy:
		return
	_shot_busy = true
	if hide_overlay:
		_debug.visible = false
		await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
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
		_shot_busy = false
		return
	WorkerThreadPool.add_task(func() -> void: image.save_png(png))
	_debug.note("screenshot: " + ProjectSettings.globalize_path(png))
	if not _settings.shot_notes or DisplayServer.get_name() == "headless":
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
		_debug.add_child(_shot_note)
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
			_debug.note("note saved")
		else:
			_debug.note("note not saved: cannot write " + ProjectSettings.globalize_path(_shot_note_json))
	_shot_note.visible = false
	_shot_note_text.release_focus()
	get_tree().paused = false
	Input.mouse_mode = _shot_note_mouse
	_shot_busy = false


## Everything needed to come back to this view, in the viewer or the game.
func _shot_metadata(overlay_hidden: bool) -> Dictionary:
	var eye := SkydotWorld.godot_to_skyrim(_camera.global_position)
	var heading := fposmod(-rad_to_deg(_rig.yaw), 360.0)
	var tilt := -rad_to_deg(_rig.pitch)
	var place := {}
	var console: Array[String] = []
	if _streamer.world_id != 0:
		var world_name := ""
		for w in _world.list_worlds():
			if w["id"] == _streamer.world_id:
				world_name = w["editor_id"]
		var grid := Vector2i(floori(eye.x / WorldStreamer.CELL_UNITS), floori(eye.y / WorldStreamer.CELL_UNITS))
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
		pack["records_hash"] = manifest.get("records", {}).get("hash", "")
		pack["world_hash"] = manifest.get("world", {}).get("hash", "")
		pack["input"] = manifest.get("input", {})
	var size := get_viewport().get_visible_rect().size
	return {
		"format": ViewerSettings.SHOT_FORMAT,
		"taken": Time.get_datetime_string_from_system(),
		"place": place,
		"camera": {
			"game": {"x": eye.x, "y": eye.y, "z": eye.z, "heading": heading, "tilt": tilt},
			"engine": {"position": [_camera.global_position.x, _camera.global_position.y,
				_camera.global_position.z], "yaw": _rig.yaw, "pitch": _rig.pitch},
			"fov": _camera.fov,
			"resolution": [int(size.x), int(size.y)],
		},
		"time": _clock.describe(),
		"weather": weather,
		"viewer": {
			"flying": _player.fly,
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
			"msaa": ViewerSettings.MSAA_NAMES[maxi(ViewerSettings.MSAA_STEPS.find(get_viewport().msaa_3d), 0)],
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


func _fail(message: String) -> void:
	push_error(message)
	printerr(message)
	get_tree().quit(1)
