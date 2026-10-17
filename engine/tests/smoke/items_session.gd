# SPDX-License-Identifier: GPL-3.0-or-later
#
# The player's items through GameSession on the real SE pack (SKYDOT_SSE_PACK):
# activating Riverwood's chest, a Bleak Falls urn and a Draugr chest raises
# container_opened; everything is taken and shows in the player's inventory
# with name, weight and value; iron gear is equipped (GetEquippedItemType, an
# armor over another's slot takes it off); an item is dropped and picked up
# again; a save and a load keep the contents, the taken items, the equipment
# and the dropped item. Headless; skips without the pack.
extends SceneTree

const PLAYER := 0x14
const CHESTS := {0x0005A3B6: "Chest", 0x0003906E: "Urn", 0x0003A7C1: "Chest"}
const MERCHANT_CHEST := 0x00078C0B
const IRON_SWORD := 0x00012EB7
const IRON_ARMOR := 0x00012E49
const HIDE_ARMOR := 0x00013911

var failures := 0
var _session: GameSession


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false
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


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_items_session: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _contents(owner: int) -> Dictionary:
	var out := {}
	for row in _session.inventory(owner):
		out[int(row["form"])] = int(row["count"])
	return out


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	config.pick_locks = true
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	var player := FakePlayer.new()
	expect(_session.open(config, viewpoint, player), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	var opened: Array[int] = []
	_session.container_opened.connect(func(ref: int) -> void: opened.append(ref))

	# Activating a container asks for its menu, with what it holds.
	var held := {}
	for ref in CHESTS:
		expect(_session.activate_ref(ref), "0x%08X activates" % ref)
		held[ref] = _contents(ref)
		expect(not held[ref].is_empty(), "0x%08X holds something: %s" % [ref, held[ref]])
		expect(_session.container_name(ref) == CHESTS[ref], "0x%08X is named %s" % [ref, CHESTS[ref]])
	expect(opened == [0x0005A3B6, 0x0003906E, 0x0003A7C1], "container_opened: %s" % [opened])
	_session.activate_ref(MERCHANT_CHEST)

	# Take it all.
	var expected := {}
	for ref in CHESTS:
		for form in held[ref]:
			expected[form] = expected.get(form, 0) + held[ref][form]
		expect(_session.transfer_all(ref, PLAYER) > 0, "0x%08X is taken" % ref)
		expect(_session.inventory(ref).is_empty(), "and empty")
	_session.transfer_all(MERCHANT_CHEST, PLAYER)
	var mine := _contents(PLAYER)
	for form in expected:
		expect(mine.get(form, 0) >= expected[form], "the player has 0x%X: %s" % [form, mine.get(form)])
	var gold: Dictionary = {}
	for row in _session.inventory():
		expect(row["name"] != "" and row.has("weight") and row.has("value"), "a row has name, weight, value: %s" % [row])
		if int(row["form"]) == 0xF:
			gold = row
	expect(gold.get("name") == "Gold" and gold["count"] >= 25 + 6 and gold["value"] == 1, "the gold: %s" % [gold])

	# Equip.
	expect(_session.equip(IRON_SWORD) and _session.equip(IRON_ARMOR), "iron sword and armor equip")
	expect(_session.papyrus.equipped_item_type(PLAYER, 1) == 1, "GetEquippedItemType(1) is a sword")
	expect(_session.papyrus.equipped_item_type(PLAYER, 0) == 0, "and the left hand is empty")
	expect(_session.equipment()["worn"] == PackedInt64Array([IRON_ARMOR]), "the iron armor is worn")
	expect(_session.equip(HIDE_ARMOR), "hide armor equips")
	expect(_session.equipment()["worn"] == PackedInt64Array([HIDE_ARMOR]), "and takes the iron armor off: %s" % [_session.equipment()])
	expect(_session.equip(IRON_ARMOR) and _session.equipment()["worn"] == PackedInt64Array([IRON_ARMOR]), "and back")

	# Drop one and pick it up.
	var dagger := 0x0001397E
	_session.papyrus.add_item(PLAYER, dagger, 3)
	var before := int(_contents(PLAYER).get(dagger, 0))
	var dropped := _session.drop(dagger, 2)
	expect(dropped != 0 and int(_contents(PLAYER).get(dagger, 0)) == before - 2, "two daggers drop as one reference")
	expect(_session.papyrus.get_ref_base(dropped) == dagger, "which is a dagger")

	# Save and load: the contents, the taken items, the equipment and the dropped reference.
	var path := "user://items_session.skydot"
	expect(_session.save_game(path), "saves")
	var gold_count := int(_contents(PLAYER).get(0xF, 0))
	var worn := _session.equipment()
	_session.papyrus.add_item(PLAYER, 0xF, 1000)
	_session.unequip(IRON_ARMOR)
	expect(_session.load_game(path), "loads")
	expect(int(_contents(PLAYER).get(0xF, 0)) == gold_count, "the gold is as saved")
	expect(_session.equipment() == worn, "the equipment is as saved")
	for ref in CHESTS:
		expect(_session.inventory(ref).is_empty(), "0x%08X stays empty after the load" % ref)
	expect(_session.papyrus.get_ref_base(dropped) == dagger, "the dropped reference is still there")
	var outcome := _session.papyrus.activate_ref(dropped, PLAYER, false, SkydotPapyrus.ACTIVATE_PLAYER, false)
	expect(outcome["kind"] == "pickup", "activating it picks it up: %s" % [outcome["kind"]])
	expect(int(_contents(PLAYER).get(dagger, 0)) == before, "with its count")
	print("smoke_items_session: failures=%d" % failures)
	quit(failures)
