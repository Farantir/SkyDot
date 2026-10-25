# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shout data of world.fb 36 on the real SE pack (SKYDOT_SSE_PACK): the shout
# Unrelenting Force (0x13E07) with its words of power and spells, the Bleak
# Falls Barrow word wall naming the word Fus, DragonRace's flight data and
# movement types, and the shouts of a fire dragon. Headless; skips without the
# pack.
extends SceneTree

const UNRELENTING_FORCE := 0x13E07
const WORD_FUS := 0x13E22
const DRAGON_RACE := 0x12E82
const ENC_DRAGON_FIRE := 0xF8115 # EncDragon01FireNoScript
const WORD_WALL_TRIGGER := 0x709D2 # a placed reference of WordWallTrigger
const WORD_WALL := 0x709D1 # a placed reference

var failures := 0
var _session: GameSession


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false

	func place_at(_at: Vector3, new_yaw: float, new_pitch: float) -> void:
		yaw = new_yaw
		pitch = new_pitch

	func set_view_distance(_metres: float) -> void:
		pass

	func save_section() -> PackedByteArray:
		return PackedByteArray()

	func load_section(_bytes: PackedByteArray) -> bool:
		return true


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_shout_data: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, viewpoint, FakePlayer.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	var world := _session.world

	# Unrelenting Force: three words, each with its spell and recovery.
	var shout := world.get_shout(UNRELENTING_FORCE)
	print("shout: ", shout)
	expect(shout.get("editor_id", "") == "UnrelentingForceShout", "the shout is found: %s" % [shout.get("editor_id")])
	expect(shout.get("name", "") == "Unrelenting Force", "its name: %s" % [shout.get("name")])
	expect(shout.get("description", "") != "" and shout.get("menu_display_object", 0) != 0, "it has a description and a menu object")
	var words: Array = shout.get("words", [])
	expect(words.size() == 3, "three words: %d" % words.size())
	if words.size() == 3:
		var want_words := [0x13E22, 0x13E23, 0x13E24]
		var want_spells := [0x13E09, 0x13F39, 0x13F3A]
		var want_recovery := [15.0, 20.0, 45.0]
		for i in 3:
			expect(words[i]["word"] == want_words[i], "word %d: 0x%X" % [i, words[i]["word"]])
			expect(words[i]["spell"] == want_spells[i], "spell %d: 0x%X" % [i, words[i]["spell"]])
			expect(is_equal_approx(words[i]["recovery"], want_recovery[i]), "recovery %d: %s" % [i, words[i]["recovery"]])
			expect(not world.get_spell(words[i]["spell"]).is_empty(), "spell %d is in the file" % i)
	var fus := world.get_word_of_power(WORD_FUS)
	print("word: ", fus)
	expect(fus.get("name", "") == "Fus", "the word: %s" % [fus.get("name")])
	expect(fus.get("translation", "") == "Force", "its translation: %s" % [fus.get("translation")])
	expect(fus.get("editor_id", "") == "WordFus", "its editor id: %s" % [fus.get("editor_id")])

	# The Bleak Falls Barrow word wall teaches Fus: the trigger (a placed
	# reference, 0x709D2) names the word and the wall (0x709D1).
	var cell := world.find_cell("BleakFallsBarrow02")
	expect(cell != 0, "BleakFallsBarrow02 is found")
	var info := world.get_ref_info(cell, WORD_WALL_TRIGGER)
	expect(info.get("editor_id", "") == "WordWallTrigger", "the trigger is placed in the cell: %s" % [info.get("editor_id")])
	var found := false
	for script in info.get("scripts", []):
		var props: Dictionary = script["properties"]
		if script["name"].to_lower() == "wordwalltriggerscript" and props.has("myWord01"):
			found = true
			print("word wall properties: ", props)
			expect(props["myWord01"] == WORD_FUS, "myWord01 is WordFus: %s" % [props["myWord01"]])
			expect(props.get("wordWall01", 0) == WORD_WALL, "wordWall01 is the wall: %s" % [props.get("wordWall01")])
	expect(found, "the trigger runs WordWallTriggerScript with a word")

	# DragonRace: flight data and the default movement types.
	var race := world.get_race(DRAGON_RACE)
	print("dragon race: ", race)
	expect(is_equal_approx(race.get("aim_angle_tolerance", 0.0), 41.75), "aim angle tolerance 41.75: %s" % [race.get("aim_angle_tolerance")])
	expect(is_equal_approx(race.get("flight_radius", 0.0), 400.0), "flight radius 400: %s" % [race.get("flight_radius")])
	expect(is_equal_approx(race.get("angular_acceleration", 0.0), 0.25), "angular acceleration 0.25: %s" % [race.get("angular_acceleration")])
	expect(is_equal_approx(race.get("angular_tolerance", 0.0), 5.0), "angular tolerance 5: %s" % [race.get("angular_tolerance")])
	expect(is_equal_approx(race.get("injured_health", 0.0), 0.35), "injured health 0.35: %s" % [race.get("injured_health")])
	var fly := world.get_movement_type(race.get("movement_fly", 0))
	print("flying movement: ", fly)
	expect(fly.get("editor_id", "") == "Dragon_Flying_MT", "flying movement type: %s" % [fly.get("editor_id")])
	expect(is_equal_approx(fly.get("forward_walk", 0.0), 2000.0) and is_equal_approx(fly.get("forward_run", 0.0), 7400.0), "its forward speeds: %s %s" % [fly.get("forward_walk"), fly.get("forward_run")])
	var walk := world.get_movement_type(race.get("movement_walk", 0))
	print("walking movement: ", walk)
	expect(walk.get("editor_id", "") == "Dragon_Default_MT", "walking movement type: %s" % [walk.get("editor_id")])
	expect(is_equal_approx(walk.get("forward_walk", 0.0), 384.0), "its forward walk 384: %s" % [walk.get("forward_walk")])

	# A fire dragon breathes and throws fireballs.
	var dragon := world.get_npc(ENC_DRAGON_FIRE)
	print("dragon npc: ", dragon)
	var shouts: PackedInt64Array = dragon.get("shouts", PackedInt64Array())
	expect(shouts.has(0x4DBA5) and shouts.has(0x252C0), "EncDragon01Fire has the fireball and breath shouts: %s" % [shouts])
	expect(world.get_shout(0x7FFFFFF).is_empty() and world.get_word_of_power(0x7FFFFFF).is_empty() and world.get_movement_type(0x7FFFFFF).is_empty(), "unknown forms are empty")
	print("smoke_shout_data: ", "FAIL" if failures > 0 else "OK")
	quit(1 if failures > 0 else 0)
