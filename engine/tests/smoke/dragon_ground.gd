# SPDX-License-Identifier: GPL-3.0-or-later
#
# A dragon on the ground on the real SE pack (SKYDOT_SSE_PACK), headless, in phases: EncDragon01Fire
# 0x1CA03 (DragonRace, dragonproject.hkx, the Dragon_Default_MT movement type: 384 units/s forward,
# 0.589 rad/s turning in place) is built with its skeleton and a clip table, fights the player in
# the combat stance, turns toward him at the movement type's rate when he stands beside it, and walks to
# him. The plain between Whiterun and the Western Watchtower around Skyrim (13100, -13650) is within
# 2.4 m of flat over 35 m. Later phases add the bite, the tail and the breath. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const DRAGON := 0x1CA03
const STEP := 1.0 / 60.0
const SPOT := Vector3(13100.0, -13650.0, -3000.0)  # Skyrim units; the ground is below
const TURN_RATE := 0.5890486  # rad/s, Dragon_Default_MT rotate_in_place
const WALK_SPEED := 384.0 * 0.0142875  # m/s, Dragon_Default_MT forward

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _node: SkydotActor
var _attacks: Array[Array] = []
var _hits: Array[Dictionary] = []
var _staggers: Array[Array] = []
var _lost := 0.0
var _shouts := 0
var _health := 0.0


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
		print("smoke_dragon_ground: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _frames(count: int) -> void:
	for i in count:
		_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT
		var health := _session.papyrus.get_actor_value(PLAYER, "Health")
		_lost += maxf(_health - health, 0.0)
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100000.0)
		_health = _session.papyrus.get_actor_value(PLAYER, "Health")
		_session.step(STEP, _feet)
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


func _ground(at: Vector3) -> Vector3:
	var space := _camera.get_world_3d().direct_space_state
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(at + Vector3.UP * 200.0, at - Vector3.UP * 400.0,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN))
	return hit["position"] if not hit.is_empty() else at


func _flat(v: Vector3) -> Vector2:
	return Vector2(v.x, v.z)


## The player stands `bearing` degrees from the dragon's facing (positive to its right), 3 m off, and it
## attacks him (Session.npc_attack): the clip must be `clip` (a file, or a part of its name), with a hit
## in its strike angle, and a tail or wing blow staggers the player (`area`).
func _staged_attack(ref: int, bearing: float, clip: String, area: bool) -> void:
	_session.ai.hold(ref, 4.0)  # its own choices wait (the combat AI would attack again and again)
	_node.face(0.0)
	_node.clear_look_target()
	var forward := Vector3(0.0, 0.0, -1.0)
	_feet = _ground(_node.global_position + forward.rotated(Vector3.UP, -deg_to_rad(bearing)) * 3.0)
	await _frames(20)
	_attacks.clear()
	_hits.clear()
	_staggers.clear()
	_lost = 0.0
	var yaw := _node.rotation.y
	expect(_session.npc_attack(ref, false, PLAYER), "the dragon attacks at bearing %.0f" % bearing)
	await _frames(170)
	var files: Array = _attacks.map(func(a: Array) -> String: return String(a[1]).get_file())
	print("dragon: bearing %.0f: attack %s, hits %d, health lost %.1f, staggers %s, turned %.3f rad" % [bearing, files, _hits.size(), _lost, _staggers, absf(wrapf(_node.rotation.y - yaw, -PI, PI))])
	expect(files.size() == 1 and String(files[0]).contains(clip), "bearing %.0f picks %s: %s" % [bearing, clip, files])
	expect(absf(wrapf(_node.rotation.y - yaw, -PI, PI)) < 0.05, "it does not turn to him first: %.3f rad" % absf(wrapf(_node.rotation.y - yaw, -PI, PI)))
	expect(_hits.size() == 1 and _lost > 10.0, "and hits him: %d hits, %.1f health" % [_hits.size(), _lost])
	if area:
		expect(_staggers.any(func(s: Array) -> bool: return s[1] == PLAYER), "the blow staggers the player: %s" % [_staggers])


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.quests = false
	config.preload_doors = false
	config.wander = false
	config.radius = 1
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, _camera, FakePlayer.new()), "the session opens")
	if _session.world == null or _session.ai == null:
		quit(1)
		return
	var world := _session.world
	expect(_session.start_game(), "the game starts")
	_session.combat.npc_attack_started.connect(func(attacker: int, clip: String, _power: bool) -> void: _attacks.append([attacker, clip]))
	_session.hit_landed.connect(func(target: int, result: Dictionary, _point: Vector3) -> void:
		if target == PLAYER:
			_hits.append(result))
	_session.combat.stagger_wanted.connect(func(source: int, target: int, magnitude: float) -> void: _staggers.append([source, target, magnitude]))
	_feet = SkydotWorld.skyrim_position(SPOT)
	expect(_session.enter_world("Tamriel", _feet, null), "the worldspace is entered")
	await _frames(240)
	var centre := _ground(_feet)

	# Phase 0: the dragon's clip table (found by name in dragonproject.hkx).
	var loco := world.get_locomotion("meshes/actors/dragon/dragonproject.hkx")
	var table := {}
	for slot in ["idle", "walk", "run", "turn_left", "turn_right"]:
		table[slot] = String(loco[slot]["name"])
	print("dragon: clip table ", table)
	expect(String(loco["idle"]["file"]).ends_with("ground_combatidle.hkx"), "the idle is ground_combatidle: %s" % [loco["idle"]["file"]])
	expect(String(loco["walk"]["file"]).ends_with("mtforwardground.hkx"), "the walk is mtforwardground: %s" % [loco["walk"]["file"]])
	expect(absf(float(loco["walk"]["speed"]) - 384.0) < 1.0, "it walks 384 units/s: %s" % [loco["walk"]["speed"]])
	expect(String(loco["turn_left"]["file"]).ends_with("mtturnleftground45.hkx"), "the left turn: %s" % [loco["turn_left"]["file"]])
	expect(String(loco["turn_right"]["file"]).ends_with("mtturnrightground45.hkx"), "the right turn: %s" % [loco["turn_right"]["file"]])
	var clips: Dictionary = loco["clips"]
	for name in ["clip_ground_combat_default_standing_idle", "clip_ground_combat_attack_bite", "clip_ground_combat_attack_tail",
			"clip_shout_exhale_firebreath", "clip_ground_stagger"]:
		expect(clips.has(name), "the table has " + name)

	# Phase 1: built (skeleton, meshes), standing on the ground in the combat stance. The player stands 25 m
	# to its right (it faces Skyrim's +y, which is -z here).
	var player_at := _ground(centre + Vector3(25.0, 0.0, 0.0))
	_feet = player_at
	var ref: int = _session.papyrus.spawn_actor(DRAGON, _session.place.cell_id, SkydotWorld.godot_to_skyrim(centre), 0.0)
	expect(ref != 0, "the dragon is spawned")
	_node = world.find_ref_node(ref) as SkydotActor
	await _frames(30)
	_node = world.find_ref_node(ref) as SkydotActor
	if _node == null:
		expect(false, "the dragon is built")
		quit(1)
		return
	var skeletons := _node.find_children("*", "Skeleton3D", true, false)
	expect(skeletons.size() == 1 and (skeletons[0] as Skeleton3D).get_bone_count() > 60, "one skeleton with its bones")
	expect(_node.find_children("*", "MeshInstance3D", true, false).size() >= 4, "its meshes")
	expect(_node.height > 3.0 and _node.radius > 1.0, "a big body: %.2f m high, radius %.2f" % [_node.height, _node.radius])
	var landed := await _until(func() -> bool: return _node.get_state() != "held", 5.0)
	expect(landed >= 0.0, "it finds the ground (not held)")
	expect(absf(_node.get_turn_rate() - TURN_RATE) < 0.001, "it turns at the movement type's rate: %.3f" % _node.get_turn_rate())
	await _frames(30)
	expect(_node.get_playing() in ["idle", "stance_idle", "turn_left", "turn_right"], "it stands in its idle or turns: '%s'" % _node.get_playing())

	# Phase 2: combat. The combat stance's idle and forward clips are the ground ones.
	_session.ai.start_combat(ref, PLAYER)
	await _frames(20)
	expect(_session.ai.get_combat_state(ref) == 1, "it fights the player: state %d" % _session.ai.get_combat_state(ref))
	expect(_node.get_stance() == "combat", "in the combat stance: '%s'" % _node.get_stance())
	print("dragon: playing ", _node.get_playing(), " stance ", _node.get_stance())

	# Phase 3: it turns toward the player standing beside it at about 0.589 rad/s, on the spot, in a turn clip.
	var start_yaw := _node.rotation.y
	var start_pos := _node.global_position
	var turn_clips := {}
	var yaw_before := start_yaw
	var turned := 0.0
	var time := 0.0
	var yaw_t0 := -1.0
	var yaw_0 := 0.0
	while time < 1.4:
		await _frames(1)
		time += STEP
		turn_clips[_node.get_playing()] = true
		if time >= 0.4 and yaw_t0 < 0.0:
			yaw_t0 = time
			yaw_0 = _node.rotation.y
	var rate := absf(wrapf(_node.rotation.y - yaw_0, -PI, PI)) / (time - yaw_t0)
	print("dragon: turn rate %.3f rad/s (%.1f deg turned in %.2f s), clips %s" % [rate, rad_to_deg(absf(wrapf(_node.rotation.y - start_yaw, -PI, PI))), time, turn_clips.keys()])
	expect(absf(rate - TURN_RATE) < TURN_RATE * 0.3, "it turns at about %.3f rad/s: %.3f" % [TURN_RATE, rate])
	expect(_flat(_node.global_position).distance_to(_flat(start_pos)) < 1.0, "turning on the spot: %.2f m" % _flat(_node.global_position).distance_to(_flat(start_pos)))
	expect(turn_clips.has("turn_right") or turn_clips.has("turn_left"), "with a turn clip: %s" % [turn_clips.keys()])
	expect(_node.rotation.y < start_yaw, "to the right (the player is at its right): %.2f -> %.2f" % [start_yaw, _node.rotation.y])

	# Phase 4: it faces him and walks at the movement type's forward speed, the walk clip playing, then stops at reach.
	var faced := await _until(func() -> bool:
		return absf(wrapf(_node.rotation.y - atan2(-(_feet.x - _node.global_position.x), -(_feet.z - _node.global_position.z)), -PI, PI)) < 0.2, 10.0)
	expect(faced >= 0.0, "it faces the player")
	var walked := {}
	var here := _node.global_position
	var t0 := 0.0
	var p0 := Vector3.ZERO
	var t_now := 0.0
	var speeds: Array[float] = []
	var last := _node.global_position
	while _flat(_node.global_position).distance_to(_flat(_feet)) > 12.0 and t_now < 20.0:
		await _frames(6)
		t_now += STEP * 6.0
		walked[_node.get_playing()] = true
		speeds.append(_flat(_node.global_position).distance_to(_flat(last)) / (STEP * 6.0))
		last = _node.global_position
	# The median of the moments it moves (it stops to shout or turn on the way).
	var moving := speeds.filter(func(s: float) -> bool: return s > 0.5)
	moving.sort()
	var mean: float = moving[moving.size() / 2] if moving.size() > 0 else 0.0
	print("dragon: speeds ", speeds.map(func(s: float) -> float: return snappedf(s, 0.1)))
	print("dragon: walked %.1f m in %.1f s, speed %.2f m/s (want %.2f), clips %s" % [_flat(here).distance_to(_flat(_node.global_position)), t_now, mean, WALK_SPEED, walked.keys()])
	expect(t_now < 20.0, "it reaches the player")
	expect(absf(mean - WALK_SPEED) < WALK_SPEED * 0.3, "it walks about %.2f m/s: %.2f" % [WALK_SPEED, mean])
	expect(walked.has("stance_walk") or walked.has("walk"), "with its walk clip: %s" % [walked.keys()])
	await _frames(120)
	var gap := _flat(_node.global_position).distance_to(_flat(_feet))
	print("dragon: stands %.1f m from the player, playing %s" % [gap, _node.get_playing()])
	expect(gap < 12.0 and gap > 1.0, "it stops within reach: %.1f m" % gap)
	expect(_session.ai.get_combat_state(ref) == 1, "still in combat")
	# Phase 5: in reach and facing him, its own choice is the bite (a bite clip, a HitFrame hit, Health drops).
	var scripted := _session.papyrus.get_animation_event_count()
	_attacks.clear()
	_hits.clear()
	_lost = 0.0
	await _until(func() -> bool: return _attacks.any(func(a: Array) -> bool: return String(a[1]).contains("ground_bite")), 12.0)
	await _frames(150)
	print("dragon: attacks ", _attacks.map(func(a: Array) -> String: return String(a[1]).get_file()), " hits ", _hits.size(), " health lost ", _lost)
	expect(_attacks.any(func(a: Array) -> bool: return String(a[1]).contains("ground_bite")), "it bites the player in front of it: %s" % [_attacks])
	expect(_hits.size() >= 1 and _lost > 20.0, "the bite lands: %d hits, %.1f health lost (bite 25 x 1.5)" % [_hits.size(), _lost])
	expect(_session.papyrus.get_animation_event_count() > scripted, "its DragonBiteEffect reaches dragonActorSCRIPT: %d events delivered" % (_session.papyrus.get_animation_event_count() - scripted))

	# Phases 6 and 7: staged (the AI off), the player at a bearing from the dragon's facing, 3 m away. The attack is
	# chosen by the bearing and the dragon does not turn to him first.
	await _staged_attack(ref, 180.0, "ground_tailwhip.hkx", true)
	await _staged_attack(ref, 90.0, "ground_wingswiperight.hkx", true)
	await _staged_attack(ref, -90.0, "ground_wingswipeleft.hkx", true)
	await _staged_attack(ref, -135.0, "ground_tailwhipleft.hkx", true)
	await _staged_attack(ref, 20.0, "ground_bite", false)

	# Phase 8: the breath (the player 18 m in front of it): the inhale and exhale clips, the concentration spell 0x252C2
	# leaves its head at Voice_SpellFire_Event and lands until shoutStop.
	_session.ai.hold(ref, 8.0)
	_node.face(0.0)
	_feet = _ground(_node.global_position + Vector3(0.0, 0.0, -18.0))
	await _frames(10)
	var seen: Array[String] = []
	_node.animation_event.connect(func(event: String) -> void: seen.append(event))
	var sparks: Array[Array] = []
	_session.magic.npc_spell_landed.connect(func(caster: int, spell: int, target: int, _point: Vector3, _hostile: bool) -> void: sparks.append([caster, spell, target]))
	_lost = 0.0
	_session.npc_shouts.on_shout(ref, PLAYER, 152256, "start")
	await _frames(48)
	_session.npc_shouts.on_shout(ref, PLAYER, 152256, "release")
	await _frames(300)
	var lands := sparks.filter(func(l: Array) -> bool: return l[1] == 0x252C2 and l[2] == PLAYER)
	print("dragon: breath: events ", seen.filter(func(e: String) -> bool: return e in ["Voice_SpellFire_Event", "shoutStop"]), ", landed ", lands.size(), " times, health lost ", snappedf(_lost, 0.1))
	expect(seen.has("Voice_SpellFire_Event"), "the exhale clip raises Voice_SpellFire_Event")
	expect(lands.size() >= 5, "the fire breath lands on the player several times: %d" % lands.size())
	expect(_lost > 12.0 and _lost < 90.0, "about 20 per second for the 2.7 s of the exhale: %.1f health" % _lost)

	# Phase 9: the fireball (a fire-and-forget projectile from its head).
	_lost = 0.0
	sparks.clear()
	_session.ai.hold(ref, 8.0)
	_node.face(0.0)
	_session.npc_shouts.on_shout(ref, PLAYER, 318373, "start")
	await _frames(48)
	_session.npc_shouts.on_shout(ref, PLAYER, 318373, "release")
	await _frames(240)
	var balls := sparks.filter(func(l: Array) -> bool: return l[1] == 475854)
	print("dragon: fireball sparks ", balls.size(), " times, health lost ", snappedf(_lost, 0.1))
	expect(balls.size() >= 1, "the fireball lands: %d" % balls.size())
	expect(_lost > 5.0, "and burns: %.1f health" % _lost)
	# Phase 10: its own choice: breath from range, then, with the player in reach, the melee first.
	_shouts = 0
	_session.npc_shouts.shouted.connect(func(_r: int, _s: int, _w: int, _sp: int) -> void: _shouts += 1)
	_node.face(0.0)
	_feet = _ground(_node.global_position + Vector3(0.0, 0.0, -18.0))
	await _frames(300)  # the hold ends, its voice recovers (5 s)
	_session.ai.start_combat(ref, PLAYER)
	var shouted_after := await _until(func() -> bool: return _shouts > 0, 40.0)
	print("dragon: shouts after %.1f s at 18 m" % shouted_after)
	expect(shouted_after >= 0.0, "from 18 m it breathes (the shout path decides)")
	_feet = _ground(_node.global_position + _node.global_transform.basis * Vector3(0.0, 0.0, -3.0))
	_attacks.clear()
	_shouts = 0
	var struck := await _until(func() -> bool: return not _attacks.is_empty(), 15.0)
	expect(struck >= 0.0 and _shouts == 0, "in reach it fights with bite, wings and tail first: %s, %d shouts" % [_attacks, _shouts])
	print("smoke_dragon_ground: failures=%d" % failures)
	quit(failures)
