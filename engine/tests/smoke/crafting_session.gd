# SPDX-License-Identifier: GPL-3.0-or-later
#
# Harvesting and crafting through SkydotPapyrus on the real SE pack
# (SKYDOT_SSE_PACK), Riverwood's references: a plant gives its ingredient once
# and respawns after ten game days (also across a save); Alvor's forge crafts
# an Iron Dagger, the grindstone tempers it (Fine, kept by a save), the tanning
# rack turns Leather into Leather Strips; the alchemy lab lists ingredients,
# eating one teaches its first effect. Headless; skips without the pack.
extends SceneTree

const PLAYER := 0x14
const FORGE := 0xD6987
const GRINDSTONE := 0x1C785
const TANNING_RACK := 0x105390
const ALCHEMY_LAB := 0x26D7B
const FLY_AMANITA_PLANT := 0xC0533
const FLY_AMANITA := 0x4DA00
const GARLIC := 0x34D22
const INGOT := 0x5ACE4
const STRIPS := 0x800E4
const LEATHER := 0xDB5D2
const DAGGER := 0x1397E
const R_DAGGER := 0xDA76A
const R_STRIPS := 0xF7F43
const R_TEMPER_DAGGER := 0xADB7E

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
		print("smoke_crafting_session: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _count(form: int, extra := 0) -> int:
	for row in _session.inventory(PLAYER):
		if int(row["form"]) == form and int(row["extra"]) == extra:
			return int(row["count"])
	return 0


func _activate(ref: int) -> Dictionary:
	return _session.papyrus.activate_ref(ref, PLAYER, false, SkydotPapyrus.ACTIVATE_PLAYER, false)


## The Fly Amanita is a NiSwitchNode model: child 0 whole, child 1 harvested.
## Built on its own it shows the harvested state the VM keeps (as a cell does
## when it is built), and a harvest shows it at once.
func _built_plant() -> Node3D:
	var cell := _session.world.get_ref_cell(FLY_AMANITA_PLANT)
	var holder := _session.world.build_ref(cell, FLY_AMANITA_PLANT)
	expect(holder != null, "the plant is built")
	if holder == null:
		return null
	root.add_child(holder)
	_session.show_harvests(holder)
	return holder


func _check_harvested_model(papyrus: SkydotPapyrus) -> void:
	# Loaded state: the save in _run left it harvested.
	var holder := _built_plant()
	if holder == null:
		return
	expect(SkydotSwitch.get_index(holder) == 1, "a built harvested plant shows child 1: %d" % SkydotSwitch.get_index(holder))
	holder.free()

	# Respawn: ten days later a rebuilt plant is whole.
	var before := _session.clock.get_game_days()
	_session.clock.set_game_days(before + 11.0)
	holder = _built_plant()
	expect(not papyrus.harvested(FLY_AMANITA_PLANT), "it has respawned")
	expect(SkydotSwitch.get_index(holder) == 0, "a respawned plant shows child 0: %d" % SkydotSwitch.get_index(holder))

	# Harvesting it again shows child 1 at once, through the bridge.
	expect(_session.activate_ref(FLY_AMANITA_PLANT), "the plant is activated")
	expect(papyrus.harvested(FLY_AMANITA_PLANT), "and harvested")
	expect(SkydotSwitch.get_index(holder) == 1, "the live node shows child 1 at once: %d" % SkydotSwitch.get_index(holder))
	# The shapes' names: FloraMushroom01_N whole, FloraMushroom01_1_N harvested.
	var cut := 0
	var whole := 0
	for mesh in holder.find_children("*", "MeshInstance3D", true, false):
		if (mesh as MeshInstance3D).is_visible_in_tree():
			if str(mesh.name).begins_with("FloraMushroom01_1_"):
				cut += 1
			elif str(mesh.name).begins_with("FloraMushroom01_"):
				whole += 1
	expect(whole == 0 and cut == 3, "only the harvested shapes are drawn (%d whole, %d cut)" % [whole, cut])
	holder.free()


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
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"a cell is entered (a load needs a place)")
	var papyrus := _session.papyrus

	# A plant: once, then "already harvested", and the prompt loses its verb.
	var plant := _activate(FLY_AMANITA_PLANT)
	print("plant: ", plant)
	expect(plant["kind"] == "harvest", "the plant is harvested: %s" % [plant.get("kind")])
	expect(int(plant.get("harvest_form", 0)) == FLY_AMANITA, "it gives Fly Amanita")
	expect(_count(FLY_AMANITA) == int(plant.get("harvest_count", -1)), "which the player holds")
	var held := _count(FLY_AMANITA)
	expect(papyrus.harvested(FLY_AMANITA_PLANT), "the plant is harvested")
	var again := _activate(FLY_AMANITA_PLANT)
	expect(again["kind"] == "none" and again["note"] == "already harvested", "a second use: %s" % [again])
	expect(_count(FLY_AMANITA) == held, "gives nothing")
	var path := "user://crafting_session.skydot"
	expect(_session.save_game(path), "saves")
	_session.clock.set_game_days(_session.clock.get_game_days() + 11.0)
	expect(not papyrus.harvested(FLY_AMANITA_PLANT), "ten days later it can be harvested again")
	expect(_session.load_game(path), "loads")
	expect(papyrus.harvested(FLY_AMANITA_PLANT), "the save keeps it harvested")
	expect(_count(FLY_AMANITA) == held, "and the ingredient")

	_check_harvested_model(papyrus)

	# The forge: Iron Dagger from an Iron Ingot and Leather Strips.
	var forge := _activate(FORGE)
	expect(forge["kind"] == "crafting" and forge["bench_type"] == 1, "the forge is a bench of type 1: %s" % [forge])
	papyrus.add_item(PLAYER, INGOT, 3)
	papyrus.add_item(PLAYER, STRIPS, 1)
	var recipe := {}
	for row in papyrus.recipes(FORGE):
		if int(row["recipe"]) == R_DAGGER:
			recipe = row
	expect(not recipe.is_empty() and recipe["craftable"] and int(recipe["product"]) == DAGGER,
		"the Iron Dagger is craftable: %s" % [recipe])
	expect(papyrus.craft(R_DAGGER, 1) == 1 and _count(DAGGER) == 1, "one is made")

	# The grindstone tempers it.
	var stone := _activate(GRINDSTONE)
	expect(stone["kind"] == "crafting" and stone["bench_type"] == 2, "the grindstone is type 2: %s" % [stone])
	var row := {}
	for r in papyrus.temper_rows(GRINDSTONE):
		if int(r["recipe"]) == R_TEMPER_DAGGER and int(r["extra"]) == 0:
			row = r
	expect(not row.is_empty() and row["craftable"] and row["next_tier"] == "Fine", "tempering is offered: %s" % [row])
	expect(papyrus.temper(R_TEMPER_DAGGER, DAGGER, 0), "it is tempered")
	var fine := {}
	for r in _session.inventory(PLAYER):
		if int(r["form"]) == DAGGER:
			fine = r
	expect(fine.get("name") == "Iron Dagger (Fine)" and int(fine["extra"]) != 0, "the row is Fine: %s" % [fine])
	expect(_session.equip(DAGGER, int(fine["extra"])), "and can be equipped")
	expect(_session.save_game(path) and _session.load_game(path), "saves and loads")
	var back := false
	for r in _session.inventory(PLAYER):
		back = back or (int(r["form"]) == DAGGER and r["name"] == "Iron Dagger (Fine)" and r["equipped"])
	expect(back, "Fine and equipped after the load")

	# The tanning rack: Leather to Leather Strips.
	var rack := _activate(TANNING_RACK)
	expect(rack["kind"] == "crafting" and rack["bench_type"] == 1, "the tanning rack is type 1")
	papyrus.add_item(PLAYER, LEATHER, 1)
	var before := _count(STRIPS)
	var made := papyrus.craft(R_STRIPS, 1)
	expect(made == 1 and _count(STRIPS) == before + 4, "Leather becomes four Leather Strips: made %d, strips %d -> %d, rows %s" % [
		made, before, _count(STRIPS), papyrus.recipes(TANNING_RACK).filter(func(r): return int(r["recipe"]) == R_STRIPS)])

	# The alchemy lab: ingredients and what eating teaches.
	var lab := _activate(ALCHEMY_LAB)
	expect(lab["kind"] == "crafting" and lab["bench_type"] == 5, "the lab is type 5: %s" % [lab])
	papyrus.add_item(PLAYER, GARLIC, 1)
	var listed := {}
	for r in papyrus.alchemy_rows():
		listed[int(r["form"])] = r
	expect(listed.has(FLY_AMANITA) and listed.has(GARLIC), "both ingredients are listed")
	expect(not listed[FLY_AMANITA]["effects"][0]["known"], "no effect is known yet")
	var preview := papyrus.alchemy_preview(PackedInt64Array([FLY_AMANITA, GARLIC]))
	expect(preview.has("mixable") and preview.has("effects"), "the preview has its fields: %s" % [preview])
	expect(papyrus.alchemy_mix(PackedInt64Array([FLY_AMANITA])) == 0, "one ingredient does not mix")
	expect(_session.equip(FLY_AMANITA), "Fly Amanita is eaten")
	for r in papyrus.alchemy_rows():
		if int(r["form"]) == FLY_AMANITA:
			expect(r["effects"][0]["known"] and r["effects"][0]["name"] != "?", "its first effect is known: %s" % [r])
	print("smoke_crafting_session: failures=%d" % failures)
	quit(failures)
