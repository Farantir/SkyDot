# SPDX-License-Identifier: GPL-3.0-or-later
#
# Melee through GameSession on the real SE pack (SKYDOT_SSE_PACK), headless:
# the Iron Sword is drawn (the first-person equip clip's time), the player
# faces the bandit 0x373A6 of BleakFallsBarrow01 and attacks through Combat;
# the HitFrame of the first-person project's clip decides when apply_hit
# runs, its damage is the formula's (combat_rules.gd), a power attack held
# past fInitialPowerAttackDelay does double, reach, strike angle and line of
# sight keep misses from hitting, the bandit's own npc_attack hurts the
# player, and he dies after several hits; the corpse stays dead through a
# save and a load. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x373A6
const IRON_SWORD := 0x12EB7
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _player: FakePlayer
var _camera: Camera3D
var _feet := Vector3.ZERO
var _hits: Array[Dictionary] = []
var _states: Array = []
var _attacks: Array = []


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
		print("smoke_combat_melee: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _near(a: float, b: float, tolerance := 0.01) -> bool:
	return absf(a - b) < tolerance


## Frames of the game: the session steps and physics run.
func _frames(count: int) -> void:
	for i in count:
		_session.step(STEP, _feet)
		await physics_frame


## Seconds until `done` is true, at most `limit`; the time it took, -1 if it did not.
func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


## The player stands `distance` metres in front of the bandit (along his
## facing), eyes at EYE_HEIGHT, looking at his chest.
func _stand_before(node: Node3D, distance: float, yaw_offset := 0.0) -> void:
	var forward := -node.global_transform.basis.z
	forward.y = 0.0
	forward = forward.normalized()
	_feet = node.global_position + forward * distance
	var eye := _feet + Vector3.UP * GameSession.EYE_HEIGHT
	_camera.global_position = eye
	_camera.look_at(node.global_position + Vector3.UP * 1.2, Vector3.UP)
	_camera.rotate_y(yaw_offset)


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	config.wander = false
	_player = FakePlayer.new()
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, _camera, _player), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	var papyrus := _session.papyrus
	_session.hit_landed.connect(func(target: int, result: Dictionary, point: Vector3) -> void:
		_hits.append({"target": target, "result": result, "point": point}))
	_session.weapon_state_changed.connect(func(state: int, clip: String) -> void: _states.append([state, clip]))
	_session.attack_started.connect(func(clip: String, power: bool) -> void: _attacks.append([clip, power]))
	var node := _session.world.find_ref_node(BANDIT) as Node3D
	expect(node != null, "the bandit is built")
	if node == null:
		quit(1)
		return
	await _frames(30)  # the ground under the actors

	# The first-person body (flat mode): hidden while sheathed.
	var body := FirstPersonBody.new(_session, _camera)
	root.add_child(body)
	await process_frame
	expect(not body.visible, "the body is hidden while sheathed")
	var arms := body.get_node_or_null("bethconv_z_up_to_y_up/Skeleton") as Skeleton3D
	expect(arms != null and arms.find_bone("Camera1st [Cam1]") >= 0 and arms.find_bone("Weapon") >= 0, "the first-person skeleton is built")
	var arm_meshes := 0
	if arms != null:
		for child in arms.get_children():
			arm_meshes += 1 if child is MeshInstance3D else 0
	expect(arm_meshes >= 2, "the player's first-person body and hands are skinned onto it: %d meshes" % arm_meshes)

	# Hands first: nothing in the right hand.
	expect(_session.player_weapon() == 0 and _session.combat.clip_prefix() == "h2h_", "bare hands play h2h_")

	# The Iron Sword drawn.
	_session.give([Vector2i(IRON_SWORD, 1)])
	expect(_session.equip(IRON_SWORD), "the Iron Sword is equipped")
	expect(_session.player_weapon() == IRON_SWORD and _session.combat.clip_prefix() == "1hm_", "a sword plays 1hm_")
	expect(_session.weapon_state() == Combat.State.SHEATHED, "it starts sheathed")
	_stand_before(node, 1.2)
	_session.toggle_weapon()
	expect(_session.weapon_state() == Combat.State.DRAWING, "drawing")
	await process_frame
	body.follow()
	var player := body.get_node("AnimationPlayer") as AnimationPlayer
	expect(body.visible and player.current_animation == "1hm_equip", "the body shows and plays 1hm_equip: %s" % player.current_animation)
	expect(body.get_node_or_null("bethconv_z_up_to_y_up/Skeleton/WeaponAttachment") != null, "the sword is on the Weapon bone")
	arms = body.get_node("bethconv_z_up_to_y_up/Skeleton") as Skeleton3D  # built again by the equip
	var cam_bone := arms.get_bone_global_rest(arms.find_bone("Camera1st [Cam1]")).origin
	var at_camera := (body.get_node("bethconv_z_up_to_y_up") as Node3D).global_transform * cam_bone
	expect(at_camera.distance_to(_camera.global_position) < 0.01, "the camera bone is at the camera: %s vs %s" % [at_camera, _camera.global_position])
	var draw_time := await _until(func() -> bool: return _session.weapon_state() == Combat.State.DRAWN, 5.0)
	print("draw took ", draw_time, " s")
	expect(draw_time > 1.0 and draw_time < 2.0, "the equip clip takes about 1.43 s")
	await process_frame
	expect(player.current_animation == "1hm_idle", "then the idle: %s" % player.current_animation)
	expect(_states.size() >= 2 and _states[0][1] == "1hm_equip" and _states[-1][1] == "1hm_idle",
		"the states name 1hm_equip then 1hm_idle: %s" % [_states])

	# A light attack: apply_hit runs at the HitFrame (0.367 s of 1HM_AttackRight).
	var player_health := papyrus.get_actor_value(PLAYER, "Health")
	var skill := papyrus.get_actor_value(PLAYER, "OneHanded")
	var armor := _bandit_armor_percent()
	var expected := 7.0 * (1.0 + 0.5 * skill / 100.0) * (1.0 - armor / 100.0)
	var health := papyrus.get_actor_value(BANDIT, "Health")
	_session.attack_pressed()
	await _frames(1)
	_session.attack_released()
	expect(_attacks.size() == 1 and _attacks[0] == ["1hm_attackright", false], "a light right attack: %s" % [_attacks])
	expect(player.current_animation == "1hm_attackright", "the body plays it: %s" % player.current_animation)
	var before_hit := _hits.size()
	var hit_after := await _until(func() -> bool: return _hits.size() > before_hit, 2.0)
	print("hit after ", hit_after, " s")
	expect(hit_after > 0.3 and hit_after < 0.45, "the hit comes at the HitFrame (0.367 s), got %f" % hit_after)
	expect(_hits.size() == 1 and _hits[0]["target"] == BANDIT, "the bandit is hit")
	if not _hits.is_empty():
		var result: Dictionary = _hits[0]["result"]
		expect(_near(result["damage"], expected, 0.001), "damage %f, expected %f" % [result["damage"], expected])
		expect(_near(health - papyrus.get_actor_value(BANDIT, "Health"), expected, 0.01), "Health drops by it")
	# Too early for the next one; then left.
	expect(not _session.attack(false), "no second attack at once")
	await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)
	expect(_session.attack(false) and _attacks[-1] == ["1hm_attackleft", false], "then the left hand: %s" % [_attacks[-1]])
	await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)
	expect(_hits.size() == 2, "the second swing hit too: %d" % _hits.size())

	# A held press makes a power attack after fInitialPowerAttackDelay.
	var delay := _session.world.get_gmst_float("fInitialPowerAttackDelay", 0.3)
	_session.attack_pressed()
	await _frames(int(delay / STEP) + 6)
	expect(_attacks[-1] == ["1hm_attackpower", true], "held: a power attack: %s" % [_attacks[-1]])
	_session.attack_released()
	var hits_before := _hits.size()
	await _until(func() -> bool: return _hits.size() > hits_before or papyrus.is_actor_dead(BANDIT), 3.0)
	if _hits.size() > hits_before:
		expect(_near(_hits[-1]["result"]["attack"], 2.0), "a power attack does x2: %s" % [_hits[-1]["result"]["attack"]])
	await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)

	# Out of reach, behind the back and through a wall: no hit.
	if not papyrus.is_actor_dead(BANDIT):
		_stand_before(node, 4.0)
		hits_before = _hits.size()
		_session.attack(false)
		await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)
		expect(_hits.size() == hits_before, "4 m is out of reach (%.2f m)" % _session.combat.reach_metres())
		_stand_before(node, 1.2, PI)
		_session.attack(false)
		await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)
		expect(_hits.size() == hits_before, "looking away misses")

	# The bandit strikes the player: from his head along his facing.
	if not papyrus.is_actor_dead(BANDIT):
		_stand_before(node, 1.0)
		expect(_session.npc_attack(BANDIT, false), "npc_attack is accepted")
		await _until(func() -> bool: return papyrus.get_actor_value(PLAYER, "Health") < player_health, 3.0)
		expect(papyrus.get_actor_value(PLAYER, "Health") < player_health, "the bandit hurts the player: %f -> %f" % [player_health, papyrus.get_actor_value(PLAYER, "Health")])
		expect(_hits[-1]["target"] == PLAYER, "hit_landed names the player")
		await _frames(120)

	# VR: the sword in the right controller; a slow move does nothing, a fast one hits.
	if not papyrus.is_actor_dead(BANDIT):
		var controller := Node3D.new()
		root.add_child(controller)
		var hand := FirstPersonBody.new(_session, null, controller)
		root.add_child(hand)
		await process_frame
		expect(controller.get_node_or_null("Weapon") != null, "VR: the weapon model is in the controller")
		_stand_before(node, 1.0)
		var aim := node.global_position + Vector3.UP * 1.2
		controller.global_position = aim + Vector3(0.0, 0.0, 1.5)
		controller.look_at(aim, Vector3.UP)
		hits_before = _hits.size()
		hand.update_hand(STEP)
		controller.global_position += Vector3(0.0, 0.0, -0.01)
		hand.update_hand(STEP)  # 0.6 m/s: no swing
		expect(_hits.size() == hits_before, "a slow move is no swing")
		controller.global_position = aim + Vector3(0.0, 0.0, 0.4)
		hand.update_hand(STEP)  # a jump of a metre in a frame
		expect(_hits.size() == hits_before + 1 and _hits[-1]["target"] == BANDIT, "a fast swing hits through the same path")
		hand.update_hand(STEP)
		expect(_hits.size() == hits_before + 1, "and not again within the cooldown")
		controller.queue_free()
		hand.queue_free()
		await process_frame

	# Until dead.
	_stand_before(node, 1.2)
	var swings := 0
	while not papyrus.is_actor_dead(BANDIT) and swings < 40:
		await _until(func() -> bool: return _session.attack(false), 3.0)
		await _until(func() -> bool: return _session.combat.current_attack().is_empty(), 3.0)
		swings += 1
	expect(papyrus.is_actor_dead(BANDIT), "the bandit dies after %d more swings" % swings)
	expect(papyrus.get_killer(BANDIT) == PLAYER, "the player is the killer")

	# Sheathe, save, load.
	_session.toggle_weapon()
	await _until(func() -> bool: return _session.weapon_state() == Combat.State.SHEATHED, 5.0)
	expect(_session.weapon_state() == Combat.State.SHEATHED, "sheathed again")
	await process_frame
	expect(not body.visible, "the body is hidden again")
	expect(_session.save_game("user://combat_melee.skydot"), "saves")
	expect(_session.load_game("user://combat_melee.skydot"), "loads")
	expect(papyrus.is_actor_dead(BANDIT), "still dead")
	expect(_session.player_weapon() == IRON_SWORD, "the sword is still in hand")
	print("smoke_combat_melee: failures=%d" % failures)
	quit(failures)


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
