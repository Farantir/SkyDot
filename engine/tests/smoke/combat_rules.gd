# SPDX-License-Identifier: GPL-3.0-or-later
#
# Combat rules through GameSession on the real SE pack (SKYDOT_SSE_PACK):
# apply_hit with the Iron Sword on the bandit 0x39FE4 of BleakFallsBarrow01
# until it dies (every hit's damage against the formula worked out here from
# the data), actor_died, the killer, searching the corpse, save and load, and
# Arvel's Golden Claw (quest MS13 creates it in his inventory). Headless;
# skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const ARVEL := 0x39655
const MS13 := 0x39645
const GOLDEN_CLAW := 0x39647
const IRON_SWORD := 0x12EB7

var failures := 0
var _session: GameSession
var _player: FakePlayer


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false
	var sprinting := false
	var allowed_calls: Array[bool] = []
	var loaded_position := Vector3.ZERO
	var loaded_yaw := 0.0
	var loaded_pitch := 0.0

	func place_at(_at: Vector3, new_yaw: float, new_pitch: float) -> void:
		yaw = new_yaw
		pitch = new_pitch

	func set_view_distance(_metres: float) -> void:
		pass

	func save_section() -> PackedByteArray:
		return PackedByteArray()

	func load_section(_bytes: PackedByteArray) -> bool:
		return true

	func is_sprinting() -> bool:
		return sprinting

	func set_sprint_allowed(allowed: bool) -> void:
		allowed_calls.append(allowed)


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_combat_rules: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _near(a: float, b: float, tolerance := 0.01) -> bool:
	return absf(a - b) < tolerance


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	_player = FakePlayer.new()
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, viewpoint, _player), "the session opens")
	if _session.world == null:
		quit(1)
		return
	var deaths: Array[int] = []
	_session.actor_died.connect(func(ref: int) -> void: deaths.append(ref))
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")

	var papyrus := _session.papyrus

	# The Iron Sword in hand.
	_session.give([Vector2i(IRON_SWORD, 1)])
	expect(_session.equip(IRON_SWORD), "the Iron Sword is equipped")

	# The numbers by hand: sword 7, lerp(1, fDamagePCSkillMax, skill / 100),
	# the race's attackStart multiplier (Nord 1.0), the bandit's armor.
	var skill := papyrus.get_actor_value(PLAYER, "OneHanded")
	print("player OneHanded: ", skill)
	var armor := _bandit_armor_percent()
	var expected := 7.0 * (1.0 + 0.5 * skill / 100.0) * (1.0 - armor / 100.0)
	print("expected per hit: ", expected, " (armor ", armor, "%)")
	var health := papyrus.get_actor_value(BANDIT, "Health")
	expect(health > 0.0 and not papyrus.is_actor_dead(BANDIT), "the bandit lives with %f health" % health)
	var start_inventory := _session.inventory(BANDIT)
	expect(not start_inventory.is_empty(), "the bandit carries his outfit and items: %d stacks" % start_inventory.size())

	var hits := 0
	var last := {}
	while not papyrus.is_actor_dead(BANDIT) and hits < 100:
		last = papyrus.apply_hit(PLAYER, BANDIT, IRON_SWORD, 0, "attackStart", 0)
		hits += 1
		if hits == 1:
			print("first hit: ", last)
			expect(_near(last["damage"], expected, 0.001), "the first hit does %f, expected %f" % [last["damage"], expected])
			expect(_near(last["base"], 7.0) and _near(last["attack"], 1.0), "base 7, attack 1")
			expect(_near(health - last["health_after"], expected, 0.01) or last["killed"], "Health drops by the damage")
	expect(hits > 1 and papyrus.is_actor_dead(BANDIT), "the bandit dies after %d hits" % hits)
	expect(last.get("killed", false), "the last hit says killed")
	expect(deaths == [BANDIT], "actor_died fired once: %s" % [deaths])
	expect(papyrus.get_killer(BANDIT) == PLAYER, "the killer is the player")
	var power := papyrus.apply_hit(PLAYER, ARVEL, IRON_SWORD, 0, "attackPowerStartInPlace", SkydotPapyrus.HIT_POWER)
	expect(not power.has("error") and _near(power["attack"], 2.0), "a standing power attack has multiplier 2: %s" % [power])
	expect(papyrus.apply_hit(PLAYER, BANDIT, IRON_SWORD, 0, "attackStart", 0).has("error"), "a dead target takes no hit")

	# The corpse: prompt, activation, loot.
	var cell: int = _session.world.get_actor(BANDIT)["cell"]
	var prompt := ActivationPrompt.new(_session.world, papyrus, _session.host, viewpoint).describe(cell, BANDIT)
	print("corpse prompt: ", prompt)
	expect(prompt.get("verb", "") == "Search", "the corpse's prompt is Search")
	var outcome := papyrus.activate_ref(BANDIT, PLAYER, false, SkydotPapyrus.ACTIVATE_PLAYER, false)
	expect(outcome["kind"] == "container", "activating a corpse opens its inventory: %s" % [outcome["kind"]])
	expect(not papyrus.dialogue_can_talk(BANDIT), "and it does not talk")
	var stacks := _session.transfer_all(BANDIT, PLAYER)
	expect(stacks == start_inventory.size() and _session.inventory(BANDIT).is_empty(), "everything is taken: %d stacks" % stacks)
	papyrus.set_corpse_place(BANDIT, cell, Vector3(100, 200, 300), 1.5)
	var place := papyrus.get_corpse_place(BANDIT)
	expect(place.get("space", 0) == cell and _near(place["rot_z"], 1.5), "the corpse place is kept")
	expect(papyrus.get_corpse_place(ARVEL).is_empty(), "another actor has none")

	# Save and load: still dead, looted, killer kept, in place.
	var path := "user://combat_rules.skydot"
	expect(_session.save_game(path), "saves")
	_session.papyrus.add_item(BANDIT, IRON_SWORD, 1)
	expect(_session.load_game(path), "loads")
	expect(papyrus.is_actor_dead(BANDIT), "still dead")
	expect(_session.inventory(BANDIT).is_empty(), "still looted")
	expect(papyrus.get_killer(BANDIT) == PLAYER, "the killer is kept")
	var again := papyrus.get_corpse_place(BANDIT)
	expect(again.get("space", 0) == cell and again["position"].is_equal_approx(Vector3(100, 200, 300)), "in place: %s" % [again])
	expect(papyrus.get_actor_value(BANDIT, "Health") <= 0.0, "with no health")

	# Arvel and the Golden Claw: quest MS13 makes it in his inventory.
	expect(papyrus.start_quest(MS13), "MS13 starts")
	var has_claw := false
	for row: Dictionary in _session.inventory(ARVEL):
		has_claw = has_claw or int(row["form"]) == GOLDEN_CLAW
	expect(has_claw, "Arvel carries the Golden Claw")
	var arvel_hits := 0
	while not papyrus.is_actor_dead(ARVEL) and arvel_hits < 100:
		papyrus.apply_hit(PLAYER, ARVEL, IRON_SWORD, 0, "attackStart", 0)
		arvel_hits += 1
	expect(papyrus.is_actor_dead(ARVEL) and papyrus.get_killer(ARVEL) == PLAYER, "Arvel dies to the player after %d hits" % arvel_hits)
	_session.transfer_all(ARVEL, PLAYER)
	expect(papyrus.get_item_count(PLAYER, GOLDEN_CLAW) == 1, "the claw is in the player's inventory")
	print("smoke_combat_rules: failures=%d" % failures)
	quit(failures)


## The armor reduction the bandit's worn pieces give, by the data (UESP
## Skyrim:Armor): rating x lerp(fArmorRatingBase, fArmorRatingMax, skill / 100)
## rounded up, x fArmorScalingFactor, plus 3 for every piece.
func _bandit_armor_percent() -> float:
	var papyrus := _session.papyrus
	var rating := 0.0
	var pieces := 0
	for form: int in papyrus.equipment(BANDIT)["worn"]:
		var info := papyrus.item_info(form)
		if info.get("armor_type", 2) == 2:
			rating += float(info.get("armor_rating", 0.0))
			continue
		var skill := papyrus.get_actor_value(BANDIT, "HeavyArmor" if info["armor_type"] == 1 else "LightArmor")
		rating += ceilf(float(info["armor_rating"]) * (1.0 + 1.5 * skill / 100.0))
		pieces += 1
	return minf(80.0, rating * 0.12 + 3.0 * pieces)
