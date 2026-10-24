# SPDX-License-Identifier: GPL-3.0-or-later
#
# Enchanted weapons and poisons through Combat on the real SE pack (SKYDOT_SSE_PACK), headless,
# 7a-3 (disenchanting and enchanting are a later step): the Dremora Fire Sword 0x17009 is drawn
# and hits the bandit 0x39FE4 of BleakFallsBarrow01 as in sse_combat_melee; its enchantment's
# effects land on him, the charge drops by the cost at each hit (the weapon moves to a new
# ItemExtra every time, and the next hit names the new one; running out of charge is covered by the
# C++ tests), and a poison put on the weapon with
# use_item is delivered by the next hit and gone after it. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const FIRE_SWORD := 0x17009
const POISON := 0x3A5A4  # Weak Poison: Damage Health
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _hits: Array[Dictionary] = []


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

	print("smoke_magic_enchant: ", "FAIL" if failures > 0 else "OK")
	quit(1 if failures > 0 else 0)
