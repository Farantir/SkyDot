# SPDX-License-Identifier: GPL-3.0-or-later
#
# NPC magic on the real SE pack (SKYDOT_SSE_PACK), headless, 7a-5: vanilla bandit mages spawned in front
# of the player (SkydotPapyrus.spawn_actor, the --spawn path of the viewer) fight with their spells.
# EncBandit01MagicNordM (Flames, Healing, Oakflesh, Lesser Ward) sees the player and enters combat,
# holds Flames at him (the player's Health drops, the mage's Magicka drops through pay_spell), heals
# itself with Healing when its Health is low, and its Magicka comes back when it is out of the fight.
# EncBandit03MagicNordM (Ice Spike, Fast Healing, Stoneflesh, Steadfast Ward) puts Stoneflesh on first,
# throws Ice Spike (a missile in the scene that lands on the player) and heals itself with Fast Healing,
# a fire-and-forget healing spell. A third mage with Conjure Familiar added through the VM summons it:
# the familiar is the mage's ally and fights the player. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const FLAMES := 0xC969A
const HEALING := 0x12FCC
const OAKFLESH := 0x5AD5C
const ICE_SPIKE := 0x2B96C
const FAST_HEALING := 0x7231C
const STONEFLESH := 0x72316
const FAMILIAR := 0x640B6
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _home := Vector3.ZERO
var _right := Vector3.RIGHT
var _casts: Array[Array] = []
var _landed: Array[Array] = []
var _started: Array[Array] = []
var _live := 0
var _min_player := 1000.0


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
		print("smoke_magic_npc: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


## One frame: the player stands where he is and cannot die here; his lowest Health is noted.
func _frames(count: int) -> void:
	var papyrus := _session.papyrus
	for i in count:
		_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT
		var health := papyrus.get_actor_value(PLAYER, "Health")
		_min_player = minf(_min_player, health)
		if health < 40.0:
			papyrus.restore_actor_value(PLAYER, "Health", 100.0)
		_session.step(STEP, _feet)
		_live = maxi(_live, _session.projectiles.get_child_count())
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


## Spawn `editor_id` where the bandit stood (`offset` metres to the side), looking at the player.
func _spawn(editor_id: String, offset: float) -> int:
	var world := _session.world
	var npc := world.find_npc(editor_id)
	expect(npc != 0, editor_id + " is in the pack")
	var where := _home + _right * offset
	var here := SkydotWorld.godot_to_skyrim(where)
	var to := SkydotWorld.godot_to_skyrim(_feet) - here
	var ref := _session.papyrus.spawn_actor(npc, world.get_ref_cell(BANDIT), here, atan2(to.x, to.y))
	expect(ref != 0, editor_id + " is spawned")
	return ref


## A place about `distance` metres from `home` with a clear line from a head there to the chest of a
## player standing at it (a cave has pillars and spits), and the ground under it.
func _pick_feet(home: Vector3, forward: Vector3, distance: float) -> Vector3:
	var space := _camera.get_world_3d().direct_space_state
	var from := home + Vector3.UP * 1.6
	for scale in [1.0, 0.8, 1.2, 0.6]:
		for step in 16:
			var dir := forward.rotated(Vector3.UP, step * TAU / 16.0)
			var feet: Vector3 = home + dir * distance * scale
			var free := space.intersect_ray(PhysicsRayQueryParameters3D.create(from, feet + Vector3.UP * 1.2, 1 | 2 | 8)).is_empty()
			var ground := not space.intersect_ray(PhysicsRayQueryParameters3D.create(feet + Vector3.UP * 0.5, feet - Vector3.UP * 0.5, 1 | 2 | 8)).is_empty()
			if free and ground:
				return feet
	expect(false, "no clear place to stand")
	return home + forward * distance


func _spells(ref: int) -> Array:
	var out: Array = []
	for spell in _session.papyrus.spells_of(ref):
		out.append(spell)
	return out


## The mage knows exactly `keep` of the spells (the leveled list of its NPC_ picks others for every
## reference); abilities and powers stay.
func _teach(ref: int, keep: Array) -> void:
	for spell in _spells(ref):
		if int(_session.papyrus.cast_info(spell).get("type", -1)) == 0 and not keep.has(spell):
			_session.papyrus.remove_spell(ref, spell)
	for spell in keep:
		if not _session.papyrus.has_spell(ref, spell):
			expect(_session.papyrus.add_spell(ref, spell), "0x%X is taught" % spell)


func _casts_of(ref: int, spell: int, phase: String) -> int:
	var count := 0
	for c in _casts:
		if c[0] == ref and c[2] == spell and c[4] == phase:
			count += 1
	return count


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
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
	if _session.world == null or _session.ai == null:
		expect(false, "the session has an AI")
		quit(1)
		return
	var papyrus := _session.papyrus
	var ai := _session.ai
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	ai.combat_cast.connect(func(ref: int, target: int, spell: int, hand: int, phase: String) -> void:
		_casts.append([ref, target, spell, hand, phase]))
	_session.magic.npc_spell_landed.connect(func(caster: int, spell: int, target: int, _point: Vector3, hostile: bool) -> void:
		_landed.append([caster, spell, target, hostile]))
	_session.magic.npc_cast_started.connect(func(caster: int, hand: int, spell: int, _info: Dictionary) -> void:
		_started.append([caster, hand, spell]))
	var bandit := _session.world.find_ref_node(BANDIT) as SkydotActor
	expect(bandit != null, "the bandit's place is built")
	if bandit == null:
		quit(1)
		return
	ai.attach_built(bandit.get_parent())
	await _frames(30)
	_home = bandit.global_position
	var forward := -bandit.global_transform.basis.z
	forward.y = 0.0
	forward = forward.normalized()
	_right = forward.cross(Vector3.UP).normalized()
	_feet = _pick_feet(_home, forward, 6.0)
	# The bandit and whatever else lives here stay out of this.
	for other in _session.world.get_cell_actors(_session.world.get_ref_cell(BANDIT)):
		papyrus.damage_actor_value(other, "Health", 100000.0)
	expect(papyrus.is_actor_dead(BANDIT), "the old bandit is dead")
	await _frames(10)

	# ---- Mage A: Flames, Healing
	print("PHASE A: EncBandit01MagicNordM")
	var mage := _spawn("EncBandit01MagicNordM", 0.0)
	_teach(mage, [FLAMES, HEALING, OAKFLESH])
	await _frames(5)
	var known := _spells(mage)
	print("spells of the mage: ", known.map(func(s: int) -> String: return "0x%X" % s))
	expect(known.has(FLAMES) and known.has(HEALING), "the mage knows Flames and Healing: %s" % [known])
	var magicka_start := papyrus.get_actor_value(mage, "Magicka")
	var health_max := papyrus.get_actor_value_max(mage, "Health")
	expect(magicka_start > 50.0, "and has Magicka: %f" % magicka_start)
	var seen := await _until(func() -> bool: return ai.get_combat_state(mage) == 1, 10.0)
	print("the mage enters combat after ", seen, " s, target 0x%X" % ai.get_combat_target(mage))
	expect(seen >= 0.0 and ai.get_combat_target(mage) == PLAYER, "the mage sees the player and enters combat")
	var first := await _until(func() -> bool: return _casts_of(mage, FLAMES, "start") > 0, 20.0)
	print("Flames held after ", first, " s")
	expect(first >= 0.0, "the mage holds Flames at the player: %s" % [_casts.slice(0, 6)])
	_min_player = 1000.0
	var hurt := await _until(func() -> bool: return _min_player < papyrus.get_actor_value_max(PLAYER, "Health") - 2.0, 20.0)
	print("the player is hurt after ", hurt, " s: Health ", _min_player)
	expect(hurt >= 0.0, "the player's Health drops: %f" % _min_player)
	expect(_landed.any(func(l: Array) -> bool: return l[0] == mage and l[1] == FLAMES and l[2] == PLAYER and l[3]), "Flames landed on the player: %d landings" % _landed.size())
	expect(_started.any(func(s: Array) -> bool: return s[0] == mage and s[2] == FLAMES), "the cast was announced (npc_cast_started)")
	expect(_casts_of(mage, OAKFLESH, "start") == 0, "Oakflesh would leave under a quarter of its Magicka: not cast")
	await _until(func() -> bool: return _casts_of(mage, FLAMES, "stop") > 0, 10.0)
	expect(_casts_of(mage, FLAMES, "stop") > 0, "a held Flames ends after a few seconds")
	var magicka_low := papyrus.get_actor_value(mage, "Magicka")
	print("the mage's Magicka: ", magicka_start, " -> ", magicka_low)
	expect(magicka_low < magicka_start - 5.0, "the mage's Magicka drops")
	expect(_session.npc_magic.phase_of(mage) in ["", "concentrating"], "its cast is held or over")
	# Healing: hurt, it heals itself.
	papyrus.damage_actor_value(mage, "Health", health_max * 0.8)
	var hurt_health := papyrus.get_actor_value(mage, "Health")
	var healing := await _until(func() -> bool: return _casts_of(mage, HEALING, "start") > 0, 10.0)
	print("the mage holds Healing after ", healing, " s (Health ", hurt_health, " of ", health_max, ")")
	expect(healing >= 0.0, "the hurt mage casts Healing on itself")
	await _frames(90)
	var healed := papyrus.get_actor_value(mage, "Health")
	print("the mage's Health: ", hurt_health, " -> ", healed)
	expect(healed > hurt_health + 3.0 or papyrus.is_actor_dead(mage) == false and healed >= health_max * 0.5, "its Health rises")
	expect(_landed.any(func(l: Array) -> bool: return l[0] == mage and l[1] == HEALING and l[2] == mage and not l[3]), "Healing landed on the mage")
	# Out of the fight, the Magicka comes back.
	ai.stop_combat(mage)
	ai.set_combat(false)
	await _frames(30)
	var drained := papyrus.get_actor_value(mage, "Magicka")
	await _frames(60 * 12)
	var regained := papyrus.get_actor_value(mage, "Magicka")
	print("the mage's Magicka regenerates: ", drained, " -> ", regained)
	expect(regained > drained + 3.0, "its Magicka regenerates")
	papyrus.damage_actor_value(mage, "Health", 100000.0)
	await _frames(10)

	# ---- Mage B: Stoneflesh, Ice Spike (fire and forget), Fast Healing
	print("PHASE B: EncBandit03MagicNordM")
	ai.set_combat(true)
	_casts.clear()
	_landed.clear()
	_live = 0
	var mage_b := _spawn("EncBandit03MagicNordM", 1.5)
	_teach(mage_b, [ICE_SPIKE, FAST_HEALING, STONEFLESH])
	await _frames(5)
	known = _spells(mage_b)
	print("spells of the mage: ", known.map(func(s: int) -> String: return "0x%X" % s))
	expect(known.has(ICE_SPIKE) and known.has(FAST_HEALING) and known.has(STONEFLESH), "mage B knows Ice Spike, Fast Healing and Stoneflesh")
	var ward := await _until(func() -> bool: return ai.get_combat_state(mage_b) == 1, 10.0)
	expect(ward >= 0.0, "mage B enters combat")
	var armor := await _until(func() -> bool: return _casts_of(mage_b, STONEFLESH, "release") > 0, 15.0)
	print("Stoneflesh released after ", armor, " s")
	expect(armor >= 0.0, "it casts Stoneflesh on itself first: %s" % [_casts.slice(0, 6)])
	await _frames(10)
	expect(papyrus.active_effects(mage_b).any(func(e: Dictionary) -> bool: return e["source"] == STONEFLESH), "Stoneflesh runs on mage B")
	var spike := await _until(func() -> bool: return _casts_of(mage_b, ICE_SPIKE, "release") > 0, 20.0)
	print("Ice Spike released after ", spike, " s, bolts in the scene at most ", _live)
	expect(spike >= 0.0, "it throws Ice Spike")
	var hit := await _until(func() -> bool: return _landed.any(func(l: Array) -> bool: return l[1] == ICE_SPIKE and l[2] == PLAYER), 8.0)
	print("Ice Spike landed after ", hit, " s: ", _landed.filter(func(l: Array) -> bool: return l[1] == ICE_SPIKE).slice(0, 2))
	expect(hit >= 0.0, "Ice Spike lands on the player")
	expect(_live > 0, "the missile flew in the scene")
	var magicka_b := papyrus.get_actor_value(mage_b, "Magicka")
	expect(magicka_b < papyrus.get_actor_value_max(mage_b, "Magicka") - 20.0, "mage B paid for its spells: %f" % magicka_b)
	var health_b := papyrus.get_actor_value_max(mage_b, "Health")
	papyrus.damage_actor_value(mage_b, "Health", health_b * 0.7)
	var low_b := papyrus.get_actor_value(mage_b, "Health")
	var fast := await _until(func() -> bool: return _casts_of(mage_b, FAST_HEALING, "release") > 0, 15.0)
	print("Fast Healing released after ", fast, " s")
	expect(fast >= 0.0, "mage B casts the fire-and-forget Fast Healing when hurt")
	await _frames(60)
	expect(papyrus.get_actor_value(mage_b, "Health") > low_b + 5.0, "and its Health rises: %f -> %f" % [low_b, papyrus.get_actor_value(mage_b, "Health")])
	papyrus.damage_actor_value(mage_b, "Health", 100000.0)
	await _frames(10)

	# ---- Mage C: Conjure Familiar added through the VM; the familiar is the mage's ally
	print("PHASE C: a summoner")
	_casts.clear()
	var mage_c := _spawn("EncBandit03MagicNordM", -1.5)
	_teach(mage_c, [FAMILIAR, ICE_SPIKE])
	await _frames(5)
	var summon := await _until(func() -> bool: return _casts_of(mage_c, FAMILIAR, "release") > 0, 20.0)
	print("Conjure Familiar released after ", summon, " s")
	expect(summon >= 0.0, "mage C summons first: %s" % [_casts.slice(0, 6)])
	await _frames(30)
	var familiar := 0
	for e in papyrus.active_effects(mage_c):
		if e["archetype"] == 18:
			familiar = int(e.get("summoned", 0))
	print("the familiar is 0x%X" % familiar)
	expect(familiar != 0, "a familiar stands next to mage C")
	if familiar != 0:
		expect(ai.get_reaction(familiar, mage_c) == 2, "an ally of mage C: %d" % ai.get_reaction(familiar, mage_c))
		expect(ai.get_reaction(familiar, PLAYER) == ai.get_reaction(mage_c, PLAYER), "and sees the player as mage C does")
		var fights := await _until(func() -> bool: return ai.get_combat_state(familiar) == 1 and ai.get_combat_target(familiar) == PLAYER, 10.0)
		expect(fights >= 0.0, "and it fights the player for mage C")

	# A location spell that is no conjuration (a rune) landing where nobody stands does not hurt its caster.
	var rune_info := _session.magic.info_of(FLAMES).duplicate()
	rune_info["delivery"] = Magic.DELIVER_TARGET_LOCATION
	var before := papyrus.get_actor_value(mage_c, "Health")
	var effects_before := papyrus.active_effects(mage_c).size()
	_landed.clear()
	var node_c := _session.world.find_ref_node(mage_c) as Node3D
	_session.magic.npc_deliver(mage_c, 1, FLAMES, rune_info, node_c.global_position + Vector3.UP * 1.6, Vector3.UP, Vector3.RIGHT, false, false)
	await _frames(5)
	expect(not _landed.any(func(l: Array) -> bool: return l[1] == FLAMES and l[2] == mage_c) and papyrus.get_actor_value(mage_c, "Health") >= before and papyrus.active_effects(mage_c).size() <= effects_before,
		"a location spell that lands nowhere leaves its caster alone")

	print("smoke_magic_npc: ", "FAIL" if failures > 0 else "OK")
	quit(1 if failures > 0 else 0)
