# SPDX-License-Identifier: GPL-3.0-or-later
#
# Loads one cell from a pack and shows it with a fly camera, or streams a
# worldspace's exterior cells around the camera. Load doors lead between them.
#
#   godot4.7 --path game res://viewer/cell_viewer.tscn -- \
#       --pack DIR --cell EDITOR_ID [--screenshot out.png] [--materials off]
#   godot4.7 --path game res://viewer/cell_viewer.tscn -- \
#       --pack DIR --world Tamriel --at X,Y,Z [--target X,Y,Z] [--radius 2]
#   ./skydot.x86_64 -- --pack DIR ...   (an exported build: the pack tool, its
#       main scene, hands over to this one when it gets --pack)
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
# default 4, about 0.3 ms at 1080p); M switches between them. Alpha-tested leaves
# are antialiased by it too (alpha to coverage). --aa off|fxaa|smaa adds a
# screen-space pass over the frame (default off; it is not temporal: nothing
# smears between frames). --foliage-bias LEVELS (default 0.5) samples the albedo of
# alpha-tested trees and grass that many mip levels blurrier, which steadies thin
# leaves at a distance. --render-scale 1..2 (default 1) draws the 3D scene that
# many times larger per side and averages it down: supersampling, the quality
# ceiling to compare against, at the cost of the pixels (2 is four times).
# --fov DEGREES sets the camera's vertical field of view (default 75; the
# game's is 50.4 at 16:9). --shadows off disables the sun's shadows.
# --light-shadows all gives every placed light shadows, not only those whose
# record asks for them (game, the default). --grass off grows no grass.
# --image-space off shows the scene's numbers ungraded (no image space);
# --image-space neutral keeps its tone mapping and eye adaptation but grades
# neutrally (saturation, brightness, contrast 1, no tint), as the game's frame
# with SkyrimRemote's `@imagespace sat=1 bright=1 contrast=1 tint_amount=0`.
# --volumetric off leaves out the weather's volumetric lighting haze (default on).
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
# --god (a flag) restores the player's Health every frame, like tgm. --shout-wait EDID:STAGE holds the
# shout flags (--teach-word … --shout) back until that quest stage is done.
# --screenshot, --benchmark, --activate and --no-input (a flag: it takes no value) runs ignore the keyboard
# and mouse.
# --activate 0xREF[,0xREF...] activates those references in turn, each in the
# place the previous one led to, then quits (for tests; works headless).
# --activate-wait FRAMES waits that many frames between two of them, for
# scripts and animations to play out; --quit-after FRAMES quits that many
# frames after the last (default 5); without --activate, from the start.
# --start-quest EDID[,EDID...] starts those quests after the load (a scene
# with "begin on quest start" then begins once its actors are loaded); the
# scenes' lines show as subtitles under the crosshair for speakers within 20 m.
# --open-magic shows the magic menu (P) after the load (--magic-category Destruction chooses the category). --open-inventory shows the inventory after the load, --open-container 0xREF activates that
# reference as the player (a container or a workbench opens its menu, --crafting-category NAME|N
# picks the enchanting table's category, --learn-enchantment 0xENCH[,...] teaches enchantments; item_menu.gd); both with --hud
# or on their own. --talk 0xREF opens a dialogue with that actor after the load (dialogue_menu.gd);
# with --screenshot the lines play out first (--shot-delay).
# --hud forces the health, magicka and stamina bars, the crosshair and the
# activation prompt on in --screenshot and --benchmark runs, which leave them
# out; --damage-actor 0xREF:ActorValue:AMOUNT[,...] damages those actor values
# once at the start (ref 0x14 is the player), so a shot shows the bars.
# --drink 0xFORM[,...] makes the player use those items (a potion, food, an ingredient; added
# first if not carried) once the place is built; --cast 0xSPELL[@0xREF][,...] makes the player
# cast those spells once, on the reference or, without one, on himself (the cost is paid, the
# effects land, no projectile yet). The log tells: `magic: 0x14 casts 0x12FCC on 0x14 (cost N)`,
# `magic: effect 0x1CEA4 on 0x14 start mag 10 dur 1`, `magic: effect ... end`.
# A gamepad plays as Skyrim SE's Xbox layout (player_input.gd): the left stick
# moves, the right stick looks (--stick-look-speed RAD_PER_S at full deflection,
# default 3; --invert-look-y), A activates, Y jumps, LB sprints, Start shows the
# journal. Sprinting costs Stamina (7 a second) and stops when it is out.
# --give 0xFORM[xN],0xFORM... puts those items (N of each, default 1) into the player's inventory
# at the start, after --load (like player.additem). --equip 0xFORM[,...] then equips them.
# Combat: R (gamepad X) draws and sheathes the weapon (the first-person arms and the
# weapon show while it is drawn: first_person_body.gd), the left mouse button
# (right trigger) attacks, a hold longer than fInitialPowerAttackDelay is a power
# attack (game/combat.gd). --ready-weapon draws it after the load; --attack-at
# 0xREF[,power] draws it, faces that actor and attacks once it is in reach (for shots
# and tests); --kill 0xREF[,...] hits those actors with the weapon in hand until dead;
# --fight 0xREF[,...] makes those actors fight the player at once (they and their fights keep
# running in a --screenshot, where nothing else moves); --npc-give 0xREF:0xFORM[xN],... puts items
# in an actor's inventory and --strip-melee 0xREF[,...] takes its melee weapons away (an archer);
# --npc-draw 0xREF[,...] makes those actors draw their weapons (combat stance, equip clip,
# the weapon model on the WEAPON bone: game/combat.gd npc_draw).
# Magic (game/magic.gd): a spell or scroll in a hand is readied with R and cast with that hand's
# attack button (left mouse the right hand, right mouse the left hand, the triggers on a gamepad);
# Z (gamepad RB) casts the power. --equip-spell 0xFORM[,left|right|power][,0xFORM...] teaches the
# spell if unknown and puts it in a hand (right by default); --cast-at 0xREF[,left|right] readies the
# hands, faces that actor and casts the hand's spell at it (holding a concentration spell until it
# is dead), for shots and tests: `--equip-spell 0x12FCD --cast-at 0x3B9F7 --screenshot out.png`.
# Shouts (game/magic.gd): Z (gamepad RB, VR: a grip) is the voice key: it casts a power in the voice slot, or
# charges the shout there while it is held (a tap is one word, 0.2 s two, 0.9 s three; the words you know and
# have unlocked limit it). --teach-word 0xWOOP[,...] teaches words of power, --unlock-word 0xWOOP[,...]
# unlocks them, --equip-shout 0xSHOU puts a shout in the voice slot, --dragon-souls N sets the dragon souls,
# --shout N[,0xREF] faces that actor (--shout-from METRES stands that far from it first) and says N words: `--teach-word 0x13E22 --unlock-word 0x13E22
# --equip-shout 0x13E07 --shout 1,0x39FE4` (Unrelenting Force at a bandit of Bleak Falls Barrow).
# F3 shows or hides the performance line at the top right (fps, frame time
# mean and max over the last second, the renderer's CPU and GPU time and the
# physics time per frame, draw calls, primitives, objects). It is in
# the shot JSON's "viewer.perf", and --screenshot prints it as a "perf:" line.
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
# long before a --screenshot is taken; --shot-frames N [--shot-every K] saves
# N frames of one view instead (see shot_recorder.gd; --shot-camera X,Y,Z --shot-look X,Y,Z puts that view
# elsewhere than at the player, Skyrim units, for a side view of a fight; --shot-live runs the whole game during
# the series, quests, scenes and AI, as in play: MQ104); --hide-refs ID[,ID...]
# (hex form IDs) hides those references in them, as the game's `disable`. Inside, time runs too (--time-scale);
# T and Shift+T move it there as well. I tells what the nearest actor's
# package is.
# --at X,Y,Z --target X,Y,Z (Skyrim game units, as `bethconv cell` prints)
# places the camera. With --screenshot, renders four views from the cell's centre
# (out_0.png .. out_3.png, one per 90 degrees of yaw; one view with --at) and
# quits. --materials off
# keeps the importer's standard materials, --effects off leaves models and
# lights still, for comparison.
#
# This script is the viewer, a shell around the game: it reads the options,
# owns the camera and player (PlayerRig, PlayerInput), the image space, the
# debug overlay, shots and the benchmark, and runs the frame. The game itself
# is GameSession (game/game_session.gd): the pack, scripts, AI, clock,
# streaming, places, saves. The shell hands it the camera and the player and
# listens to its signals.
extends Node3D

var _settings: ViewerSettings
var _session: GameSession
var _image_space: SkydotImageSpace
var _rig: PlayerRig
var _controls: PlayerInput
var _loop: ShellLoop  # the commands, game signals, start options and frame order both shells share
var _hud: Hud
var _dialogue: DialogueMenu
var _items: ItemMenu
var _debug: DebugOverlay  # notes, journal, navmesh, path; Shift+F12 hides it for a shot
var _shots: ShotRecorder
var _body: FirstPersonBody  # the player's arms and weapon
var _benchmark := BenchmarkRun.new()

## Off for runs that drive themselves (tools set it): the keyboard and mouse
## are ignored and doors travel without a fade.
var _input := true:
	set(value):
		_input = value
		if _session != null:
			_session.interactive = value
		if _controls != null:
			_controls.enabled = value


func _ready() -> void:
	_settings = ViewerSettings.from_arguments(OS.get_cmdline_user_args())
	if not _settings.shot_note.is_empty():
		print("shot note: ", _settings.shot_note)
	if not _settings.error.is_empty():
		_fail(_settings.error)
		return
	if not _open_game():
		return
	_create_components()
	_start_game()
	_loop.arm_quit()  # nothing to activate: --quit-after counts from the start


## The camera and player, and the game around them: the session opens the
## pack. False if it does not (the run is over).
func _open_game() -> bool:
	_image_space = SkydotImageSpace.new()
	_rig = PlayerRig.new(_settings, _image_space)
	add_child(_rig.camera)
	add_child(_rig.player)
	_session = GameSession.new()
	_session.name = "GameSession"
	_session.failed.connect(_fail)
	add_child(_session)
	# Runs that capture, measure or activate on their own ignore the keyboard
	# and mouse, so a stray touch cannot move the view.
	_input = _settings.interactive
	if not _session.open(_settings.game_config(), _rig.camera, _rig):
		return false
	get_viewport().msaa_3d = ViewerSettings.MSAA_STEPS[_settings.msaa_index]
	get_viewport().screen_space_aa = ViewerSettings.AA_STEPS[_settings.aa_index]
	get_viewport().scaling_3d_scale = _settings.render_scale
	return true


## Make the shell's parts and connect them to the game's signals.
func _create_components() -> void:
	_debug = DebugOverlay.new()
	_debug.setup(_session, _rig.camera, _rig.player)
	add_child(_debug)
	_session.place_built.connect(func(root: Node, _cell_id: int) -> void: _debug.navmesh_overlay(root))
	_session.place_left.connect(_debug.clear_path)
	_controls = PlayerInput.new(_rig)
	_controls.enabled = _input
	_controls.look_speed = _settings.stick_look_speed
	_controls.invert_look_y = _settings.invert_look_y
	add_child(_controls)
	_loop = ShellLoop.new(self, _session, _rig, _controls, _settings, _image_space, _debug.note)
	_loop.command.connect(_on_command)
	_loop.inventory_requested.connect(func() -> void:
		if _items != null:
			_items.open_inventory())
	_loop.magic_requested.connect(func() -> void:
		if _items != null:
			_items.open_magic())
	_body = FirstPersonBody.new(_session, _rig.camera)
	add_child(_body)
	if _settings.hud:
		_hud = Hud.new(_session, _controls)
		add_child(_hud)
	if _settings.hud or not _settings.start_quests.is_empty():
		add_child(Subtitles.new(_session))
	if _settings.hud or _settings.talk != 0:
		_dialogue = DialogueMenu.new(_session, _controls)
		add_child(_dialogue)
	if _settings.hud or _settings.open_inventory or _settings.open_magic or _settings.open_container != 0:
		_items = ItemMenu.new(_session, _controls)
		add_child(_items)
	add_child(DeathMenu.new(_session, _rig, _loop))
	_shots = ShotRecorder.new(self, _settings, _session.world, _rig, _session.place, _session.streamer,
		_session.clock, _debug)
	if _settings.has_screenshot:
		_debug.toggle_perf()  # kept out of rendered comparison shots; still printed as "perf:"
	_controls.capture_mouse()


## Start the game (ShellLoop.start: the place and the options) and show what the options ask
## of the viewer: menus, the screenshot run, the benchmark.
func _start_game() -> void:
	var settings := _settings
	if not _loop.start():
		return
	_select_crafting_category(settings)
	if _loop.in_world:
		if settings.has_benchmark:
			_benchmark.begin(settings.benchmark, settings.fly_speed)
			_session.streamer.max_usec = 0
			_session.streamer.load_everything()
		if settings.has_screenshot:
			# Everything in range first, so the capture is complete.
			_session.streamer.load_everything()
			if _session.streamer.lod != null:
				print("lod: ", _session.streamer.lod.get_stats())
			_shots.begin_run(settings.screenshot, [NAN])
		return
	if settings.open_inventory and _items != null:
		_items.open_inventory()
	if settings.open_magic and _items != null:
		_items.open_magic()
		_items.magic_view.select_category(settings.magic_category)
	if settings.has_screenshot:
		if (settings.has_at and settings.has_target) or (settings.has_at and settings.has_look):  # --look: a shot's view
			_shots.begin_run(settings.screenshot, [NAN])
		else:
			var yaws: Array[float] = []
			for i in 4:
				yaws.append(i * PI / 2.0)
			_shots.begin_run(settings.screenshot, yaws)


## --crafting-category: the category an opened enchanting table shows.
func _select_crafting_category(settings: ViewerSettings) -> void:
	if settings.crafting_category != "" and _items != null:
		_items.crafting_view.select_category(settings.crafting_category)


func _process(delta: float) -> void:
	if _shots == null:
		return
	ImageSpaceSync.apply(get_viewport(), _settings, _session, _image_space)
	_debug.sample_perf(delta)
	if _shots.is_running() and _settings.shot_live:
		# --shot-live: the game runs as in play (quests, scenes, the AI), the camera at the shot's view.
		_loop.options.step(delta)
		_session.step(delta, _rig.player.global_position)
		if _settings.has_shot_camera:
			_rig.camera.global_position = SkydotWorld.skyrim_position(_settings.shot_camera)
			_rig.camera.look_at(SkydotWorld.skyrim_position(_settings.shot_look))
		_body.follow()
		_shots.step(delta)
		return
	if _shots.is_running():
		# The shot's own frame: the game runs only as far as a still picture needs.
		_loop.options.step(delta)
		if _settings.shot_frames > 1 or _settings.shot_delay > 0.0:
			# A frame series, or a shot after a delay, shows what scripts do over time (a trap
			# swinging when the feet reach its plate, a shout's push); other single shots stay still.
			_session.update_scripts(delta, _rig.player.global_position)
			_session.visual_effects.step(delta)  # the streams of a soul absorbed follow their ends
			if _session.has_activations():
				_session.run_activation()  # --activate inside the series
		if _settings.has_shot_camera:
			# A side view: the camera stands apart from the player, whom the actors fight.
			_rig.camera.global_position = SkydotWorld.skyrim_position(_settings.shot_camera)
			_rig.camera.look_at(SkydotWorld.skyrim_position(_settings.shot_look))
		_session.update_dialogue(delta)
		_session.update_combat(delta)
		_session.update_fights(delta, _rig.player.global_position)
		_loop.options.aim_step()
		_body.follow()
		_shots.step(delta)
		return
	var may_drive := _loop.frame(delta, func(_dt: float) -> void: _body.follow(), _benchmark.is_running())
	_debug.age(delta)
	if not may_drive:
		return
	if _benchmark.is_running():
		if _benchmark.frame(delta, _rig.player, _session.streamer.max_usec):
			if _session.streamer.lod != null:
				print("lod: ", _session.streamer.lod.get_stats())
			get_tree().quit(0)
		return
	_loop.drive(delta)


## The keys ShellLoop does not handle: the viewer's debug keys (the overlay, shots, streaming and
## quality knobs are the viewer's; the VR shell has none of them).
func _on_command(action: StringName, shift: bool) -> void:
	match action:
		PlayerInput.SHOW_POSITION:
			_debug.note(_debug.position_text(_rig.yaw, _rig.pitch))
		PlayerInput.TOGGLE_PRELOAD:
			_debug.note("load doors preload what is behind them" if _session.streamer.toggle_preload() else "load doors load on use")
		PlayerInput.TAKE_SHOT:
			_shots.capture(shift)
		PlayerInput.SHOW_JOURNAL:
			_debug.toggle_journal()
		PlayerInput.INSPECT_ACTOR:
			_debug.inspect_actor()
		PlayerInput.TOGGLE_NAVMESH:
			_debug.toggle_navmesh()
		PlayerInput.TOGGLE_PERF:
			_debug.toggle_perf()
		PlayerInput.SHOW_PATH:
			_debug.path_to_view()
		PlayerInput.LOD_DETAIL_DOWN:
			_debug.note(_lod_text(_session.streamer.scale_lod_split(0.8)))
		PlayerInput.LOD_DETAIL_UP:
			_debug.note(_lod_text(_session.streamer.scale_lod_split(1.25)))
		PlayerInput.RADIUS_DOWN:
			if _session.streamer.world_id != 0:
				_debug.note(_radius_text(_session.streamer.change_radius(-1)))
		PlayerInput.RADIUS_UP:
			if _session.streamer.world_id != 0:
				_debug.note(_radius_text(_session.streamer.change_radius(1)))
		PlayerInput.CYCLE_MSAA:
			var next: int = (ViewerSettings.MSAA_STEPS.find(get_viewport().msaa_3d) + 1) % ViewerSettings.MSAA_STEPS.size()
			get_viewport().msaa_3d = ViewerSettings.MSAA_STEPS[next]
			_debug.note("MSAA " + ("off" if next == 0 else ViewerSettings.MSAA_NAMES[next] + "x"))


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
