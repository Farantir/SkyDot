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
## The player was shoved (PushActorAway): the source and the velocity given.
signal player_pushed(source: int, velocity: Vector3)
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
## A spell's effects landed on `target` (Magic.spell_landed).
signal spell_landed(hand: int, spell: int, target: int, point: Vector3, hostile: bool)

## Item categories `use_item` takes effect of.
const USABLE: Array[String] = ["potion", "food", "ingredient"]

## Metres in front of the player a dropped item lands, and above the feet.
const DROP_DISTANCE := 0.8
const DROP_HEIGHT := 0.3
## The ids of created forms (potions mixed by alchemy; id_ranges::created_forms).
const CREATED_FIRST := 0xFF700000
const CREATED_LAST := 0xFF7FFFFF

## The lines the player says for the words of a shout (VoicePowers topics Shout01a, Shout01b, Shout02,
## Shout03; which INFO plays, by voice type and equipped shout, is the dialogue's). The vanilla INFOs
## read "Fus!" (01a), "Fus..." (01b), "Ro!" (02) and "Ro...Da!" (03): one word is 01a; two words are 01b
## then 02; three words are 01b then 03.
const SHOUT_TOPICS := {1: [0x13E01], 2: [0x13E00, 0x13DFF], 3: [0x13E00, 0x13DFE]}
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
## The player's spells, scrolls and powers in the hands.
var magic: Magic
## What mages cast in a fight (the AI's combat_cast), carried out with Magic's delivery.
var npc_magic: NpcMagic
## What shouters say in a fight (the AI's combat_shout), carried out with the player's shout rules.
var npc_shouts: NpcShouts
## What a death looks like: ragdolls, corpse places.
var corpses: Corpses
## What visual effects and skin fades look like (placeholders).
var visual_effects: VisualEffects
## What scripts place, fire and play.
var placed: PlacedRefs
## The magic archetypes in the scene (lights, stagger, reanimate, bound weapons, slow time).
var magic_world: MagicWorld
## The player is paralysed: the shell reads no input, the hands are lowered.
var player_paralyzed := false
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
	papyrus.soul_trapped.connect(_on_soul_trapped)
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
		combat.stagger_wanted.connect(_on_stagger_wanted)
		# Every hit, the player's and the actors', raises the alarm.
		combat.hit_landed.connect(func(target: int, result: Dictionary, _point: Vector3) -> void:
			ai.on_hit(int(result.get("attacker", 0)), target))
	items_changed.connect(combat.on_items_changed)
	magic = Magic.new(world, pack, papyrus, host, combat, projectiles, func() -> Node3D: return _pointer)
	items_changed.connect(magic.on_items_changed)
	magic.spell_landed.connect(spell_landed.emit)
	magic.scroll_used.connect(func(_spell: int) -> void: items_changed.emit())
	magic.notice.connect(func(text: String) -> void: message.emit(text, false))
	magic.sound_wanted.connect(func(sound: int, source: int) -> void: sounds.play(sound, source))
	magic.shouted.connect(_on_shouted)
	papyrus.push_actor.connect(_on_push_actor)
	papyrus.knock_area.connect(_on_knock_area)
	if ai != null:
		# A hostile spell that lands on an actor raises the alarm, as a weapon's hit does.
		magic.spell_landed.connect(func(_hand: int, _spell: int, target: int, _point: Vector3, hostile: bool) -> void:
			if hostile and target != SkydotPapyrus.PLAYER_REF:
				ai.on_hit(SkydotPapyrus.PLAYER_REF, target))
	npc_magic = NpcMagic.new(world, papyrus, magic, combat)
	if ai != null:
		ai.combat_cast.connect(npc_magic.on_cast)
		# A hostile spell an actor lands on someone raises the alarm, as a weapon's hit does.
		magic.npc_spell_landed.connect(func(caster: int, _spell: int, target: int, _point: Vector3, hostile: bool) -> void:
			if hostile and target != caster:
				ai.on_hit(caster, target))
	npc_shouts = NpcShouts.new(world, papyrus, magic, combat, speech, pack)
	if ai != null:
		ai.combat_shout.connect(npc_shouts.on_shout)
	corpses = Corpses.new(world, papyrus, host, func() -> Vector3: return _feet)
	corpses.ai = ai
	actor_died.connect(corpses.on_died)
	combat.hit_landed.connect(corpses.on_hit)
	actor_arrived.connect(corpses.on_actor_arrived)
	visual_effects = VisualEffects.new(world, host, func() -> Vector3: return _feet + Vector3.UP * 1.2)
	papyrus.visual_effect.connect(visual_effects.on_visual_effect)
	papyrus.sub_graph_animation.connect(visual_effects.on_sub_graph_animation)
	papyrus.worn_changed.connect(_on_worn_changed)
	placed = PlacedRefs.new(world, papyrus, ai)
	host.add_child(placed)
	placed.actor_built.connect(corpses.on_actor_arrived)  # a dead one built again lies down
	magic_world = MagicWorld.new(self)
	host.add_child(magic_world)
	placed.place_on_screen = place
	placed.streamer = streamer
	papyrus.ref_placed.connect(placed.place)
	papyrus.ref_moved.connect(placed.moved)
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
	if from == SkydotPapyrus.PLAYER_REF and to != from and magic_world.is_bound(form):
		return 0  # PLACEHOLDER: a bound weapon cannot be given away or sold
	var moved := papyrus.transfer(from, to, form, count, extra)
	if moved > 0:
		items_changed.emit()
	return moved


## An actor's worn armor changed (EquipItem, UnequipItem): its built body gets the parts of what it wears
## now (a dragon's underskin and skin hider replace its skin). The player has no body to dress.
func _on_worn_changed(ref: int) -> void:
	if ref == SkydotPapyrus.PLAYER_REF or world.find_ref_node(ref) == null:
		return
	world.refresh_actor_parts(ref, papyrus.get_ref_base(ref))


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
	if caster == SkydotPapyrus.PLAYER_REF:
		papyrus.break_invisibility(caster)
	return papyrus.cast_spell(caster, spell, target, seconds)


func unequip(form: int, extra := -1) -> bool:
	var done := papyrus.unequip(SkydotPapyrus.PLAYER_REF, form, extra)
	if done:
		# Putting a bound weapon away ends its effect.
		if magic_world.is_bound(form):
			papyrus.dispel_bound_weapon(SkydotPapyrus.PLAYER_REF, form)
		items_changed.emit()
	return done


## Drop up to `count` of `form` as one reference in front of the player, which
## keeps the count and is picked up by activating it. Returns the reference, 0
## if the player has none.
func drop(form: int, count: int, extra := -1) -> int:
	if magic_world.is_bound(form):
		message.emit("A bound weapon cannot be dropped", false)  # PLACEHOLDER: the game says nothing
		return 0
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


## What an enchanting table lists: {disenchant, items, enchantments, gems}, see
## SkydotPapyrus.enchant_rows.
func enchant_rows() -> Dictionary:
	return papyrus.enchant_rows()


## Whether the learned enchantment fits the item (its kind and worn restrictions).
func enchantment_fits(item: int, enchantment: int) -> bool:
	return papyrus.enchantment_fits(item, enchantment)


## What enchanting would make: {ok, refusal, name, effects, charge, cost, uses, constant}.
func enchant_preview(item: int, item_extra: int, enchantment: int, gem: int, gem_extra: int, name := "") -> Dictionary:
	return papyrus.enchant_preview(item, item_extra, enchantment, gem, gem_extra, name)


## Enchant one unit with a learned enchantment and a filled soul gem (spent, unless it is reusable);
## {ok, refusal, name, extra, created}. `name` empty takes "<item> of <enchantment>".
func enchant(item: int, item_extra: int, enchantment: int, gem: int, gem_extra: int, name := "") -> Dictionary:
	var result := papyrus.enchant(item, item_extra, enchantment, gem, gem_extra, name)
	if result["ok"]:
		message.emit("%s enchanted" % result["name"], false)
		print("enchanting: made %s (created 0x%X, extra %d)" % [result["name"], int(result["created"]), int(result["extra"])])
		items_changed.emit()
	elif result["refusal"] != "":
		message.emit(result["refusal"], false)
	return result


## Destroy one unit and learn its enchantment: {ok, refusal, learned}.
func disenchant(form: int, extra: int) -> Dictionary:
	var result := papyrus.disenchant(form, extra)
	if result["ok"]:
		message.emit("Learned %s" % result["learned"], false)
		print("enchanting: learned %s" % result["learned"])
		items_changed.emit()
	elif result["refusal"] != "":
		message.emit(result["refusal"], false)
	return result


## The filled soul gems that could recharge the weapon unit.
func recharge_options(item: int, extra: int) -> Array:
	return papyrus.recharge_options(item, extra)


## Recharge a weapon unit with a filled soul gem: {ok, refusal, charge, max, extra}.
func recharge(item: int, extra: int, gem: int, gem_extra: int) -> Dictionary:
	var result := papyrus.recharge(item, extra, gem, gem_extra)
	if result["ok"]:
		message.emit("Recharged to %d" % int(result["charge"]), false)
		print("enchanting: recharged to %d of %d" % [int(result["charge"]), int(result["max"])])
		items_changed.emit()
	elif result["refusal"] != "":
		message.emit(result["refusal"], false)
	return result


## Soul Trap killed `actor`: the notification and the log line.
func _on_soul_trapped(actor: int, filled: bool, gem: int, soul: int) -> void:
	if filled:
		message.emit("Soul captured!", false)  # GMST sSoulCaptured
		print("soul trap: 0x%X filled gem 0x%X with soul %d" % [actor, gem, soul])
		items_changed.emit()
	else:
		print("soul trap: 0x%X, no soul gem fits" % actor)


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
	magic.step(delta)
	npc_magic.step(delta)
	npc_shouts.step(delta)
	magic_world.step(delta)
	corpses.step(delta)
	visual_effects.step(delta)
	_drain_sprint(delta)
	if ai != null:
		clock.sync(benchmarking)
		ai.set_player(SkydotWorld.godot_to_skyrim(feet), not papyrus.is_player_dead())
		ai.set_player_view(_viewpoint)  # Actor.HasLOS of the player looks from the camera
		ai.update(delta)


## The fights under way only, for runs that keep the world still (shots). The player's feet are
## kept as `step` keeps them: fighters and NPC spells aim at them.
func update_fights(delta: float, feet: Vector3) -> void:
	_feet = feet
	if ai != null:
		ai.set_player(SkydotWorld.godot_to_skyrim(feet), not papyrus.is_player_dead())
		ai.set_player_view(_viewpoint)
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

## Draw the player's weapon, or sheathe it (R, gamepad X). Hands that hold spells or scrolls
## are readied with it: alone when the right hand holds no weapon, else in step with the weapon.
func toggle_weapon() -> void:
	if papyrus.is_player_dead() or player_paralyzed:
		return
	if magic.hands_with_spells().is_empty():
		combat.toggle_weapon()
		magic.set_ready(false, true)
	elif combat.player_weapon() != 0:
		combat.toggle_weapon()
		var drawn := combat.weapon_state() == Combat.State.DRAWING or combat.weapon_state() == Combat.State.DRAWN
		magic.set_ready(drawn, true)
	else:
		magic.toggle_ready()


## The attack button went down or up: a short press is a light attack, a long
## one a power attack (Combat.attack_pressed). A spell in the right hand is cast instead.
func attack_pressed() -> void:
	if papyrus.is_player_dead() or player_paralyzed:
		return
	papyrus.break_invisibility(SkydotPapyrus.PLAYER_REF)
	if magic.spell_in(Magic.RIGHT) != 0:
		magic.hand_pressed(Magic.RIGHT)
	elif combat.weapon_state() == Combat.State.SHEATHED and not magic.hands_with_spells().is_empty():
		toggle_weapon()
	else:
		combat.attack_pressed()


func attack_released() -> void:
	magic.hand_released(Magic.RIGHT)
	combat.attack_released()


## The left hand's attack button (right mouse, left trigger) went down or up: a spell in
## the left hand is cast (it is reserved for blocking otherwise).
func attack_left_pressed() -> void:
	if not papyrus.is_player_dead() and not player_paralyzed and magic.spell_in(Magic.LEFT) != 0:
		papyrus.break_invisibility(SkydotPapyrus.PLAYER_REF)
		magic.hand_pressed(Magic.LEFT)


func attack_left_released() -> void:
	magic.hand_released(Magic.LEFT)


## The power key: cast the power in the power slot (Magic.cast_power). {ok, reason}.
func cast_power() -> Dictionary:
	return {"ok": false, "reason": "dead"} if papyrus.is_player_dead() else magic.cast_power()


## The voice key went down: the voice slot holds a power (cast at once, like `cast_power`) or a
## shout (it charges while the key is down, Magic.shout_begin). {ok, reason}.
func voice_pressed() -> Dictionary:
	if papyrus.is_player_dead():
		return {"ok": false, "reason": "dead"}
	var shout := int(papyrus.equipped_shout(SkydotPapyrus.PLAYER_REF))
	if shout == 0:
		return cast_power()
	if player_paralyzed:
		return {"ok": false, "reason": "paralyzed"}
	return magic.shout_begin(shout)


## The voice key went up: a charging shout is said (Magic.shout_release).
func voice_released() -> void:
	magic.shout_release()


## Put `shout` in the voice slot (it empties the power slot). Not while a shout is being said:
## sFailShouting, "You cannot change shouts while shouting."
func equip_shout(shout: int) -> bool:
	if magic.is_shouting():
		message.emit(world.get_gmst_string("sFailShouting", "You cannot change shouts while shouting."), false)
		return false
	var done := papyrus.equip_shout(SkydotPapyrus.PLAYER_REF, shout)
	if done:
		items_changed.emit()
	return done


## The player's shouts for the magic menu: shout_info of each (name, description, words with their
## names, translations and known / unlocked flags, usable, equipped), by name.
func known_shouts() -> Array:
	var out: Array = []
	for shout in papyrus.shouts_of(SkydotPapyrus.PLAYER_REF):
		var info := papyrus.shout_info(SkydotPapyrus.PLAYER_REF, shout)
		if info["found"]:
			out.append(info)
	out.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return String(a["name"]) < String(b["name"]))
	return out


## The player's dragon souls.
func dragon_souls() -> int:
	return roundi(actor_value(SkydotPapyrus.PLAYER_REF, "DragonSouls"))


## Spend a dragon soul on `word` (SkydotPapyrus.unlock_word_with_soul). {ok, reason}; a refusal is also
## said on the message line (sFailSpendSoul with no soul).
func unlock_word(word: int) -> Dictionary:
	var result := papyrus.unlock_word_with_soul(SkydotPapyrus.PLAYER_REF, word)
	if not result["ok"] and not String(result["reason"]).is_empty():
		message.emit(result["reason"], false)
	items_changed.emit()
	return result


## The voice meter the HUD draws: {recovering, charging, fill}. `fill` rises from 0 to 1 as the recovery
## runs out (the seconds left against those of the last shout), or as a charging shout nears the held
## time of its most usable words.
func voice_meter() -> Dictionary:
	var left := papyrus.voice_recovery(SkydotPapyrus.PLAYER_REF)
	var charge := magic.shout_state()
	if not charge.is_empty() and not charge["fired"] and float(charge["threshold"]) > 0.0:
		return {"recovering": false, "charging": true, "fill": clampf(float(charge["held"]) / float(charge["threshold"]), 0.0, 1.0)}
	if left > 0.0:
		var total := maxf(magic.last_recovery, left)  # a loaded game knows only what is left
		return {"recovering": true, "charging": false, "fill": clampf(1.0 - left / total, 0.0, 1.0)}
	return {"recovering": false, "charging": false, "fill": 1.0 if magic.is_shouting() else 0.0}


func _on_shouted(_shout: int, words: int, _spell: int, _info: Dictionary) -> void:
	for topic: int in SHOUT_TOPICS[clampi(words, 1, 3)]:
		var line := papyrus.topic_line(SkydotPapyrus.PLAYER_REF, topic)
		if line.is_empty() or String(line.get("voice", "")).is_empty():
			print("shout: voice missing (topic 0x%X)" % topic)
			continue
		print("shout: voice %s" % line["voice"])
		speech.say(SkydotPapyrus.PLAYER_REF, line)


## PushActorAway: a living actor is knocked down (Corpses.knock_down); the player's own knock-down is
## not done (PLACEHOLDER: no ragdoll for the player).
func _on_push_actor(source: int, target: int, force: float) -> void:
	if target == SkydotPapyrus.PLAYER_REF:
		_push_player(source, force)
		return
	if not corpses.knock_down(target, source, force):
		print("push: 0x%08X is not knocked down (dead, not built or down already)" % target)


## The player is shoved away from `source` (no ragdoll, no lost control): a velocity along source to
## player with Corpses' speed per unit of force and lift, which the walk's acceleration takes out again.
func _push_player(source: int, force: float) -> void:
	var node := world.find_ref_node(source) as Node3D
	var away := Vector3.ZERO if node == null else _feet - node.global_position
	away.y = 0.0
	if away.length() < 0.001:
		away = -_viewpoint.global_transform.basis.z
		away.y = 0.0
	var speed := (away.normalized() + Vector3.UP * Corpses.PUSH_LIFT).normalized() * force * Corpses.PUSH_SPEED_PER_FORCE
	var body: Object = _player.get("player") if _player != null else null
	if body == null or not body.has_method("impulse"):
		print("push: the player is pushed by 0x%08X (force %s): no body to push" % [source, force])
		return
	body.impulse(speed)
	player_pushed.emit(source, speed)
	print("push: the player is pushed by 0x%08X (force %s, %.1f m/s)" % [source, force, speed.length()])


## KnockAreaEffect: every actor within `radius` game units of the source is pushed away with `magnitude`.
func _on_knock_area(source: int, magnitude: float, radius: float) -> void:
	var at := _feet
	if source != SkydotPapyrus.PLAYER_REF:
		var node := world.find_ref_node(source) as Node3D
		if node == null:
			return
		at = node.global_position
	var near := combat.actors_within(source, at, radius * SkydotWorld.unit_scale())
	print("push: area of 0x%08X (magnitude %s, radius %s) reaches %d actors" % [source, magnitude, radius, near.size()])
	for body in near:
		_on_push_actor(source, body["ref"], magnitude)


## Put `spell` (a known spell or a carried scroll) in `hand` (Magic.LEFT, RIGHT, POWER).
func equip_spell(spell: int, hand: int) -> bool:
	var done := papyrus.equip_spell(SkydotPapyrus.PLAYER_REF, spell, hand)
	if done:
		items_changed.emit()
	return done


## The player's spells for the magic menu: cast_info of each spell (type 0), power and lesser power
## (2, 3), plus `cost` (Magicka, rounded) and `cooldown` (game days left of a power, as
## SkydotPapyrus.power_cooldown). Abilities, diseases and poisons are not listed.
func known_spells() -> Array:
	var out: Array = []
	for form in papyrus.spells_of(SkydotPapyrus.PLAYER_REF):
		var info := papyrus.cast_info(form)
		if not info["found"] or not int(info["type"]) in [0, 2, 3]:
			continue
		info["cost"] = roundi(papyrus.spell_cost(SkydotPapyrus.PLAYER_REF, form))
		info["cooldown"] = papyrus.power_cooldown(SkydotPapyrus.PLAYER_REF, form) if int(info["type"]) >= 2 else 0.0
		out.append(info)
	return out


## cast_info of a spell or a scroll the player holds, {} if unknown.
func spell_info(form: int) -> Dictionary:
	var info := papyrus.cast_info(form)
	return info if info["found"] else {}


## The spell or scroll in `hand` (Magic.LEFT, RIGHT, POWER), 0 for none.
func spell_in_hand(hand: int) -> int:
	return magic.spell_in(hand)


## The effects acting on the player (SkydotPapyrus.active_effects).
func player_effects() -> Array:
	return papyrus.active_effects(SkydotPapyrus.PLAYER_REF)


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


## PLACEHOLDER: the shove of a stagger blow on the player, force per magnitude (see _push_player).
const PLAYER_STAGGER_FORCE := 8.0


## A blow staggers `target`: an actor takes SkydotAi.stagger (its stagger clip); the player is shoved and
## logs it (no camera shake hook exists yet).
func _on_stagger_wanted(source: int, target: int, magnitude: float) -> void:
	if target == SkydotPapyrus.PLAYER_REF:
		print("combat: the player staggers (%s) from 0x%X" % [magnitude, source])
		_push_player(source, magnitude * PLAYER_STAGGER_FORCE)
	elif ai != null:
		ai.stagger(target, magnitude)


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
	magic.step(delta)
	npc_magic.step(delta)
	npc_shouts.step(delta)
	magic_world.step(delta)
	corpses.step(delta)
	visual_effects.step(delta)


## Debug (--kill): hit `ref` with the weapon in hand until it is dead. Returns
## the hits it took; 0 if it was dead already or is no actor.
func kill_actor(ref: int) -> int:
	var hits := 0
	var weapon := combat.player_weapon()
	var extra: int = papyrus.equipment(SkydotPapyrus.PLAYER_REF)["right_extra"]
	while hits < 2000 and not papyrus.is_actor_dead(ref):
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
	if player_paralyzed:
		return false
	papyrus.break_invisibility(SkydotPapyrus.PLAYER_REF)
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
		magic.on_items_changed()
		magic_world.on_loaded()
	return done


## The player's feet (Godot space) as of the last step.
func player_feet() -> Vector3:
	return _feet


func player_object() -> Object:
	return _player


## Hands down: the weapon sheathed and the spell hands unreadied (paralysis).
func lower_hands() -> void:
	magic.set_ready(false, true)
	if combat.weapon_state() == Combat.State.DRAWN or combat.weapon_state() == Combat.State.DRAWING:
		combat.toggle_weapon()


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
	placed.cell_built(cell_id)
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
