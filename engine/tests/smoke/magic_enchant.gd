# SPDX-License-Identifier: GPL-3.0-or-later
#
# Enchanted weapons and poisons through Combat on the real SE pack (SKYDOT_SSE_PACK), headless,
# 7a-3 and 7a-6: the Dremora Fire Sword 0x17009 is drawn
# and hits the bandit 0x39FE4 of BleakFallsBarrow01 as in sse_combat_melee; its enchantment's
# effects land on him, the charge drops by the cost at each hit (the weapon moves to a new
# ItemExtra every time, and the next hit names the new one; running out of charge is covered by the
# C++ tests), and a poison put on the weapon with
# use_item is delivered by the next hit and gone after it.
#
# The enchanting table (7a-6): an Iron Dagger of Burning 0x896B4 is disenchanted (the dagger is gone,
# its enchantment known, a second one refused as known) and Mehrunes' Razor 0xDA07's (MagicDisallowEnchanting)
# is refused; an Iron Sword 0x12EB7 is enchanted with it using Azura's Star 0x63B27 holding a soul
# (kept, emptied; the petty soul limits the charge and the cost of a use) and, through the CraftingView,
# with a filled grand soul gem 0x2E4FF (gone; "Iron Sword of ...", charge 3000). The sword is drawn and hits
# the bandit: the created enchantment's effect lands and the charge drops by its cost; the inventory's
# recharge takes a filled petty gem 0x2E4E3 (charge up, capped at the maximum). A save and a load keep
# the enchantment and the charge. Soul Trap 0x4DBA4 on the skeever, which then dies, fills an empty gem.
# Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const FIRE_SWORD := 0x17009
const POISON := 0x3A5A4  # Weak Poison: Damage Health
const IRON_SWORD := 0x12EB7
const FIRE_DAGGER := 0x896B4  # Iron Dagger of Burning
const RAZOR := 0x240D2  # Mehrunes' Razor: MagicDisallowEnchanting
const GRAND_FULL := 0x2E4FF
const PETTY := 0x2E4E2
const PETTY_FULL := 0x2E4E3
const AZURA := 0x63B27
const SOUL_TRAP := 0x4DBA4
const SKEEVER := 0x3B9F7
const SAVE := "user://magic_enchant.skydot"
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _hits: Array[Dictionary] = []
var _traps: Array[Dictionary] = []


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
		print("smoke_magic_enchant: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _frames(count: int) -> void:
	for i in count:
		_session.step(STEP, _feet)
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


## The player stands `distance` metres from `node`, where he has a clear line to it, and looks at it.
func _stand_before(node: Node3D, distance: float) -> void:
	var space := _camera.get_world_3d().direct_space_state
	var chest := node.global_position + Vector3.UP * 1.0
	var forward := -node.global_transform.basis.z
	forward.y = 0.0
	forward = forward.normalized()
	for step in 12:
		var feet: Vector3 = node.global_position + forward.rotated(Vector3.UP, step * TAU / 12.0) * distance
		var eye := feet + Vector3.UP * GameSession.EYE_HEIGHT
		var free := space.intersect_ray(PhysicsRayQueryParameters3D.create(eye, chest, 1 | 2 | 8)).is_empty()
		var ground := not space.intersect_ray(PhysicsRayQueryParameters3D.create(feet + Vector3.UP * 0.5, feet - Vector3.UP * 0.5, 1 | 2 | 8)).is_empty()
		if free and ground:
			_feet = feet
			_camera.global_position = eye
			_camera.look_at(chest, Vector3.UP)
			return
	expect(false, "no clear place to stand before the target")


## The built actor with the most Health (the enchantment's effects must not kill it in one hit).
func _toughest() -> int:
	var best := 0
	var best_health := 0.0
	for node in _session.host.find_children("*", "SkydotActor", true, false):
		if not node.has_meta("skydot_ref"):
			continue
		var ref: int = node.get_meta("skydot_ref")
		var health := _session.papyrus.get_actor_value_max(ref, "Health")
		if health > best_health and not _session.papyrus.is_actor_dead(ref) and ref != PLAYER:
			best = ref
			best_health = health
	return best


## One light attack through the session; the hit it lands (the last of `_hits`), after the clip is over.
func _swing() -> Dictionary:
	var before := _hits.size()
	expect(_session.attack(false), "the attack starts")
	await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 4.0)
	expect(_hits.size() == before + 1, "the swing hit: %d -> %d" % [before, _hits.size()])
	return _hits[-1] if _hits.size() > before else {}


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	config.wander = false
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, _camera, FakePlayer.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	var papyrus := _session.papyrus
	_session.hit_landed.connect(func(target: int, result: Dictionary, _point: Vector3) -> void:
		_hits.append({"target": target, "result": result}))
	var target := _toughest()
	var bandit := _session.world.find_ref_node(target) as Node3D
	print("target 0x%X with %f Health" % [target, papyrus.get_actor_value_max(target, "Health")])
	expect(bandit != null and papyrus.get_actor_value_max(target, "Health") > 80.0, "a tough actor is built")
	if bandit == null:
		quit(1)
		return
	await _frames(30)

	# ---- the enchanted sword
	_session.give([Vector2i(FIRE_SWORD, 1)])
	expect(_session.equip(FIRE_SWORD) and _session.player_weapon() == FIRE_SWORD, "the Dremora Fire Sword is equipped")
	var charge := papyrus.weapon_charge(PLAYER, FIRE_SWORD)
	print("charge: ", charge)
	expect(charge["enchantment"] != 0 and charge["max"] > 0.0 and charge["cost"] > 0.0, "it is enchanted, with a charge and a cost per hit")
	expect(absf(charge["charge"] - charge["max"]) < 1e-3, "and starts full: %s" % [charge])
	_stand_before(bandit, 1.2)
	_session.toggle_weapon()
	await _until(func() -> bool: return _session.weapon_state() == Combat.State.DRAWN, 5.0)
	papyrus.restore_actor_value(target, "Health", 1000.0)
	var extra0: int = papyrus.equipment(PLAYER)["right_extra"]
	var hit := await _swing()
	var result: Dictionary = hit.get("result", {})
	print("first hit: ", result)
	expect(result.get("enchantment", 0) == charge["enchantment"], "the hit names the enchantment: %s" % [result])
	expect(absf(result.get("charge", 0.0) - (charge["charge"] - charge["cost"])) < 1e-2, "and its charge after the hit")
	var charge1 := papyrus.weapon_charge(PLAYER, FIRE_SWORD)
	expect(absf(charge1["charge"] - (charge["charge"] - charge["cost"])) < 1e-2, "the charge drops by the cost: %f -> %f (cost %f)" % [charge["charge"], charge1["charge"], charge["cost"]])
	var extra1: int = papyrus.equipment(PLAYER)["right_extra"]
	expect(extra1 != extra0, "the weapon moved to a new ItemExtra: %d -> %d" % [extra0, extra1])
	var effects := papyrus.active_effects(target)
	print("bandit effects: ", effects)
	var ench_effects: Array = _session.world.get_enchantment(charge["enchantment"])["effects"]
	print("enchantment effects: ", ench_effects, " on ", effects.map(func(e: Dictionary) -> String: return e["name"]))
	expect(ench_effects.size() == 1 and absf(ench_effects[0]["magnitude"] - 30.0) < 1e-3, "the sword's enchantment is one effect of 30")
	expect(papyrus.get_actor_value(target, "Health") < result["health_after"] - 15.0,
		"its fire damage lands on the target: %f after the blow's %f" % [papyrus.get_actor_value(target, "Health"), result["health_after"]])
	papyrus.dispel_all_spells(target)
	await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)
	papyrus.restore_actor_value(target, "Health", 1000.0)
	hit = await _swing()
	result = hit.get("result", {})
	var charge2 := papyrus.weapon_charge(PLAYER, FIRE_SWORD)
	expect(result.get("enchantment", 0) == charge["enchantment"], "a second hit fires again (the new extra was followed): %s" % [result])
	expect(absf(charge2["charge"] - (charge1["charge"] - charge["cost"])) < 1e-2, "and spends again: %f -> %f" % [charge1["charge"], charge2["charge"]])
	expect(papyrus.equipment(PLAYER)["right_extra"] == extra1, "the sole holder keeps its ItemExtra id (changed in place)")
	papyrus.dispel_all_spells(target)

	# ---- a poison
	await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)
	_session.give([Vector2i(POISON, 1)])
	expect(_session.use_item(POISON), "the poison is used")
	expect(papyrus.get_item_count(PLAYER, POISON) == 0, "one is used up")
	papyrus.restore_actor_value(target, "Health", 1000.0)
	hit = await _swing()
	result = hit.get("result", {})
	print("poisoned hit: ", result)
	expect(result.get("poison", 0) == POISON, "the next hit delivers the poison: %s" % [result])
	expect(papyrus.get_actor_value(target, "Health") < result["health_after"] - 0.5, "its Damage Health lands on the target: %f after %f" % [papyrus.get_actor_value(target, "Health"), result["health_after"]])
	papyrus.dispel_all_spells(target)
	await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)
	hit = await _swing()
	result = hit.get("result", {})
	expect(result.get("poison", 0) == 0, "and the one after carries none: %s" % [result])

	await _enchanting(papyrus, target)

	print("smoke_magic_enchant: ", "FAIL" if failures > 0 else "OK")
	quit(1 if failures > 0 else 0)


## The row of `rows` for (form, extra) or the entry of `key` equal to `value`; {} if none.
func _row(rows: Array, form: int) -> Dictionary:
	for row: Dictionary in rows:
		if int(row["form"]) == form:
			return row
	return {}


func _enchanting(papyrus: SkydotPapyrus, target: int) -> void:
	papyrus.soul_trapped.connect(func(actor: int, filled: bool, gem: int, soul: int) -> void:
		_traps.append({"actor": actor, "filled": filled, "gem": gem, "soul": soul}))
	await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)

	# ---- Azura's Star takes the skeever's soul (in an extra: it has no filled form)
	_session.give([Vector2i(AZURA, 1)])
	var fill := papyrus.fill_gem(SKEEVER)
	print("fill_gem: ", fill)
	expect(fill["filled"] and fill["gem"] == AZURA and fill["soul"] >= 1 and fill["extra"] != 0, "the skeever's soul goes into Azura's Star: %s" % [fill])
	var azura_soul: int = fill["soul"]
	expect(int(_row(_session.inventory(), AZURA).get("soul", 0)) == azura_soul, "and the inventory row shows it")

	# ---- disenchanting
	_session.give([Vector2i(FIRE_DAGGER, 2), Vector2i(RAZOR, 1), Vector2i(IRON_SWORD, 3)])
	var rows := _session.enchant_rows()
	var dagger_row := _row(rows["disenchant"], FIRE_DAGGER)
	var razor_row := _row(rows["disenchant"], RAZOR)
	print("disenchant rows: ", dagger_row, razor_row)
	expect(not dagger_row.is_empty() and dagger_row["ok"], "the dagger can be disenchanted: %s" % [dagger_row])
	expect(not razor_row.is_empty() and not razor_row["ok"] and razor_row["refusal"] != "", "the Razor is refused: %s" % [razor_row])
	expect(rows["enchantments"].is_empty(), "nothing is learned yet")
	expect(not _session.disenchant(RAZOR, 0)["ok"] and papyrus.get_item_count(PLAYER, RAZOR) == 1, "disenchanting the Razor is refused and keeps it")
	var done := _session.disenchant(FIRE_DAGGER, 0)
	expect(done["ok"] and done["learned"] != "", "the dagger is disenchanted: %s" % [done])
	expect(papyrus.get_item_count(PLAYER, FIRE_DAGGER) == 1, "one dagger is gone")
	rows = _session.enchant_rows()
	var learned: Array = rows["enchantments"].filter(func(e: Dictionary) -> bool: return e["kind"] == "weapon")
	expect(learned.size() == 1 and not learned[0]["effects"].is_empty(), "its enchantment is known, with its effects: %s" % [learned])
	var again := _session.disenchant(FIRE_DAGGER, 0)
	expect(not again["ok"] and papyrus.get_item_count(PLAYER, FIRE_DAGGER) == 1, "the same enchantment is not learned twice: %s" % [again])
	var ench: int = learned[0]["id"]

	# ---- enchanting with Azura's Star
	var azura_extra: int = _row(_session.inventory(), AZURA)["extra"]
	var preview := _session.enchant_preview(IRON_SWORD, 0, ench, AZURA, azura_extra)
	print("preview: ", preview)
	expect(preview["ok"] and preview["name"].begins_with("Iron Sword of "), "the preview names the sword: %s" % [preview])
	expect(preview["charge"] <= 250.0 * azura_soul * 2.0 and preview["cost"] <= preview["charge"], "one use fits the soul's charge")
	var made := _session.enchant(IRON_SWORD, 0, ench, AZURA, azura_extra)
	expect(made["ok"] and made["name"] == preview["name"], "the sword is enchanted: %s" % [made])
	expect(papyrus.get_item_count(PLAYER, AZURA) == 1 and int(_row(_session.inventory(), AZURA).get("soul", 0)) == 0, "Azura's Star is kept, emptied")
	expect(papyrus.get_item_count(PLAYER, IRON_SWORD) == 3, "the swords are three still")

	# ---- enchanting through the menu, with a grand gem
	_session.give([Vector2i(GRAND_FULL, 1)])
	var view := CraftingView.new(_session)
	view.size = Vector2(1280, 720)
	root.add_child(view)
	view.open(0, 3)
	view.select_category("Item")
	var plain_row := -1
	for i in view._rows.size():
		if int(view._rows[i]["form"]) == IRON_SWORD and int(view._rows[i]["extra"]) == 0:
			plain_row = i
	expect(plain_row >= 0, "the plain sword is listed to enchant")
	view._selected = maxi(plain_row, 0)
	view.accept()
	view.select_category("Enchantment")
	view.accept()
	view.select_category("Soul gem")
	for i in view._rows.size():
		if int(view._rows[i]["form"]) == GRAND_FULL:
			view._selected = i
	view.accept()
	print("menu preview: ", view._preview)
	expect(view._preview.get("ok", false) and view._preview["charge"] == 3000.0, "the menu previews a charge of 3000: %s" % [view._preview])
	view.enchant_marked()
	view.close()
	view.queue_free()
	expect(papyrus.get_item_count(PLAYER, GRAND_FULL) == 0, "the grand gem is used up")
	var swords := _session.inventory().filter(func(r: Dictionary) -> bool: return int(r["form"]) == IRON_SWORD and r["charge_max"] == 3000.0 if r.has("charge_max") else false)
	expect(swords.size() == 1 and swords[0]["name"].begins_with("Iron Sword of "), "the inventory lists the new sword with its charge: %s" % [swords])
	if swords.is_empty():
		return
	var extra: int = swords[0]["extra"]
	expect(_session.equip(IRON_SWORD, extra) and _session.player_weapon() == IRON_SWORD, "the enchanted sword is equipped")
	var charge := papyrus.weapon_charge(PLAYER, IRON_SWORD, extra)
	print("charge: ", charge)
	expect(charge["max"] == 3000.0 and charge["charge"] == 3000.0 and charge["cost"] > 0.0 and charge["enchantment"] >= 0x1000000, "full, a created enchantment with a cost")

	# ---- a hit with it
	papyrus.dispel_all_spells(target)
	papyrus.restore_actor_value(target, "Health", 1000.0)
	if _session.weapon_state() != Combat.State.DRAWN:
		_session.toggle_weapon()
		await _until(func() -> bool: return _session.weapon_state() == Combat.State.DRAWN, 5.0)
	var hit := await _swing()
	var result: Dictionary = hit.get("result", {})
	print("created hit: ", result)
	expect(result.get("enchantment", 0) == charge["enchantment"], "the hit names the created enchantment: %s" % [result])
	expect(papyrus.get_actor_value(target, "Health") < result["health_after"] - 0.5, "its effect lands on the target: %f after the blow's %f" % [papyrus.get_actor_value(target, "Health"), result["health_after"]])
	var after := papyrus.weapon_charge(PLAYER, IRON_SWORD)
	expect(absf(after["charge"] - (3000.0 - charge["cost"])) < 1e-2, "the charge drops by the cost: %f" % after["charge"])

	# ---- recharge from the inventory
	_session.give([Vector2i(PETTY_FULL, 1)])
	var inventory := InventoryView.new(_session)
	inventory.size = Vector2(1280, 720)
	root.add_child(inventory)
	inventory.open()
	inventory._category = 1
	inventory._refresh()
	for i in inventory._rows.size():
		if int(inventory._rows[i]["form"]) == IRON_SWORD and int(inventory._rows[i]["extra"]) == papyrus.equipment(PLAYER)["right_extra"]:
			inventory._selected = i
	expect(InventoryView.rechargeable(inventory.current()), "the sword can take a charge: %s" % [inventory.current()])
	inventory.recharge_current()
	inventory.close()
	inventory.queue_free()
	var recharged := papyrus.weapon_charge(PLAYER, IRON_SWORD)
	expect(absf(recharged["charge"] - minf(3000.0, after["charge"] + 250.0)) < 1e-2, "petty gem: charge %f -> %f" % [after["charge"], recharged["charge"]])
	expect(papyrus.get_item_count(PLAYER, PETTY_FULL) == 0, "and the gem is used up")

	# ---- a save and a load
	expect(_session.save_game(SAVE), "the game is saved")
	papyrus.restore_actor_value(target, "Health", 1000.0)
	hit = await _swing()
	expect(_session.load_game(SAVE), "and loaded")
	var loaded := papyrus.weapon_charge(PLAYER, IRON_SWORD)
	expect(loaded["enchantment"] == charge["enchantment"] and absf(loaded["charge"] - recharged["charge"]) < 1e-2,
		"the enchantment and its charge are kept: %s" % [loaded])
	var kept := _session.inventory().filter(func(r: Dictionary) -> bool: return int(r["form"]) == IRON_SWORD and r.get("charge_max", 0.0) == 3000.0)
	expect(kept.size() == 1 and kept[0]["name"].begins_with("Iron Sword of "), "and the name: %s" % [kept])
	papyrus.dispel_all_spells(target)
	papyrus.restore_actor_value(target, "Health", 1000.0)
	await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)
	hit = await _swing()
	expect(hit.get("result", {}).get("enchantment", 0) == charge["enchantment"]
		and papyrus.get_actor_value(target, "Health") < hit["result"]["health_after"] - 0.5,
		"it still fires its effects after the load: %s" % [hit])

	# ---- Soul Trap
	_session.give([Vector2i(PETTY, 1)])
	_traps.clear()
	var cast := _session.cast_spell(SOUL_TRAP, SKEEVER)
	print("soul trap cast: ", cast)
	expect(cast["ok"], "Soul Trap is cast on the skeever: %s" % [cast])
	print("skeever effects: ", papyrus.active_effects(SKEEVER))
	_session.kill_actor(SKEEVER)
	await _frames(5)
	expect(papyrus.is_actor_dead(SKEEVER), "the skeever is dead")
	print("soul trapped: ", _traps)
	expect(_traps.size() == 1 and _traps[0]["filled"] and _traps[0]["actor"] == SKEEVER, "a soul is captured: %s" % [_traps])
	var held := _session.inventory().filter(func(r: Dictionary) -> bool: return int(r.get("soul", 0)) > 0 and (int(r["form"]) == PETTY or int(r["form"]) == AZURA))
	expect(held.size() == 1, "a gem holds it: %s" % [held])
