# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game: the composition root of everything that is not a front end. It
# opens a pack and owns the world, the script VM (SkydotPapyrus), the AI, the
# clock, the streamer, the place being shown and the transitions between
# places, the references scripts place, projectiles, sounds and the saves.
# Every SkydotPapyrus signal is connected here or in a helper made here
# (ScriptBridge). It runs headless: a test builds it with a bare Camera3D and a
# stand-in player.
#
# What a shell (today the viewer; later a flat game and a VR shell) gives it:
#
# - a viewpoint (Camera3D): the streamer and the weather follow it. It is the
#   shell's to move; the session only reads where it is.
# - a player object, which the session never reaches into beyond this contract:
#     yaw, pitch: float             where it looks (radians), read
#     flying: bool                  read and write
#     place_at(position, yaw, pitch)  put it there (engine space)
#     set_view_distance(metres)     how far the viewpoint sees
#     save_section() -> PackedByteArray   its part of the PLYR section
#     load_section(bytes) -> bool   decode and check it (changes nothing yet);
#                                   then loaded_position, loaded_yaw and
#                                   loaded_pitch say what place_at gets
#     is_sprinting() -> bool        optional, with set_sprint_allowed(bool): the
#                                   player runs at the sprint gait; the session
#                                   drains Stamina meanwhile and forbids
#                                   sprinting while it is out
#
# The shell calls `step` once a frame, with the player's feet, and listens to
# the signals; it does not subscribe to SkydotPapyrus.
class_name GameSession
extends Node

## Metres from the feet to the eyes.
const EYE_HEIGHT := 1.7

## The session cannot go on (a pack that does not open, a place that cannot be
## entered, an --activate reference that never appears); the shell ends the run.
signal failed(text: String)
## Something to tell the user: a script's message, a quest note, what an
## activation says, the weather.
signal message(text: String, box: bool)
signal quest_started(quest: int)
signal quest_stage(quest: int, stage: int, text: String)
signal objective_changed(quest: int, index: int, state: String, text: String)
## A cell was built and shown (streamed or an interior): its scripts are
## attached.
signal place_built(root: Node, cell_id: int)
## The place on screen is about to go.
signal place_left
## An actor the AI brought into the place on screen was built.
signal actor_arrived(ref: int)
## The --activate run: everything was activated.
signal activations_finished
## A non-essential actor other than the player was brought to Health 0.
signal actor_died(ref: int)

## Stamina a sprinting player loses per second. To measure in the original
## (the game's fSprintStaminaDrainMult is a multiplier of something else).
const SPRINT_STAMINA_PER_SECOND := 7.0

var config: GameConfig
var pack: SkydotPack
var world: SkydotWorld
var papyrus: SkydotPapyrus
var ai: SkydotAi  # null with the AI off
var clock: SkydotClock
var streamer: SkydotStreamer
var place: Place
var transition: PlaceTransition
var saves: SaveService
## The root of the place's nodes (cells, sky, LOD, placed references).
var host: Node3D

## Off for runs that drive themselves: doors travel without a fade.
var interactive := true:
	set(value):
		interactive = value
		if transition != null:
			transition.interactive = value

var _viewpoint: Camera3D
var _prompt: ActivationPrompt
var _target := {}
var _target_frame := -1
var _sprint_allowed := true
var _player: Object
var _bridge: ScriptBridge
## What scripts place, fire and play.
var placed: PlacedRefs
var projectiles: Projectiles
var sounds: Sounds


## Open the pack and put the game together around `viewpoint` and `player`.
## False (with `failed` emitted) if the pack does not open.
func open(game_config: GameConfig, viewpoint: Camera3D, player: Object) -> bool:
	config = game_config
	_viewpoint = viewpoint
	_player = player
	clock = SkydotClock.new()
	clock.setup(config.time, config.time_scale, config.still)
	if not _open_world():
		return false
	host = Node3D.new()
	host.name = "Host"
	add_child(host)
	_make_streamer()
	place = Place.new(world, host, config, _viewpoint, _player, ai, clock, streamer)
	place.built.connect(_on_interior_built)
	place.left.connect(place_left.emit)
	place.message.connect(func(text: String) -> void: message.emit(text, false))
	place.failed.connect(failed.emit)
	if ai != null:
		ai.actor_arrived.connect(place.on_actor_arrived)
		ai.actor_arrived.connect(actor_arrived.emit)
	transition = PlaceTransition.new(place, streamer)
	transition.interactive = interactive
	add_child(transition)
	_make_game_nodes()
	saves = SaveService.new(papyrus, place, streamer, _player, _bridge, clock)
	return true


## The pack, its world, the VM and the AI, and what is built.
func _open_world() -> bool:
	pack = SkydotPack.new()
	if pack.open(config.pack) != OK:
		failed.emit(pack.get_error())
		return false
	world = pack.open_world()
	if world == null:
		failed.emit(pack.get_error())
		return false
	papyrus = SkydotPapyrus.new()
	papyrus.setup(pack, world)
	papyrus.attach_clock(clock)
	papyrus.actor_died.connect(actor_died.emit)
	world.skyrim_materials = config.materials
	world.effects = config.effects
	world.grass = config.grass
	world.all_light_shadows = config.all_light_shadows
	world.collision = config.collision
	world.navigation = config.navigation
	world.actors = config.actors
	world.actor_wander = config.wander
	if config.ai:
		ai = SkydotAi.new()
		if ai.setup(world, papyrus) == OK:
			clock.attach_ai(ai)
			ai.drive = world.actor_wander
			ai.actor_left.connect(_on_actor_left)
		else:
			ai = null
	world.foliage_bias = config.foliage_bias
	if config.has_tiling:
		world.terrain_tiling = config.tiling
	return true


## The streaming of exterior cells and the building ahead behind load doors,
## as the options say.
func _make_streamer() -> void:
	streamer = SkydotStreamer.new()
	streamer.setup(world, pack, host, _viewpoint, ai)
	streamer.radius = config.radius
	streamer.build_budget_usec = config.build_budget_usec
	streamer.lod_enabled = config.lod
	streamer.lod_split = config.lod_split
	streamer.large_refs = config.large_refs
	streamer.large_ref_radius = config.large_ref_radius
	if config.has_tree_distance:
		streamer.set_tree_distance(config.tree_distance)
	streamer.eye_height = EYE_HEIGHT
	streamer.preload_enabled = config.preload_doors
	streamer.preload_distance = config.preload_distance
	streamer.cell_finished.connect(_on_cell_built)


## What scripts make and play (references, projectiles, sounds) and the bridge
## from the VM's signals to the scene.
func _make_game_nodes() -> void:
	projectiles = Projectiles.new(world, pack, world.find_ref_node)
	host.add_child(projectiles)
	sounds = Sounds.new(world, pack, world.find_ref_node, papyrus.new_sound_instance)
	host.add_child(sounds)
	placed = PlacedRefs.new(world, papyrus)
	host.add_child(placed)
	papyrus.ref_placed.connect(placed.place)
	papyrus.ref_deleted.connect(placed.remove)
	papyrus.destruction_model.connect(placed.swap_model)
	papyrus.fire_weapon.connect(projectiles.fire)
	papyrus.play_sound.connect(sounds.play)
	papyrus.stop_sound.connect(sounds.stop)
	papyrus.sound_volume.connect(sounds.set_volume)
	sounds.finished.connect(papyrus.notify_sound_finished)
	_prompt = ActivationPrompt.new(world, papyrus, host, _viewpoint)
	_bridge = ScriptBridge.new(world, papyrus, host, _viewpoint, config, place, streamer, transition, sounds)
	place.left.connect(_bridge.place_left)
	streamer.cell_dropped.connect(_bridge.scripts_unloaded)
	_bridge.activations_finished.connect(activations_finished.emit)
	_bridge.failed.connect(failed.emit)
	_bridge.message.connect(message.emit)
	_bridge.quest_started.connect(quest_started.emit)
	_bridge.quest_stage.connect(quest_stage.emit)
	_bridge.objective_changed.connect(objective_changed.emit)


## Start the quests the game starts with and set the stages the options name.
## False (with `failed` emitted) for a stage that cannot be read.
func start_game() -> bool:
	if config.quests:
		var started := papyrus.start_game_enabled_quests()
		print("quests: %d of %d started with the game" % [started, world.get_quest_count()])
	_bridge.quests_ready = true
	if not config.set_stage.is_empty():
		for item in config.set_stage.split(","):
			var parts: PackedStringArray = item.split(":")
			var quest := world.find_quest(parts[0])
			if quest == 0 or parts.size() != 2:
				failed.emit("--set-stage wants EDID:STAGE, got " + item)
				return false
			if not papyrus.set_stage(quest, int(parts[1])):
				print("%s: stage %s refused" % [parts[0], parts[1]])
	return true


## Enter worldspace `world_name` at `at`, looking at `target` (null: keep the
## look). Positions are engine space. False (with `failed` emitted) if it
## cannot be entered.
func enter_world(world_name: String, at: Vector3, target: Variant) -> bool:
	var world_id := world.find_world(world_name)
	if world_id == 0:
		failed.emit("no worldspace named " + world_name)
		return false
	var started := Time.get_ticks_usec()
	var variants := world.warm_up()
	print("warm_up: %d shader variants in %.0f ms" % [variants, (Time.get_ticks_usec() - started) / 1000.0])
	return place.enter_exterior(world_id, at, target)


## Enter interior `cell_name`. `at` null puts the player where the game does
## (the arrival spot of a door leading in), else at `at`, looking at `target`.
func enter_cell(cell_name: String, at: Variant, target: Variant) -> bool:
	var cell_id := world.find_cell(cell_name)
	if cell_id == 0:
		failed.emit("no cell named " + cell_name)
		return false
	place.enter_interior(cell_id, at, target)
	return true


## Put items in the player's inventory: Vector2i(form id, count).
func give(items: Array[Vector2i]) -> void:
	for item in items:
		papyrus.add_item(SkydotPapyrus.PLAYER_REF, item.x, item.y)


## The game's frame, after the shell has moved the player and before it
## draws: streaming, the transition, scripts, the clock and the AI. `feet` is
## the player's position (engine space).
func step(delta: float, feet: Vector3, benchmarking := false) -> void:
	streamer.update()
	streamer.preload_step(feet)
	if transition.is_fading():
		transition.step(delta)
	update_scripts(delta, feet)
	_drain_sprint(delta)
	if ai != null:
		clock.sync(benchmarking)
		ai.update(delta)


## Scripts only: the player's position for triggers, then the VM's time.
func update_scripts(delta: float, feet: Vector3) -> void:
	papyrus.update_actor(SkydotPapyrus.PLAYER_REF, SkydotWorld.godot_to_skyrim(feet))
	papyrus.update(delta)


## A sprinting player (the shell's player object says so) loses Stamina; at 0
## it may not sprint until Stamina is back above 0. Damaging Stamina also holds
## its regeneration back, so it does not recover while sprinting.
func _drain_sprint(delta: float) -> void:
	if not (_player.has_method("is_sprinting") and _player.has_method("set_sprint_allowed")):
		return
	if _player.is_sprinting():
		papyrus.damage_actor_value(SkydotPapyrus.PLAYER_REF, "Stamina", SPRINT_STAMINA_PER_SECOND * delta)
	var allowed := actor_value(SkydotPapyrus.PLAYER_REF, "Stamina") > 0.0
	if allowed != _sprint_allowed:
		_sprint_allowed = allowed
		_player.set_sprint_allowed(allowed)


func is_fading() -> bool:
	return transition.is_fading()


# ---- activation

## The player activates what the viewpoint looks at (Shift: picks locks).
func activate_in_view(force: bool) -> void:
	if not transition.is_fading():
		_bridge.activate_in_view(force)


func has_activations() -> bool:
	return _bridge.has_activations()


## The --activate and --damage runs, a step a frame.
func run_activation() -> void:
	_bridge.run_activation()


# ---- actor values and the prompt

## Actor value `name` ("Health", "Stamina"...) of `ref`; 0 for no actor.
func actor_value(ref: int, name: String) -> float:
	return papyrus.get_actor_value(ref, name)


func actor_value_max(ref: int, name: String) -> float:
	return papyrus.get_actor_value_max(ref, name)


## What activating what the viewpoint looks at would do: {ref, verb, name,
## lock_level} (lock_level -1: not locked), or {} if nothing is in reach or it
## has no name. Worked out once per frame.
func activation_target() -> Dictionary:
	var frame := Engine.get_process_frames()
	if frame != _target_frame:
		_target_frame = frame
		_target = _prompt.target() if _prompt != null and not transition.is_fading() else {}
	return _target


# ---- saves

func save_game(path: String) -> bool:
	return saves.save(path)


func load_game(path: String) -> bool:
	return saves.load_game(path)


# ---- queries for the shell's overlays

func get_running_quests() -> PackedInt64Array:
	return papyrus.get_running_quests()


func get_quest_state(quest: int) -> Dictionary:
	return papyrus.get_quest_state(quest)


## A quest's name as the game shows it, else its editor id.
func quest_name(quest: int) -> String:
	return _bridge.quest_name(quest)


# ---- from the parts

## A cell was built and shown: its scripts attach, the shell hears of it.
func _on_cell_built(cell: Node3D, cell_id: int) -> void:
	_bridge.scripts_loaded(cell, cell_id)
	place_built.emit(cell, cell_id)


## The streamer registers the load doors of the cells it builds; an interior is
## built by the place.
func _on_interior_built(cell: Node3D, cell_id: int) -> void:
	_on_cell_built(cell, cell_id)
	streamer.register_doors(cell)


func _on_actor_left(ref: int, _door: int) -> void:
	print("0x%08X leaves through a door" % ref)
