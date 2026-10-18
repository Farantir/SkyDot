# SPDX-License-Identifier: GPL-3.0-or-later
#
# NPC gear, stance and actions on the real SE pack (SKYDOT_SSE_PACK), headless
# (GAMEPLAY-PLAN.md, 6g): the bandit 0x39FE4 of BleakFallsBarrow01 holds a
# weapon in its right hand once its inventory is resolved, its model hangs on
# the sheath bone and moves to the WEAPON bone when it draws; set_stance picks
# 1HM_Idle; play_action plays a one-shot; npc_attack against the player hurts
# him with that weapon, and against another built actor hits that actor; the
# skeever's idle is found. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const STEP := 1.0 / 60.0
const SKEEVER_PROJECT := "meshes/actors/skeever/skeeverproject.hkx"

var failures := 0
var _session: GameSession
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
		print("smoke_combat_npc: SKIP (SKYDOT_SSE_PACK unset)")
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


## The attachments of an actor's skeleton by node name -> bone name.
func _gear_bones(node: Node) -> Dictionary:
	var out := {}
	for attachment in node.find_children("Gear_*", "BoneAttachment3D", true, false):
		out[String(attachment.name)] = (attachment as BoneAttachment3D).bone_name.to_lower()
	return out


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	config.wander = false
	var camera := Camera3D.new()
	root.add_child(camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, camera, FakePlayer.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	var world := _session.world
	var papyrus := _session.papyrus
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	_session.hit_landed.connect(func(target: int, result: Dictionary, _point: Vector3) -> void:
		_hits.append({"target": target, "result": result}))
	var node := world.find_ref_node(BANDIT) as SkydotActor
	expect(node != null, "the bandit is built")
	if node == null:
		quit(1)
		return
	await _frames(30)

	# The skeever's idle: Main_Idle was missed by the name rule.
	var skeever := world.get_locomotion(SKEEVER_PROJECT)
	print("skeever idle: ", skeever["idle"])
	expect(skeever["idle"]["file"] != "" and String(skeever["idle"]["name"]).to_lower() == "main_idle",
		"the skeever's idle is Main_Idle: %s" % [skeever["idle"]])

	# Gear: a weapon in the right hand once the inventory is resolved.
	var equipment := papyrus.equipment(BANDIT)
	var weapon := world.get_weapon(int(equipment["right"]))
	print("bandit right hand: ", equipment["right"], " ", weapon.get("editor_id"), " damage ", weapon.get("damage"))
	expect(equipment["right"] != 0 and not weapon.is_empty() and int(weapon["weapon_type"]) in [1, 2, 3, 4, 5, 6],
		"the bandit holds a melee weapon: %s" % [equipment])
	expect(not _gear_bones(node).is_empty(), "the weapon model hangs on the skeleton: %s" % [_gear_bones(node)])
	var sheath: String = String(ActorGear.SHEATH_BONES.get(int(weapon.get("weapon_type", 0)), "?")).to_lower()
	expect(_gear_bones(node).get("Gear_Weapon", "") == sheath, "sheathed on %s: %s" % [sheath, _gear_bones(node)])

	# Stance by name.
	var plan := world.get_actor_plan(BANDIT)
	var loco := world.get_locomotion(plan["behaviour"])
	expect(loco["clips"].has("1hm_idle") and loco["clips"].has("1hm_walkforward") and loco["clips"].has("1hm_runforward"),
		"the project has the 1hm set")
	expect(node.set_stance("1HM") and node.get_stance() == "1hm", "set_stance(1HM) finds the set")
	await _frames(3)
	expect(node.get_playing() == "stance_idle", "the idle is the stance's: %s" % node.get_playing())
	var player := node.get_node("AnimationPlayer") as AnimationPlayer
	expect(player.has_animation("stance_idle") and player.has_animation("stance_walk") and player.has_animation("stance_run"),
		"the stance has idle, walk and run")
	expect(node.set_stance("") and node.get_stance() == "", "an empty prefix restores the normal set")
	await _frames(3)
	expect(node.get_playing() == "idle", "back to the idle: %s" % node.get_playing())
	expect(not node.set_stance("nosuchstance"), "an unknown stance is refused")

	# Draw: stance, equip clip, the model in hand halfway through it.
	expect(_session.npc_draw(BANDIT, true), "the bandit draws")
	expect(node.get_stance() == "1hm" and node.is_acting(), "stance 1hm and the equip clip plays: %s" % node.get_stance())
	await _until(func() -> bool: return _gear_bones(node).get("Gear_Weapon", "") == "weapon", 3.0)
	expect(_gear_bones(node).get("Gear_Weapon", "") == "weapon", "the weapon is on the WEAPON bone: %s" % [_gear_bones(node)])
	await _until(func() -> bool: return not node.is_acting(), 3.0)
	expect(not node.is_acting(), "the equip clip ends")

	# play_action returns its length and gives the gait back.
	var attack_clip: String = loco["clips"]["1hm_attackright"]["file"]
	var length := node.play_action(attack_clip, 1.0)
	expect(length > 0.3 and node.is_acting(), "play_action plays 1hm_attackright: %f s" % length)
	expect(node.play_action("meshes/nothing/here.hkx", 1.0) == 0.0, "a missing clip plays nothing")
	await _until(func() -> bool: return not node.is_acting(), length + 1.0)
	await _frames(3)
	expect(node.get_playing() == "stance_idle", "then the stance idle again: %s" % node.get_playing())

	# Look target while standing.
	var start_yaw := node.rotation.y
	node.set_look_target(node.global_position + Vector3(1.0, 0.0, 0.0) * 5.0)
	await _frames(120)
	var forward := -node.global_transform.basis.z
	expect(node.has_look_target() and forward.dot(Vector3.RIGHT) > 0.95, "the bandit faces the look target: %s (from yaw %f)" % [forward, start_yaw])
	node.clear_look_target()

	# The bandit hits the player with his weapon.
	node.face(start_yaw)  # as placed: the room is before him, a wall may be elsewhere
	forward = -node.global_transform.basis.z
	forward.y = 0.0
	_feet = node.global_position + forward.normalized() * 1.0
	await _frames(2)  # the session learns where the player stands
	var health := papyrus.get_actor_value(PLAYER, "Health")
	_hits.clear()
	expect(_session.npc_attack(BANDIT, false), "npc_attack against the player is accepted")
	expect(node.is_acting(), "the attacker plays its clip")
	await _until(func() -> bool: return not _hits.is_empty(), 3.0)
	expect(not _hits.is_empty() and _hits[0]["target"] == PLAYER, "the player is hit: %s" % [_hits])
	if not _hits.is_empty():
		expect(_hits[0]["result"]["weapon"] == equipment["right"], "with the bandit's weapon, not hands: %s" % [_hits[0]["result"]["weapon"]])
		expect(papyrus.get_actor_value(PLAYER, "Health") < health, "the player lost health")
	await _until(func() -> bool: return not node.is_acting(), 3.0)

	# Actor against actor: another built actor of the cell next to him, the player far away.
	var other: SkydotActor = null
	var other_ref := 0
	for candidate in node.get_parent().find_children("*", "SkydotActor", true, false):
		var ref: int = candidate.get_meta("skydot_ref", 0)
		if ref != 0 and ref != BANDIT and not papyrus.is_actor_dead(ref):
			other = candidate
			other_ref = ref
			break
	expect(other != null, "the cell has another actor")
	if other != null:
		_feet = node.global_position + Vector3(0, -500, 0)
		node.set_dead(false)
		var spot := node.global_position + forward.normalized() * 1.0
		other.teleport(spot)
		await _frames(2)
		_hits.clear()
		expect(_session.npc_attack(BANDIT, false, other_ref), "npc_attack against 0x%X is accepted" % other_ref)
		await _until(func() -> bool: return not _hits.is_empty(), 3.0)
		expect(not _hits.is_empty() and _hits[0]["target"] == other_ref, "the hit lands on 0x%X: %s" % [other_ref, [_hits.size()]])
		await _until(func() -> bool: return not node.is_acting(), 3.0)

	# Sheathe: the model returns to the sheath.
	expect(_session.npc_draw(BANDIT, false), "the bandit sheathes")
	expect(node.get_stance() == "", "no stance when sheathed")
	await _until(func() -> bool: return _gear_bones(node).get("Gear_Weapon", "") == sheath, 3.0)
	expect(_gear_bones(node).get("Gear_Weapon", "") == sheath, "the weapon is back on %s: %s" % [sheath, _gear_bones(node)])
	print("smoke_combat_npc: failures=%d" % failures)
	quit(failures)
