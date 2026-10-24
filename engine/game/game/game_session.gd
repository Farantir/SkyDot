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

## A line of dialogue starts: the speaker's reference, the (subtitle) text, and
## the seconds it stays.
signal dialogue_line(speaker: int, text: String, seconds: float)
## The lines are spoken: the topics to pick from, [{id, text, said}]. Choose
## one with `dialogue_choose(index)`; "Goodbye" is the shell's own entry.
signal dialogue_topics(list: Array)
## The dialogue is over (a Goodbye line, or `dialogue_close`).
signal dialogue_closed
## A scene's line starts: the speaker's reference, the subtitle text and how
## long it lasts. The shell decides whether the player is near enough to see it.
signal subtitle_line(speaker: int, text: String, seconds: float)

## The player's Health reached 0 (log `death: player killed by 0xA`): the killer, 0 if none.
## Controls stop (`is_player_dead`) and the AI ends its fights against him; the shell shows the
## death (fade, "Load the last save?") and asks `reload_or_restart`.
signal player_died(killer: int)

## A magic effect started acting on `target` or ended (SkydotPapyrus.magic_effect_changed): the
## MGEF, its archetype and the instance. For presentation: lights, art, the active effects list.
signal magic_effect_changed(target: int, mgef: int, archetype: int, started: bool, instance: int)

## The player activated a container: its reference. The shell shows the menu
## (`inventory(ref)` is what it holds).
signal container_opened(ref: int)
## The player activated a workbench: its reference and bench type (1 create
## object, 2 grindstone, 3/4 enchanting, 5/6 alchemy, 7 armor workbench). The
## shell shows the crafting menu.
signal crafting_opened(ref: int, bench_type: int)
## Something moved between inventories, or was equipped or dropped.
signal items_changed

## The player's weapon state changed (Combat.State) or an attack ended: the
## state and the first-person clip to play (Combat.weapon_state_changed).
signal weapon_state_changed(state: int, clip: String)
## The player's attack began (Combat.attack_started).
signal attack_started(clip: String, power: bool)
## A hit landed: the target, apply_hit's result and the point (Combat.hit_landed).
signal hit_landed(target: int, result: Dictionary, point: Vector3)

## Item categories `use_item` takes effect of.
const USABLE: Array[String] = ["potion", "food", "ingredient"]

## Metres in front of the player a dropped item lands, and above the feet.
const DROP_DISTANCE := 0.8
const DROP_HEIGHT := 0.3
## The ids of created forms (potions mixed by alchemy; id_ranges::created_forms).
const CREATED_FIRST := 0xFF700000
const CREATED_LAST := 0xFF7FFFFF

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
var _pointer: Node3D  # what activating and the prompt point along: the viewpoint unless the shell says
var _prompt: ActivationPrompt
var _target := {}
var _target_frame := -1
var _sprint_allowed := true
var _feet := Vector3.ZERO  # the player's feet at the last step, engine space
var _player: Object
var _bridge: ScriptBridge
# Dialogue: the speaker, the lines still to speak, the one being spoken (and
# the seconds left of it), and what comes after the lines.
var _talk_speaker := 0
var _talk_lines: Array = []
var _talk_current := {}
var _talk_left := 0.0
var _talk_after := {}
var _talk_topics: Array = []
# Scenes: the dialogue actions waiting for the player's dialogue with their
# speaker to end, and the lines each running action still has to finish.
var _scene_held: Array = []
var _scene_lines := {}  # "scene:action" -> lines left
## The player's weapon, attacks and hit tests, and actors' attacks.
var _start := {}  # the first place entered: where a death with no save starts again
var combat: Combat
## What a death looks like: ragdolls, corpse places.
var corpses: Corpses
## What scripts place, fire and play.
var placed: PlacedRefs
var projectiles: Projectiles
var sounds: Sounds
## Spoken lines (dialogue voices), one queue per speaker.
var speech: Speech


## Open the pack and put the game together around `viewpoint` and `player`.
## False (with `failed` emitted) if the pack does not open.
func open(game_config: GameConfig, viewpoint: Camera3D, player: Object) -> bool:
	config = game_config
	_viewpoint = viewpoint
	_pointer = viewpoint
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
	papyrus.player_died.connect(player_died.emit)
	papyrus.magic_effect_changed.connect(magic_effect_changed.emit)
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
			papyrus.scene_package.connect(func(actor: int, package: int, push: bool) -> void:
				if push:
					ai.push_package(actor, package)
				else:
					ai.pop_package(actor, package))
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
	speech = Speech.new(sounds, world.find_ref_node)
	host.add_child(speech)
	speech.line_started.connect(_on_speech_started)
	speech.line_ended.connect(_on_speech_ended)
	combat = Combat.new(world, pack, papyrus, host, sounds, func() -> Node3D: return _pointer,
		func() -> Vector3: return _feet)
	combat.projectiles = projectiles
	combat.weapon_state_changed.connect(weapon_state_changed.emit)
	combat.attack_started.connect(attack_started.emit)
	combat.hit_landed.connect(hit_landed.emit)
	if ai != null:
		ai.set_combat(config.combat)
		ai.combat_state_changed.connect(_on_combat_state)
		ai.combat_attack.connect(_on_combat_attack)
		# Every hit, the player's and the actors', raises the alarm.
		combat.hit_landed.connect(func(target: int, result: Dictionary, _point: Vector3) -> void:
			ai.on_hit(int(result.get("attacker", 0)), target))
	items_changed.connect(combat.on_items_changed)
	corpses = Corpses.new(world, papyrus, host, func() -> Vector3: return _feet)
	actor_died.connect(corpses.on_died)
	combat.hit_landed.connect(corpses.on_hit)
	actor_arrived.connect(corpses.on_actor_arrived)
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
	_bridge.talk_requested.connect(dialogue_open)
	_bridge.container_opened.connect(container_opened.emit)
	_bridge.crafting_opened.connect(crafting_opened.emit)
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
	papyrus.prepare_actor_magic(SkydotPapyrus.PLAYER_REF)  # the player's abilities (race, birthsign)
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
	if _start.is_empty():
		_start = {"kind": "world", "name": world_name, "at": at, "target": target}
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
	if _start.is_empty():
		_start = {"kind": "cell", "name": cell_name, "at": at, "target": target}
	var cell_id := world.find_cell(cell_name)
	if cell_id == 0:
		failed.emit("no cell named " + cell_name)
		return false
	place.enter_interior(cell_id, at, target)
	return true


## Start quests by editor id (the viewer's --start-quest); returns how many
## started. A scene of such a quest with "begin on quest start" begins when its
## actors are loaded.
func start_quests(editor_ids: PackedStringArray) -> int:
	var started := 0
	for editor_id in editor_ids:
		var quest := world.find_quest(editor_id.strip_edges())
		if quest == 0:
			failed.emit("no quest named " + editor_id)
		elif papyrus.start_quest(quest):
			print("quest %s started" % editor_id)
			started += 1
	return started


## Put items in the player's inventory: Vector2i(form id, count).
func give(items: Array[Vector2i]) -> void:
	for item in items:
		papyrus.add_item(SkydotPapyrus.PLAYER_REF, item.x, item.y)


# ---- items

## What `owner` carries (the player by default; a container's reference), start
## items resolved first: [{form, name, count, weight, value, category,
## equipped, playable}] by name.
func inventory(owner := SkydotPapyrus.PLAYER_REF) -> Array:
	return papyrus.inventory(owner)


## What the player holds and wears: {right, left, ammo, worn: [forms]}.
func equipment() -> Dictionary:
	return papyrus.equipment(SkydotPapyrus.PLAYER_REF)


## Move up to `count` of `form`; returns how many moved. `extra` names a stack
## (a tempered or enchanted one; a row's `extra`), -1 takes plain units first.
func transfer(from: int, to: int, form: int, count: int, extra := -1) -> int:
	var moved := papyrus.transfer(from, to, form, count, extra)
	if moved > 0:
		items_changed.emit()
	return moved


## Everything `from` has, to `to`; returns the number of stacks moved.
func transfer_all(from: int, to: int) -> int:
	var stacks := 0
	for row: Dictionary in papyrus.inventory(from):
		if transfer(from, to, int(row["form"]), int(row["count"]), int(row["extra"])) > 0:
			stacks += 1
	return stacks


## The player equips (or consumes, or reads) one `form`. False if it is not
## carried or cannot be equipped.
func equip(form: int, extra := -1) -> bool:
	var done := papyrus.equip(SkydotPapyrus.PLAYER_REF, form, extra)
	if done:
		items_changed.emit()
	return done


## What the player uses up: a potion, food or an ingredient takes effect on the player and is
## taken from the inventory (SkydotPapyrus.equip does both). A poison waits for the weapon (7a-3):
## it logs `magic: poison 0x.. waits for the weapon` and stays. False if it is not carried or is
## no such item.
func use_item(form: int, extra := -1) -> bool:
	var category: String = papyrus.item_info(form).get("category", "")
	if category not in USABLE:
		return false
	return equip(form, extra)


## The player (or `caster`) casts `spell` on `target` (0: on itself): the cost is paid, the effects
## land, no projectile yet (SkydotPapyrus.cast_spell). {ok, reason, cost, instances}.
func cast_spell(spell: int, target := 0, seconds := 0.0, caster := SkydotPapyrus.PLAYER_REF) -> Dictionary:
	return papyrus.cast_spell(caster, spell, target, seconds)


func unequip(form: int, extra := -1) -> bool:
	var done := papyrus.unequip(SkydotPapyrus.PLAYER_REF, form, extra)
	if done:
		items_changed.emit()
	return done


## Drop up to `count` of `form` as one reference in front of the player, which
## keeps the count and is picked up by activating it. Returns the reference, 0
## if the player has none.
func drop(form: int, count: int, extra := -1) -> int:
	if form >= CREATED_FIRST and form <= CREATED_LAST:
		# The game cannot build a reference for a created form yet.
		message.emit("A mixed potion cannot be dropped yet", false)
		return 0
	var forward := Vector3(-sin(_player.yaw), 0.0, -cos(_player.yaw))
	var at := SkydotWorld.godot_to_skyrim(_feet + forward * DROP_DISTANCE + Vector3.UP * DROP_HEIGHT)
	var ref := papyrus.drop(SkydotPapyrus.PLAYER_REF, form, count, place.cell_id, at, Vector3.ZERO, extra)
	if ref != 0:
		items_changed.emit()
	return ref


## A container's name as the game shows it, else its editor id.
func container_name(ref: int) -> String:
	var base := world.get_base(papyrus.get_ref_base(ref))
	var text: String = base.get("name", "")
	return text if text != "" else String(base.get("editor_id", ""))


# ---- crafting

## A workbench's name as the game shows it.
func bench_name(ref: int) -> String:
	return container_name(ref)


## The create-object recipes of bench `ref`: see SkydotPapyrus.recipes.
func recipes(ref: int) -> Array:
	return papyrus.recipes(ref)


## Make `count` of `recipe`; returns how many were made.
func craft(recipe: int, count := 1) -> int:
	var made := papyrus.craft(recipe, count)
	if made > 0:
		items_changed.emit()
	return made


func temper_rows(ref: int) -> Array:
	return papyrus.temper_rows(ref)


func temper(recipe: int, form: int, extra: int) -> bool:
	var done := papyrus.temper(recipe, form, extra)
	if done:
		items_changed.emit()
	return done


## The carried items with an enchantment: [{form, extra, name, enchantment}].
func enchant_rows() -> Array:
	return papyrus.enchant_rows()


func alchemy_rows() -> Array:
	return papyrus.alchemy_rows()


func alchemy_preview(forms: PackedInt64Array) -> Dictionary:
	return papyrus.alchemy_preview(forms)


## Mix `forms`; the created potion's form, 0 if it could not be done.
func alchemy_mix(forms: PackedInt64Array) -> int:
	var potion := papyrus.alchemy_mix(forms)
	if potion != 0:
		items_changed.emit()
	return potion


## The game's frame, after the shell has moved the player and before it
## draws: streaming, the transition, scripts, the clock and the AI. `feet` is
## the player's position (engine space).
func step(delta: float, feet: Vector3, benchmarking := false) -> void:
	_feet = feet
	streamer.update()
	streamer.preload_step(feet)
	if transition.is_fading():
		transition.step(delta)
	update_scripts(delta, feet)
	update_dialogue(delta)
	update_scenes(delta)
	speech.update(delta)  # after the dialogue: a line it starts ends first, never after its subtitle
	combat.step(delta)
	corpses.step(delta)
	_drain_sprint(delta)
	if ai != null:
		clock.sync(benchmarking)
		ai.set_player(SkydotWorld.godot_to_skyrim(feet), not papyrus.is_player_dead())
		ai.update(delta)


## The fights under way only, for runs that keep the world still (shots).
func update_fights(delta: float, feet: Vector3) -> void:
	if ai != null:
		ai.set_player(SkydotWorld.godot_to_skyrim(feet), not papyrus.is_player_dead())
		ai.update_fights(delta)


## Make `ref` fight the player now (the viewer's --fight).
func start_fight(ref: int) -> bool:
	return ai != null and ai.start_combat(ref, SkydotPapyrus.PLAYER_REF)


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


# ---- combat

## Draw the player's weapon, or sheathe it (R, gamepad X).
func toggle_weapon() -> void:
	if not papyrus.is_player_dead():
		combat.toggle_weapon()


## The attack button went down or up: a short press is a light attack, a long
## one a power attack (Combat.attack_pressed).
func attack_pressed() -> void:
	if not papyrus.is_player_dead():
		combat.attack_pressed()


func attack_released() -> void:
	combat.attack_released()


## Attack now: power or light (Combat.attack).
func attack(power := false) -> bool:
	return not papyrus.is_player_dead() and combat.attack(power)


## The right-hand weapon's form, 0 for bare hands.
func player_weapon() -> int:
	return combat.player_weapon()


## Combat.State.
func weapon_state() -> int:
	return combat.weapon_state()


## The AI's fights reach the game: the stance and weapon, the Papyrus event.
func _on_combat_state(ref: int, target: int, state: int) -> void:
	if state == 1:
		combat.npc_draw(ref, true)
	elif state == 0:
		combat.npc_draw(ref, false)
	papyrus.combat_state_changed(ref, target, state)


func _on_combat_attack(ref: int, target: int, kind: String) -> void:
	match kind:
		"shoot":
			combat.npc_shoot(ref, target)
		"power":
			combat.npc_attack(ref, true, target)
		_:
			combat.npc_attack(ref, false, target)


## An actor attacks the player or `target` (the AI's door into Combat.npc_attack).
func npc_attack(attacker: int, power := false, target := Combat.PLAYER_REF) -> bool:
	return combat.npc_attack(attacker, power, target)


## An actor draws its weapon or sheathes it (Combat.npc_draw).
func npc_draw(ref: int, drawn: bool) -> bool:
	return combat.npc_draw(ref, drawn)


## The combat part alone, for runs where the session's `step` does not run
## (shots).
func update_combat(delta: float) -> void:
	combat.step(delta)
	corpses.step(delta)


## Debug (--kill): hit `ref` with the weapon in hand until it is dead. Returns
## the hits it took; 0 if it was dead already or is no actor.
func kill_actor(ref: int) -> int:
	var hits := 0
	var weapon := combat.player_weapon()
	var extra: int = papyrus.equipment(SkydotPapyrus.PLAYER_REF)["right_extra"]
	while hits < 200 and not papyrus.is_actor_dead(ref):
		var result := papyrus.apply_hit(SkydotPapyrus.PLAYER_REF, ref, weapon, extra, "attackStart", 0)
		if result.has("error"):
			break
		hits += 1
		hit_landed.emit(ref, result, _actor_chest(ref))
	return hits


func _actor_chest(ref: int) -> Vector3:
	var node := world.find_ref_node(ref) as Node3D
	return node.global_position + Vector3.UP * 1.2 if node != null else _feet


# ---- activation

## Activating and the prompt point along `node`'s -Z axis from its position
## (a VR shell: the controller's aim), not along the viewpoint.
func set_pointer(node: Node3D) -> void:
	_pointer = node


## The player activates what the pointer points at (the viewpoint's view unless
## set_pointer says otherwise; Shift: picks locks).
func activate_in_view(force: bool) -> void:
	if not transition.is_fading() and not papyrus.is_player_dead():
		_bridge.activate_in_view(force, _pointer)


## The player activates reference `ref` (not what the pointer shows): the
## viewer's --open-container. False if the reference is unknown.
func activate_ref(ref: int, force := false) -> bool:
	return _bridge.activate(place.cell_id, ref, null, force)


## Show the harvested model on the harvested plants under `root` (a built cell
## does it itself; for a reference built on its own).
func show_harvests(root: Node) -> void:
	_bridge.show_harvests(root)


func has_activations() -> bool:
	return _bridge.has_activations()


## The --activate and --damage runs, a step a frame.
func run_activation() -> void:
	_bridge.run_activation()


# ---- dialogue

func is_dialogue_open() -> bool:
	return _talk_speaker != 0


## How far the actor `ref` is from the player's view, metres; INF if it is not built.
func distance_to_player(ref: int) -> float:
	var node := world.find_ref_node(ref) as Node3D
	if node == null or not node.is_inside_tree():
		return INF
	return _viewpoint.global_position.distance_to(node.global_position)


## The name of the actor `ref` for a subtitle; empty if it has none.
func ref_name(ref: int) -> String:
	return world.get_actor(ref).get("name", "")


## The player talks to `ref`: its lines play (`dialogue_line`), then the topics
## come (`dialogue_topics`). False if `ref` cannot talk or a dialogue is open.
func dialogue_open(ref: int) -> bool:
	if _talk_speaker != 0 or not papyrus.dialogue_can_talk(ref):
		return false
	_talk_speaker = ref
	_begin_talk(papyrus.dialogue_open(ref))
	return true


## Pick topic `index` of the list `dialogue_topics` sent. Ignored while lines play.
func dialogue_choose(index: int) -> void:
	if _talk_speaker == 0 or _is_talking() or index < 0 or index >= _talk_topics.size():
		return
	var topic: Dictionary = _talk_topics[index]
	_talk_topics = []
	_begin_talk(papyrus.dialogue_choose(topic["id"]))


## End the dialogue now (the shell's Goodbye).
func dialogue_close() -> void:
	if _talk_speaker == 0:
		return
	if not _talk_current.is_empty() and _talk_current["last"]:
		papyrus.dialogue_fragment(_talk_current["info"], true, _talk_speaker)
	_finish_talk()


## Cut the line being spoken short.
func dialogue_skip() -> void:
	_talk_left = 0.0
	if _talk_speaker != 0:
		speech.skip(_talk_speaker)


func _is_talking() -> bool:
	return not _talk_current.is_empty() or not _talk_lines.is_empty() or not _talk_after.is_empty()


func _begin_talk(result: Dictionary) -> void:
	_talk_lines.append_array(result["lines"])
	_talk_after = result
	update_dialogue(0.0)


## The lines on their timer: fragments begin with an INFO's first line and end
## with its last, then the topics (or the end).
func update_dialogue(delta: float) -> void:
	if _talk_speaker == 0:
		return
	_talk_left -= delta
	while true:
		if not _talk_current.is_empty():
			if _talk_left > 0.0:
				return
			if _talk_current["last"]:
				papyrus.dialogue_fragment(_talk_current["info"], true, _talk_speaker)
			_talk_current = {}
		if _talk_lines.is_empty():
			break
		_talk_current = _talk_lines.pop_front()
		if _talk_current["first"]:
			papyrus.dialogue_fragment(_talk_current["info"], false, _talk_speaker)
		_talk_left = speech.say(_talk_speaker, _talk_current)  # the voice's length, else the text timer
		if not (_talk_current["text"] as String).is_empty():
			dialogue_line.emit(_talk_speaker, _talk_current["text"], _talk_left)
	if _talk_after.is_empty():
		return
	var after := _talk_after
	_talk_after = {}
	if after["closed"]:
		_finish_talk()
	else:
		_talk_topics = after["topics"]
		dialogue_topics.emit(_talk_topics)


## The scenes' frame: what the runner asks to be spoken goes through the
## speakers' queues, and an action is done when its lines are. A speaker in
## the player's dialogue is left alone until it ends.
func update_scenes(delta: float) -> void:
	var requests: Array = _scene_held
	_scene_held = []
	requests.append_array(papyrus.scenes_update(delta))
	for request: Dictionary in requests:
		if request["speaker"] == _talk_speaker:
			_scene_held.append(request)
			continue
		var key := "%d:%d" % [request["scene"], request["action"]]
		_scene_lines[key] = request["lines"].size()
		for line: Dictionary in request["lines"]:
			line["scene"] = request["scene"]
			line["action"] = request["action"]
			speech.say(request["speaker"], line)


func _on_speech_started(speaker: int, line: Dictionary, seconds: float) -> void:
	if line.has("scene") and not (line["text"] as String).is_empty():
		subtitle_line.emit(speaker, line["text"], seconds)


func _on_speech_ended(_speaker: int, line: Dictionary) -> void:
	if not line.has("scene"):
		return
	var key := "%d:%d" % [line["scene"], line["action"]]
	_scene_lines[key] = int(_scene_lines.get(key, 1)) - 1
	if _scene_lines[key] <= 0:
		_scene_lines.erase(key)
		papyrus.scene_action_done(line["scene"], line["action"])


func _finish_talk() -> void:
	speech.stop(_talk_speaker)
	papyrus.dialogue_close()
	_talk_speaker = 0
	_talk_lines = []
	_talk_current = {}
	_talk_after = {}
	_talk_topics = []
	dialogue_closed.emit()


# ---- actor values and the prompt

## Actor value `name` ("Health", "Stamina"...) of `ref`; 0 for no actor.
func actor_value(ref: int, name: String) -> float:
	return papyrus.get_actor_value(ref, name)


func actor_value_max(ref: int, name: String) -> float:
	return papyrus.get_actor_value_max(ref, name)


## What activating what the pointer points at would do: {ref, verb, name,
## lock_level} (lock_level -1: not locked), or {} if nothing is in reach or it
## has no name. Worked out once per frame.
func activation_target() -> Dictionary:
	var frame := Engine.get_process_frames()
	if frame != _target_frame:
		_target_frame = frame
		_target = _prompt.target(_pointer) if _prompt != null and not transition.is_fading() else {}
	return _target


# ---- saves

## Whether the player is dead (Health 0): the shell stops his controls.
func is_player_dead() -> bool:
	return papyrus != null and papyrus.is_player_dead()


## After a death: load the newest save (SaveService.newest) or, with none, start the start
## place again with full Health. False if neither worked.
func reload_or_restart() -> bool:
	var path := saves.newest()
	if path != "":
		return load_game(path)
	if _start.is_empty():
		return false
	var health := papyrus.get_actor_value_max(SkydotPapyrus.PLAYER_REF, "Health")
	var done: bool
	if _start["kind"] == "world":
		done = enter_world(_start["name"], _start["at"], _start["target"])
	else:
		done = enter_cell(_start["name"], _start["at"], _start["target"])
	papyrus.restore_actor_value(SkydotPapyrus.PLAYER_REF, "Health", health)
	return done


func save_game(path: String) -> bool:
	return saves.save(path)


func load_game(path: String) -> bool:
	var done := saves.load_game(path)
	if done:
		combat.on_items_changed()
	return done


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
	corpses.on_place_built(cell)
	combat.gear.on_place_built(cell)
	place_built.emit(cell, cell_id)


## The streamer registers the load doors of the cells it builds; an interior is
## built by the place.
func _on_interior_built(cell: Node3D, cell_id: int) -> void:
	_on_cell_built(cell, cell_id)
	streamer.register_doors(cell)


func _on_actor_left(ref: int, _door: int) -> void:
	print("0x%08X leaves through a door" % ref)
