# SPDX-License-Identifier: GPL-3.0-or-later
#
# Casting through GameSession on the real SE pack (SKYDOT_SSE_PACK), headless, 7a-3: the real
# Magic path (hands readied, attack pressed, Magic stepped every frame, released), projectiles
# flying in the scene. Healing held on the player restores Health and spends Magicka; Flames
# held at the skeever 0x3B9F7 of BleakFallsBarrow01 drains Magicka while held (the regeneration
# waits after it), leaves as flame bolts and burns the skeever to death; Firebolt, charged and
# released at the bandit 0x39FE4, flies as a missile and lands on him; a released-too-early
# spell costs nothing; a power cast with Z is paid once and refuses its second cast; the hand
# art of Flames and the first-person clips are named. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const SKEEVER := 0x3B9F7
const BANDIT := 0x39FE4
const HEALING := 0x12FCC
const FLAMES := 0x12FCD
const FIREBOLT := 0x12FD0
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _landed: Array[Dictionary] = []
var _clips: Array[String] = []
var _arts: Array = []
var _max_live := 0


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
		print("smoke_magic_cast: SKIP (SKYDOT_SSE_PACK unset)")
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
		_max_live = maxi(_max_live, _session.projectiles.get_child_count())
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


## The player stands about `distance` metres from `node`, on the side of it with a clear line from
## the eyes to its chest (a cave has walls), and looks at it.
func _stand_before(node: Node3D, distance: float) -> void:
	var space := _camera.get_world_3d().direct_space_state
	var chest := node.global_position + Vector3.UP * 0.6
	var forward := -node.global_transform.basis.z
	forward.y = 0.0
	forward = forward.normalized()
	for scale in [1.0, 0.75, 0.5, 1.25]:
		for step in 12:
			var dir := forward.rotated(Vector3.UP, step * TAU / 12.0)
			var feet: Vector3 = node.global_position + dir * distance * scale
			var eye := feet + Vector3.UP * GameSession.EYE_HEIGHT
			var free := space.intersect_ray(PhysicsRayQueryParameters3D.create(eye, chest, 1 | 2 | 8)).is_empty()
			var to_chest := (chest - eye).normalized()
			var side_axis := to_chest.cross(Vector3.UP).normalized()
			var ahead := true  # the shots leave a little to the side and below the eyes
			for offset in [-0.15, 0.0, 0.15]:
				var from: Vector3 = eye + to_chest * 0.5 + side_axis * offset - Vector3.UP * 0.12
				ahead = ahead and space.intersect_ray(PhysicsRayQueryParameters3D.create(from, chest, 1 | 2 | 8)).is_empty()
			var ground := not space.intersect_ray(PhysicsRayQueryParameters3D.create(feet + Vector3.UP * 0.5, feet - Vector3.UP * 0.5, 1 | 2 | 8)).is_empty()
			if free and ahead and ground:
				_feet = feet
				_camera.global_position = eye
				_camera.look_at(chest, Vector3.UP)
				return
	expect(false, "no clear place to stand before 0x%X" % int(node.get_meta("skydot_ref", 0)))


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
	var magic := _session.magic
	_session.spell_landed.connect(func(hand: int, spell: int, target: int, point: Vector3, hostile: bool) -> void:
		_landed.append({"hand": hand, "spell": spell, "target": target, "point": point, "hostile": hostile}))
	magic.hand_clip.connect(func(clip: String, _loops: bool, _speed: float) -> void: _clips.append(clip))
	magic.hand_art_changed.connect(func(hand: int, model: String) -> void: _arts.append([hand, model]))
	var skeever := _session.world.find_ref_node(SKEEVER) as Node3D
	var bandit := _session.world.find_ref_node(BANDIT) as Node3D
	expect(skeever != null and bandit != null, "the skeever and the bandit are built")
	if skeever == null or bandit == null:
		quit(1)
		return
	await _frames(30)
	var body := FirstPersonBody.new(_session, _camera)
	root.add_child(body)
	await process_frame

	# ---- the hands
	expect(papyrus.add_spell(PLAYER, FIREBOLT), "Firebolt is taught")
	expect(_session.equip_spell(HEALING, Magic.RIGHT) and magic.spell_in(Magic.RIGHT) == HEALING, "Healing is in the right hand")
	expect(magic.hands_with_spells() == [Magic.RIGHT] and magic.ready_state() == Magic.Ready.SHEATHED, "sheathed")
	_stand_before(skeever, 4.0)

	# ---- Healing: a press on a sheathed hand readies it, then it is held on the player
	var max_health := papyrus.get_actor_value_max(PLAYER, "Health")
	papyrus.damage_actor_value(PLAYER, "Health", 60.0)
	_session.attack_pressed()
	_session.attack_released()
	expect(magic.ready_state() == Magic.Ready.READYING and _clips.has("mrh_equip"), "the first press readies the hand: %s" % [_clips])
	var ready_time := await _until(func() -> bool: return magic.ready_state() == Magic.Ready.READY, 5.0)
	print("readying took ", ready_time, " s")
	expect(ready_time > 0.2 and ready_time < 3.0, "the equip clip takes its time: %f" % ready_time)
	await process_frame
	expect(not body.get_node_or_null("bethconv_z_up_to_y_up/Skeleton") == null and body.visible, "the first-person arms show for readied hands")
	var player_anim := body.get_node("AnimationPlayer") as AnimationPlayer
	expect(player_anim.current_animation == "mrh_readyloop", "and idle in mrh_readyloop: %s" % player_anim.current_animation)
	var hurt := papyrus.get_actor_value(PLAYER, "Health")
	var magicka := papyrus.get_actor_value(PLAYER, "Magicka")
	_session.attack_pressed()
	expect(magic.is_casting(Magic.RIGHT), "Healing is held")
	await _frames(60)
	_session.attack_released()
	expect(not magic.is_casting(Magic.RIGHT), "and let go")
	var healed := papyrus.get_actor_value(PLAYER, "Health")
	print("Healing held for 1 s: Health ", hurt, " -> ", healed, ", Magicka ", magicka, " -> ", papyrus.get_actor_value(PLAYER, "Magicka"))
	expect(healed > hurt + 5.0 and healed < max_health, "Health rises while Healing is held: %f -> %f" % [hurt, healed])
	expect(papyrus.get_actor_value(PLAYER, "Magicka") < magicka - 5.0, "and Magicka is spent")
	expect(_landed.any(func(l: Dictionary) -> bool: return l["spell"] == HEALING and l["target"] == PLAYER and not l["hostile"]), "Healing landed on the player")
	papyrus.dispel_all_spells(PLAYER)
	papyrus.restore_actor_value(PLAYER, "Health", 1000.0)

	# ---- released too early: nothing, no cost
	expect(_session.equip_spell(FIREBOLT, Magic.RIGHT), "Firebolt in the right hand")
	papyrus.restore_actor_value(PLAYER, "Magicka", 1000.0)
	await _frames(2)
	var full := papyrus.get_actor_value(PLAYER, "Magicka")
	_session.attack_pressed()
	await _frames(3)
	_session.attack_released()
	expect(papyrus.get_actor_value(PLAYER, "Magicka") == full and magic.phase_of(Magic.RIGHT) == "", "a Firebolt let go too early costs nothing")

	# ---- Firebolt at the bandit: charged, released, a missile in flight, lands on him
	_stand_before(bandit, 5.0)
	await _frames(2)
	var bandit_health := papyrus.get_actor_value(BANDIT, "Health")
	_landed.clear()
	_session.attack_pressed()
	expect(magic.is_casting(Magic.RIGHT) and magic.phase_of(Magic.RIGHT) == "charging", "Firebolt is charging")
	var charge_time := await _until(func() -> bool: return magic.phase_of(Magic.RIGHT) == "charged", 5.0)
	print("Firebolt charged after ", charge_time, " s")
	expect(charge_time > 0.0, "it charges")
	var before_cast := papyrus.get_actor_value(PLAYER, "Magicka")
	_max_live = 0
	_session.attack_released()
	expect(papyrus.get_actor_value(PLAYER, "Magicka") < before_cast, "the cost is paid at the release")
	await _frames(1)
	expect(_session.projectiles.get_child_count() > 0, "the missile flies in the scene")
	var flight := await _until(func() -> bool: return not _landed.is_empty(), 4.0)
	print("Firebolt landed after ", flight, " s: ", _landed)
	expect(flight > 0.05, "it lands some time after it left")
	expect(not _landed.is_empty() and _landed[0]["target"] == BANDIT and _landed[0]["hostile"], "on the bandit")
	expect(papyrus.get_actor_value(BANDIT, "Health") < bandit_health or papyrus.is_actor_dead(BANDIT), "who is hurt")
	await _frames(30)

	# ---- Flames at the skeever: concentration, flame bolts, Magicka drains, the skeever burns
	_session.equip_spell(FLAMES, Magic.RIGHT)
	expect(_arts.any(func(a: Array) -> bool: return a[0] == Magic.RIGHT and a[1] != ""), "the casting art of the right hand is named: %s" % [_arts])
	print("hand art: ", _arts)
	for spell in [HEALING, FIREBOLT, FLAMES]:
		var shown := magic.info_of(spell)
		print("cast_info 0x%X: delivery %s casting %s proj 0x%X explosion 0x%X casting_art 0x%X hit_art 0x%X area %s" % [spell, shown["delivery"], shown["casting_type"], shown["projectile"], shown["explosion"], shown["casting_art"], shown["hit_art"], shown["area_units"]])
	var art_node := body.get_node_or_null("bethconv_z_up_to_y_up/Skeleton/CastingArt1")
	expect(art_node != null and art_node.get_child_count() > 0, "and sits on the right magic node")
	_stand_before(skeever, 4.0)
	papyrus.restore_actor_value(PLAYER, "Magicka", 1000.0)
	await _frames(2)
	var skeever_health := papyrus.get_actor_value(SKEEVER, "Health")
	var start_magicka := papyrus.get_actor_value(PLAYER, "Magicka")
	var last := start_magicka
	var monotonic := true
	var burning := false
	_max_live = 0
	_landed.clear()
	var puffs_before: int = _session.projectiles.puffs_spawned
	_session.attack_pressed()
	expect(magic.is_casting(Magic.RIGHT), "Flames is held")
	var held := 0.0
	while not papyrus.is_actor_dead(SKEEVER) and held < 15.0:
		await _frames(1)
		held += STEP
		var now := papyrus.get_actor_value(PLAYER, "Magicka")
		monotonic = monotonic and now <= last + 1e-6
		last = now
		burning = burning or papyrus.active_effects(SKEEVER).any(func(e: Dictionary) -> bool: return e["mgef"] == 0x13CA9)
	print("Flames held for ", held, " s: Magicka ", start_magicka, " -> ", last, ", skeever ", skeever_health, " -> ", papyrus.get_actor_value(SKEEVER, "Health"),
		", bolts at most ", _max_live, ", landings ", _landed.size())
	expect(monotonic and last < start_magicka - 5.0, "Magicka drains while it is held and never comes back")
	expect(_max_live >= 1, "flame bolts fly")
	var puffs: int = _session.projectiles.puffs_spawned - puffs_before
	print("Flames puffs ", puffs, " for ", _landed.size(), " landings")
	expect(puffs >= 5 * _landed.size(), "a held flame streams visual puffs between its shots: %d puffs, %d landings" % [puffs, _landed.size()])
	expect(burning, "the skeever burns (FireDamageConcAimed)")
	expect(papyrus.is_actor_dead(SKEEVER), "and dies")
	expect(_landed.size() > 3 and _landed.all(func(l: Dictionary) -> bool: return l["target"] == SKEEVER), "Flames landed on the skeever repeatedly: %d" % _landed.size())
	_session.attack_released()
	var after_release := papyrus.get_actor_value(PLAYER, "Magicka")
	await _frames(20)
	expect(papyrus.get_actor_value(PLAYER, "Magicka") == after_release, "the regeneration waits after the last tick")
	await _frames(60 * 6)
	expect(papyrus.get_actor_value(PLAYER, "Magicka") > after_release, "and then Magicka comes back")
	expect(_clips.has("mrh_aimedconcentration"), "the arms played the concentration clip: %s" % [_clips])

	# ---- Magicka out: the held spell stops by itself
	papyrus.damage_actor_value(PLAYER, "Magicka", papyrus.get_actor_value(PLAYER, "Magicka") - 3.0)
	_session.attack_pressed()
	await _frames(1)
	await _frames(60)
	expect(not magic.is_casting(Magic.RIGHT), "a concentration spell stops when the Magicka is gone")
	_session.attack_released()

	# ---- the power key
	var power := 0
	for known in papyrus.spells_of(PLAYER):
		var power_info := magic.info_of(known)
		if power_info.get("found", false) and int(power_info["type"]) == 2:
			power = known
			break
	print("greater power of the player: 0x%X" % power)
	expect(power != 0, "the player has a greater power")
	if power != 0:
		expect(_session.equip_spell(power, Magic.POWER) and magic.spell_in(Magic.POWER) == power, "the power is in the power slot")
		papyrus.restore_actor_value(PLAYER, "Magicka", 1000.0)
		var first := _session.cast_power()
		print("power cast: ", first, " cooldown ", papyrus.power_cooldown(PLAYER, power))
		expect(first["ok"], "the power key casts it: %s" % [first])
		expect(papyrus.power_cooldown(PLAYER, power) > 0.0, "and starts the cooldown")
		var second := _session.cast_power()
		expect(not second["ok"], "a greater power once a day: the second cast is refused: %s" % [second])
		expect(_session.cast_power() == second, "and again")

	# ---- unequipped hands lower
	_session.toggle_weapon()
	expect(magic.ready_state() == Magic.Ready.SHEATHED, "R lowers the hands")
	await _frames(2)
	expect(not body.visible, "the arms are hidden again")

	print("smoke_magic_cast: ", "FAIL" if failures > 0 else "OK")
	quit(1 if failures > 0 else 0)
