# SPDX-License-Identifier: GPL-3.0-or-later
#
# The viewer's options (see the header of cell_viewer.gd), read once from the
# command line, or from a shot's JSON with --from-shot, into typed fields.
# Nothing here changes while the viewer runs; what keys change (radius, LOD
# detail, preloading) is copied into the components that own it.
class_name ViewerSettings
extends RefCounted

const SHOT_FORMAT := 1  # "format" of the JSON a shot writes (ShotRecorder)
const MSAA_STEPS := [Viewport.MSAA_DISABLED, Viewport.MSAA_2X, Viewport.MSAA_4X, Viewport.MSAA_8X]
## Options without a value: the next token is another option, not theirs.
const FLAGS := ["no-input", "hud", "invert-look-y", "open-inventory", "open-magic", "ready-weapon", "god"]
const MSAA_NAMES := ["off", "2", "4", "8"]
## --aa: screen-space anti-aliasing after the frame is drawn (not temporal).
const AA_STEPS := [Viewport.SCREEN_SPACE_AA_DISABLED, Viewport.SCREEN_SPACE_AA_FXAA, Viewport.SCREEN_SPACE_AA_SMAA]
const AA_NAMES := ["off", "fxaa", "smaa"]

## Why the options cannot be used; empty when they can.
var error := ""
## --from-shot: the note stored with the shot, to print.
var shot_note := ""

# Where to start.
var pack := ""
var cell := ""  # an interior, by editor id
var world := ""  # a worldspace, by editor id
var has_at := false
var at := Vector3.ZERO  # game units, as `bethconv cell` prints them
var has_target := false
var target := Vector3.ZERO  # game units
var has_look := false
var look_yaw := 0.0  # radians, engine terms
var look_pitch := 0.0

# What is built.
var materials := true
var effects := true
var grass := true
var all_light_shadows := false  # every placed light casts shadows
var collision := true
var navigation := true
var actors := true
var wander := true
var ai := true
var has_tiling := false
var tiling := 0.0  # land texture repeats per cell
var lod := true
var large_refs := true  # large references as models beyond the cells in full
var large_ref_radius := 5  # cells (the game's uLargeRefLODGridSize 11 is radius 5)
var has_tree_distance := false
var tree_distance := 0.0
var shadows := true  # the sun's, outside
var image_space := true
var neutral_grade := false  # --image-space neutral
var volumetric := true  # the weather's volumetric lighting haze
var quests := true
var set_stage := ""  # EDID:STAGE[,EDID:STAGE...]
var god := false  # --god: the player's Health is restored every frame (like tgm)
var shout_wait := ""  # --shout-wait EDID:STAGE: the shout flags wait until that stage is done

# Time and weather.
var time := 12.0  # hour to start at
var time_scale := 20.0  # game seconds per second
var weather := ""  # keep this one; empty: they take turns

# The view.
var radius := 2  # cells around the camera built in full
var lod_split := 1.5
var msaa_index := 2  # into MSAA_STEPS: 4x
var aa_index := 0  # into AA_STEPS
var foliage_bias := 0.5  # mip levels blurrier for alpha-tested trees and grass
var render_scale := 1.0  # of the window; above 1 draws bigger and averages down (supersampling)
var has_fov := false
var fov := 75.0  # degrees, vertical
var fly := false  # starts flying through everything
var build_budget_usec := 8000  # per frame for streaming cells in

# Doors.
var pick_locks := false
var preload_doors := true  # build the place behind a near load door ahead
var preload_distance := 15.0  # metres

# Runs that drive themselves.
var activate: Array[int] = []  # references to activate in turn, then quit
var has_activate := false
var damage: Array[Vector2] = []  # --damage 0xREF=AMOUNT: DamageObject calls after the activations
var activate_wait := 0  # frames between two --activate references (0: next frame)
var quit_after := -1  # frames after the last --activate before quitting (-1: 5)
var start_quests := PackedStringArray()  # --start-quest EDID[,EDID]: started after the load
var give: Array[Vector2i] = []  # (form id, count) the player starts with (--give)
var save_to := ""  # saves when the --activate run ends
var load_path := ""  # loads at start
var has_screenshot := false
var screenshot := ""
var shot_delay := 0.0  # seconds the world runs before --screenshot captures
var shot_frames := 1  # --screenshot of one view: consecutive frames to save (an effect over time)
var shot_every := 1  # of those, every this many rendered frames
var hide_refs: Array[int] = []  # --screenshot: references (form IDs) hidden in the shots
var has_benchmark := false
var benchmark := 0.0  # seconds
var fly_speed := 20.0  # m/s during the benchmark
var captures := false  # screenshot or benchmark: still time, still actors
var hud := true  # health, magicka and stamina bars, crosshair, activation prompt
var open_inventory := false  # --open-inventory: show the inventory after the load
var open_magic := false  # --open-magic: show the magic menu after the load
var learn_enchantment: Array[int] = []  # --learn-enchantment 0xENCH[,...]: the player knows those enchantments (for the enchanting table)
var crafting_category := ""  # --crafting-category NAME|N: the enchanting table's category (Disenchant, Item, Enchantment, Soul gem)
var magic_category := ""  # --magic-category NAME: the category it shows (Powers, Destruction, Scrolls...)
var open_container := 0  # --open-container 0xREF: activate this reference as the player after the load
var talk := 0  # --talk 0xREF: open a dialogue with this actor after the load
var ready_weapon := false  # --ready-weapon: draw the weapon after the load
var attack_at := 0  # --attack-at 0xREF[,power]: face that actor and attack once it is in reach
var attack_power := false
var fight: Array[int] = []  # --fight 0xREF[,...]: those actors fight the player after the load
var strip_melee: Array[int] = []  # --strip-melee 0xREF[,...]: those actors lose their melee weapons
var npc_give: Array[Dictionary] = []  # --npc-give 0xREF:0xFORM[xN],...: {ref, form, count}
var npc_draw: Array[int] = []  # --npc-draw 0xREF[,...]: those actors draw their weapons after the load
var kill: Array[int] = []  # --kill 0xREF[,...]: hit those actors with the weapon in hand until dead
var equip: Array[int] = []  # --equip 0xFORM[,...]: the player equips those (after --give)
var equip_spell: Array[Dictionary] = []  # --equip-spell 0xFORM[,left|right|power]: {form, hand} (Magic.RIGHT by default)
var cast_at := 0  # --cast-at 0xREF[,left|right]: ready the hands, face that actor, cast the hand's spell at it
var cast_at_hand := 1
var teach_word: Array[int] = []  # --teach-word 0xWOOP[,...]: the player learns those words of power (known, locked)
var unlock_word: Array[int] = []  # --unlock-word 0xWOOP[,...]: those words are unlocked
var equip_shout := 0  # --equip-shout 0xSHOU: the shout in the voice slot
var dragon_souls := -1  # --dragon-souls N: the player's DragonSouls (-1: unchanged)
var shout_words := 0  # --shout N[,0xREF]: say N words (hold the voice key for them) facing that actor
var shout_at := 0
var shout_side := 0  # --shout-side K: the first of twelve sides round the actor --shout-from tries
var kill_after := 0.0  # --kill-after SECONDS: --kill waits that long (a spawned actor needs its scripts first)
var shout_from := 0.0  # --shout-from METRES: stand that far from the actor first (a shot)
var spawn: Array[int] = []  # --spawn 0xNPC_[,...]: those actors (NPC_ or LVLN) appear in front of the player after the load
var spawn_distance := SpawnActors.DISTANCE  # --spawn-distance METRES: how far ahead --spawn puts them
var drink: Array[int] = []  # --drink 0xFORM[,...]: the player uses those items (added first if not carried)
var cast: Array[Dictionary] = []  # --cast 0xSPELL[@0xREF][,...]: {spell, target}, the player casts (0: on itself)
var damage_actor: Array[Dictionary] = []  # --damage-actor 0xREF:AV:AMOUNT, applied once at the start
var stick_look_speed := 3.0  # rad/s at full deflection of the right stick (pitch: 0.7 of it)
var invert_look_y := false  # the right stick looks down when pushed up
var interactive := true  # false: ignore the keyboard and mouse

# F12.
var shot_dir := "user://screenshots"
var has_shot_camera := false  # --shot-camera X,Y,Z --shot-look X,Y,Z: a series' camera stands there (Skyrim units) while the player stays at --at
var shot_camera := Vector3.ZERO
var shot_look := Vector3.ZERO
var shot_notes := true  # ask what is wrong after a shot

var has_pck := false  # given, though ignored


## What the game session is opened with, from these options.
func game_config() -> GameConfig:
	var c := GameConfig.new()
	c.pack = pack
	c.materials = materials
	c.effects = effects
	c.grass = grass
	c.all_light_shadows = all_light_shadows
	c.collision = collision
	c.navigation = navigation
	c.actors = actors
	c.wander = wander
	c.ai = ai
	c.has_tiling = has_tiling
	c.tiling = tiling
	c.foliage_bias = foliage_bias
	c.shadows = shadows
	c.radius = radius
	c.build_budget_usec = build_budget_usec
	c.lod = lod
	c.lod_split = lod_split
	c.large_refs = large_refs
	c.large_ref_radius = large_ref_radius
	c.has_tree_distance = has_tree_distance
	c.tree_distance = tree_distance
	c.preload_doors = preload_doors
	c.preload_distance = preload_distance
	c.time = time
	c.time_scale = time_scale
	c.still = captures
	c.weather = weather
	c.quests = quests
	c.set_stage = set_stage
	c.pick_locks = pick_locks
	c.activate = activate.duplicate()
	c.damage = damage.duplicate()
	c.activate_wait = activate_wait
	return c


## The settings for `list` (the user arguments). `error` says if they cannot
## be used.
static func from_arguments(list: PackedStringArray) -> ViewerSettings:
	var settings := ViewerSettings.new()
	var args := _split(list)
	if args.has("from-shot"):
		var shot := _arguments_from_shot(args["from-shot"])
		if shot.is_empty():
			settings.error = "cannot read the shot " + args["from-shot"]
			return settings
		settings.shot_note = shot.get("note", "")
		shot.erase("note")
		shot.merge(args, true)  # the command line wins
		args = shot
	settings._read(args)
	return settings


func _read(args: Dictionary) -> void:
	time = args.get("time", "12").to_float()
	if not args.has("pack") or not (args.has("cell") or args.has("world")):
		error = "usage: -- --pack DIR (--cell EDITOR_ID | --world EDITOR_ID --at X,Y,Z)" \
			+ " [--screenshot out.png]"
		return
	pack = args["pack"]
	cell = args.get("cell", "")
	world = args.get("world", "")
	has_pck = args.has("pck")
	has_screenshot = args.has("screenshot")
	screenshot = args.get("screenshot", "")
	has_benchmark = args.has("benchmark")
	benchmark = args.get("benchmark", "0").to_float()
	captures = has_screenshot or has_benchmark
	has_activate = args.has("activate")
	if has_activate:
		for id in args["activate"].split(","):
			activate.append(id.hex_to_int() if id.begins_with("0x") else id.to_int())
	for entry: String in args.get("damage", "").split(",", false):
		var eq := entry.find("=")
		var ref_text := entry if eq < 0 else entry.substr(0, eq)
		var ref: int = ref_text.hex_to_int() if ref_text.begins_with("0x") else ref_text.to_int()
		damage.append(Vector2(ref, entry.substr(eq + 1).to_float() if eq >= 0 else 1.0))
		has_activate = true  # a run with damage ends by itself
	for entry: String in args.get("give", "").split(",", false):
		var cut := entry.find("x", 2)  # 0xFORMxN: the count follows the second x
		var form_text := entry if cut < 0 else entry.substr(0, cut)
		var form: int = form_text.hex_to_int() if form_text.begins_with("0x") else form_text.to_int()
		give.append(Vector2i(form, maxi(1, entry.substr(cut + 1).to_int()) if cut >= 0 else 1))
	start_quests = PackedStringArray(String(args.get("start-quest", "")).split(",", false))
	activate_wait = maxi(0, args.get("activate-wait", "0").to_int())
	quit_after = maxi(0, args["quit-after"].to_int()) if args.has("quit-after") else -1
	hud = args.has("hud") or not captures
	open_inventory = args.has("open-inventory")
	open_magic = args.has("open-magic")
	magic_category = args.get("magic-category", "")
	crafting_category = args.get("crafting-category", "")
	for entry: String in args.get("learn-enchantment", "").split(",", false):
		learn_enchantment.append(entry.hex_to_int() if entry.begins_with("0x") else entry.to_int())
	if args.has("open-container"):
		var container_text: String = args["open-container"]
		open_container = container_text.hex_to_int() if container_text.begins_with("0x") else container_text.to_int()
	if args.has("talk"):
		var talk_text: String = args["talk"]
		talk = talk_text.hex_to_int() if talk_text.begins_with("0x") else talk_text.to_int()
	ready_weapon = args.has("ready-weapon") or args.has("attack-at")
	if args.has("attack-at"):
		var parts: PackedStringArray = String(args["attack-at"]).split(",")
		attack_at = parts[0].hex_to_int() if parts[0].begins_with("0x") else parts[0].to_int()
		attack_power = parts.size() > 1 and parts[1] in ["power", "1", "true"]
	for entry: String in args.get("kill", "").split(",", false):
		kill.append(entry.hex_to_int() if entry.begins_with("0x") else entry.to_int())
	for entry: String in args.get("fight", "").split(",", false):
		fight.append(entry.hex_to_int() if entry.begins_with("0x") else entry.to_int())
	for entry: String in args.get("strip-melee", "").split(",", false):
		strip_melee.append(entry.hex_to_int() if entry.begins_with("0x") else entry.to_int())
	for entry: String in args.get("npc-give", "").split(",", false):
		var halves: PackedStringArray = entry.split(":")
		if halves.size() == 2:
			var cut: int = halves[1].rfind("x")
			var has_count := cut > 1
			npc_give.append({"ref": halves[0].hex_to_int(),
				"form": (halves[1].substr(0, cut) if has_count else halves[1]).hex_to_int(),
				"count": halves[1].substr(cut + 1).to_int() if has_count else 1})
	for entry: String in args.get("npc-draw", "").split(",", false):
		npc_draw.append(entry.hex_to_int() if entry.begins_with("0x") else entry.to_int())
	for entry: String in args.get("equip", "").split(",", false):
		equip.append(entry.hex_to_int() if entry.begins_with("0x") else entry.to_int())
	for entry: String in str(args.get("equip-spell", "")).split(",", false):
		if entry in ["left", "right", "power"] and not equip_spell.is_empty():
			equip_spell[-1]["hand"] = _hand_of(entry)  # the hand of the form before it
		else:
			equip_spell.append({"form": entry.hex_to_int() if entry.begins_with("0x") else entry.to_int(), "hand": 1})
	for entry: String in str(args.get("teach-word", "")).split(",", false):
		teach_word.append(entry.hex_to_int() if entry.begins_with("0x") else entry.to_int())
	for entry: String in str(args.get("unlock-word", "")).split(",", false):
		unlock_word.append(entry.hex_to_int() if entry.begins_with("0x") else entry.to_int())
	if args.has("equip-shout"):
		var shout_text: String = args["equip-shout"]
		equip_shout = shout_text.hex_to_int() if shout_text.begins_with("0x") else shout_text.to_int()
	if args.has("dragon-souls"):
		dragon_souls = maxi(0, str(args["dragon-souls"]).to_int())
	shout_side = str(args.get("shout-side", "0")).to_int()
	shout_from = maxf(0.0, str(args.get("shout-from", "0")).to_float())
	if args.has("shout"):
		var shout_parts: PackedStringArray = String(args["shout"]).split(",")
		shout_words = clampi(shout_parts[0].to_int(), 1, 3)
		if shout_parts.size() > 1:
			shout_at = shout_parts[1].hex_to_int() if shout_parts[1].begins_with("0x") else shout_parts[1].to_int()
	if args.has("cast-at"):
		var cast_parts: PackedStringArray = String(args["cast-at"]).split(",")
		cast_at = cast_parts[0].hex_to_int() if cast_parts[0].begins_with("0x") else cast_parts[0].to_int()
		cast_at_hand = _hand_of(cast_parts[1]) if cast_parts.size() > 1 else 1
	kill_after = maxf(0.0, str(args.get("kill-after", "0")).to_float())
	spawn_distance = maxf(0.5, str(args.get("spawn-distance", str(SpawnActors.DISTANCE))).to_float())
	for entry: String in args.get("spawn", "").split(",", false):
		spawn.append(entry.hex_to_int() if entry.begins_with("0x") else entry.to_int())
	for entry: String in args.get("drink", "").split(",", false):
		drink.append(entry.hex_to_int() if entry.begins_with("0x") else entry.to_int())
	for entry: String in args.get("cast", "").split(",", false):
		var halves: PackedStringArray = entry.split("@")
		cast.append({"spell": halves[0].hex_to_int() if halves[0].begins_with("0x") else halves[0].to_int(),
			"target": 0 if halves.size() < 2 else (halves[1].hex_to_int() if halves[1].begins_with("0x") else halves[1].to_int())})
	for entry: String in args.get("damage-actor", "").split(",", false):
		var parts: PackedStringArray = entry.split(":")
		if parts.size() != 3 or parts[1].is_empty():
			error = "--damage-actor wants 0xREF:ActorValue:AMOUNT, got " + entry
			return
		damage_actor.append({"ref": parts[0].hex_to_int() if parts[0].begins_with("0x") else parts[0].to_int(),
			"av": parts[1], "amount": parts[2].to_float()})
	stick_look_speed = maxf(args.get("stick-look-speed", "3").to_float(), 0.0)
	invert_look_y = args.has("invert-look-y")
	interactive = not (captures or has_activate or args.has("no-input"))

	materials = args.get("materials", "on") != "off"
	effects = args.get("effects", "on") != "off"
	grass = args.get("grass", "on") != "off"
	all_light_shadows = args.get("light-shadows", "game") == "all"
	collision = args.get("collision", "on") != "off"
	navigation = args.get("navigation", "on") != "off"
	actors = args.get("actors", "on") != "off"
	wander = args.get("wander", "off" if captures else "on") != "off"
	ai = args.get("ai", "on") != "off"
	has_tiling = args.has("tiling")
	tiling = args.get("tiling", "0").to_float()
	lod = args.get("lod", "on") != "off"
	large_refs = args.get("large-refs", "on") != "off"
	large_ref_radius = args.get("large-ref-radius", "5").to_int()
	has_tree_distance = args.has("tree-distance")
	tree_distance = args.get("tree-distance", "0").to_float()
	shadows = args.get("shadows", "on") != "off"
	image_space = args.get("image-space", "on") != "off"
	neutral_grade = args.get("image-space", "on") == "neutral"
	volumetric = args.get("volumetric", "on") != "off"
	quests = args.get("quests", "on") != "off"
	set_stage = args.get("set-stage", "")
	god = args.has("god")
	shout_wait = args.get("shout-wait", "")

	time_scale = args.get("time-scale", "20").to_float()
	weather = args.get("weather", "")

	radius = args.get("radius", "2").to_int()
	lod_split = args.get("lod-split", "1.5").to_float()
	msaa_index = maxi(MSAA_NAMES.find(args.get("msaa", "4")), 0)
	aa_index = maxi(AA_NAMES.find(args.get("aa", "off")), 0)
	foliage_bias = clampf(args.get("foliage-bias", "0.5").to_float(), 0.0, 4.0)
	render_scale = clampf(args.get("render-scale", "1").to_float(), 1.0, 2.0)
	has_fov = args.has("fov")
	fov = args.get("fov", "75").to_float()
	fly = args.get("walk", "on") == "off" or not collision or captures
	build_budget_usec = args.get("build-budget", "8000").to_int()

	pick_locks = args.get("pick-locks", "off") != "off"
	preload_doors = args.get("preload-doors", "off" if captures else "on") != "off"
	preload_distance = args.get("preload-distance", "15").to_float()

	save_to = args.get("save-to", "")
	load_path = args.get("load", "")
	shot_delay = args.get("shot-delay", "0").to_float()
	shot_frames = maxi(args.get("shot-frames", "1").to_int(), 1)
	shot_every = maxi(args.get("shot-every", "1").to_int(), 1)
	for id in args.get("hide-refs", "").split(",", false):
		hide_refs.append(id.hex_to_int())
	fly_speed = args.get("fly-speed", "20").to_float()
	shot_dir = args.get("shot-dir", "user://screenshots")
	shot_notes = args.get("shot-notes", "on") != "off"

	if args.has("shot-camera") and args.has("shot-look"):
		var from = _vec3(args["shot-camera"])
		var to = _vec3(args["shot-look"])
		if from == null or to == null:
			error = "--shot-camera and --shot-look want X,Y,Z"
			return
		has_shot_camera = true
		shot_camera = from
		shot_look = to
	has_at = args.has("at")
	if has_at:
		var value = _vec3(args["at"])
		if value == null:
			error = "--at wants X,Y,Z, got " + args["at"]
			return
		at = value
	has_target = args.has("target")
	if has_target:
		var value = _vec3(args["target"])
		if value == null:
			error = "--target wants X,Y,Z, got " + args["target"]
			return
		target = value
	if args.has("look"):
		has_look = true
		var parts: PackedStringArray = args["look"].split(",")
		look_yaw = -deg_to_rad(parts[0].to_float())
		look_pitch = -deg_to_rad(parts[1].to_float()) if parts.size() > 1 else 0.0


## "X,Y,Z" as a Vector3, or null if it is not three numbers.
static func _vec3(text: String) -> Variant:
	var parts := text.split(",")
	if parts.size() < 3:
		return null
	return Vector3(parts[0].to_float(), parts[1].to_float(), parts[2].to_float())


## `--key value` pairs; a key without a value is skipped.
static func _split(list: PackedStringArray) -> Dictionary:
	var out := {}
	var i := 0
	while i < list.size():
		var key := list[i]
		if key.begins_with("--") and key.substr(2) in FLAGS:
			out[key.substr(2)] = ""
			i += 1
		elif key.begins_with("--") and i + 1 < list.size():
			out[key.substr(2)] = list[i + 1]
			i += 2
		else:
			i += 1
	return out


## Viewer arguments that come back to a shot's view (--from-shot), with time
## stopped there. {} if the file is not a shot.
static func _arguments_from_shot(path: String) -> Dictionary:
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
	var time_state: Dictionary = shot.get("time", {})
	if time_state.has("hour"):
		out["time"] = str(time_state["hour"])
	out["time-scale"] = "0"
	var weather_state: Dictionary = shot.get("weather", {})
	if not str(weather_state.get("editor_id", "")).is_empty():
		out["weather"] = weather_state["editor_id"]
	var viewer: Dictionary = shot.get("viewer", {})
	if viewer.get("flying", false):
		out["walk"] = "off"
	if viewer.has("radius"):
		out["radius"] = str(viewer["radius"])
	if viewer.has("large_refs"):
		out["large-refs"] = "on" if viewer["large_refs"] else "off"
	if viewer.has("large_ref_radius"):
		out["large-ref-radius"] = str(viewer["large_ref_radius"])
	if viewer.has("lod_split"):
		out["lod-split"] = str(viewer["lod_split"])
	if viewer.has("msaa"):
		out["msaa"] = str(viewer["msaa"])
	if viewer.has("aa"):
		out["aa"] = str(viewer["aa"])
	if viewer.has("foliage_bias"):
		out["foliage-bias"] = str(viewer["foliage_bias"])
	if viewer.has("render_scale"):
		out["render-scale"] = str(viewer["render_scale"])
	if viewer.get("quests", true) == false:
		out["quests"] = "off"
	if viewer.get("materials", true) == false:
		out["materials"] = "off"
	if not str(shot.get("note", "")).is_empty():
		out["note"] = shot["note"]
	return out


## --equip-spell and --cast-at: "left" is 0, "right" 1, "power" 2 (Magic.LEFT, RIGHT, POWER).
static func _hand_of(text: String) -> int:
	match text.to_lower():
		"left":
			return 0
		"power":
			return 2
	return 1
