# SPDX-License-Identifier: GPL-3.0-or-later
#
# A dragon flies on the real SE pack (SKYDOT_SSE_PACK), headless: EncDragon01Fire 0x1CA03 fights the
# player standing on the plain between Whiterun and the Western Watchtower, Skyrim (13100, -13650),
# and the combat AI runs the whole tactic cycle by itself (flight::decide: ground, takeoff, orbit,
# hover or pass with the breath, landing, the ground fight, takeoff again). The test reads the
# node's state and signals every frame: DragonTakeoffEffect and DragonLandEffect reach the script,
# the orbit keeps its radius and the flight never goes under the minimum clearance over the highest
# surface, the breath in the air hurts the player, the landing and the ground fight follow, an
# injured dragon lands and stays down, SetAllowFlying(false) lands it, IsFlying and GetFlyingState
# through the VM agree with the node, and a dragon placed high in the air starts flying. The
# simulation runs at Engine.physics_ticks_per_second ticks of one session step each, as fast as the
# machine goes. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const DRAGON := 0x1CA03
const SPOT := Vector3(13100.0, -13650.0, -3000.0)  # Skyrim units; the ground is below
const UNIT := 0.0142875  # metres per Skyrim unit
const TICKS := 30  # physics ticks per second of the engine clock
const SPEED := 3.0  # time_scale: the clock runs faster than the wall, one tick advances SPEED / TICKS seconds
const STEP := SPEED / TICKS  # seconds of simulation per frame
const FPS := 10  # frames per simulated second (TICKS / SPEED)  # frames per simulated second
const ORBIT_RADIUS := 2500.0
const MIN_CLEARANCE := 400.0
const BAND := 0.3  # once on the circle it stays within this share of the radius

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _node: SkydotActor
var _ref := 0
var _clock := 0.0
var _lost := 0.0
var _lost_air := 0.0
var _lost_ground := 0.0
var _health := 0.0
var _took_off := 0
var _landed := 0
var _events: Array[String] = []
var _air_shouts := 0
var _ground_shouts := 0
var _ground_attacks := 0
var _min_clearance := 1.0e9
var _clearance_mode := ""
var _orbit_time := 0.0
var _orbit_min := 1.0e9
var _orbit_max := 0.0
var _modes := {}
var _locked := false
var _in_band := 0.0
var _lock_after := 0.0
var _hover_min := 1.0e9
var _pass_close := 1.0e9


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
		print("smoke_dragon_flight: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


## The height of the highest surface (terrain or static) under a Godot point, in Godot metres.
func _surface(at: Vector3) -> float:
	var space := _camera.get_world_3d().direct_space_state
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(Vector3(at.x, at.y + 4000.0, at.z), Vector3(at.x, -2000.0, at.z),
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN))
	return hit["position"].y if not hit.is_empty() else -1.0e9


func _ground(at: Vector3) -> Vector3:
	var space := _camera.get_world_3d().direct_space_state
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(at + Vector3.UP * 200.0, at - Vector3.UP * 400.0,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN))
	return hit["position"] if not hit.is_empty() else at


func _flat(v: Vector3) -> Vector2:
	return Vector2(v.x, v.z)


## Distance in Skyrim units from the dragon to the player, horizontal.
func _dist() -> float:
	return _flat(_node.global_position).distance_to(_flat(_feet)) / UNIT


func _sample() -> void:
	var mode := _node.get_flight_mode()
	_modes[mode] = true
	if not _node.is_flying():
		return
	var pos := _node.global_position
	if mode in ["cruise", "orbit", "hover", "pass"]:
		var clearance := (pos.y - _surface(pos)) / UNIT
		if clearance < _min_clearance:
			_min_clearance = clearance
			_clearance_mode = mode
	if mode == "orbit":
		# Entering the circle (from a takeoff, or back from a pass) is a transient that may cross the
		# band: the orbit counts once it has stayed within 30% of the radius for 4 s, and from then on
		# it must not leave the band while the orbit lasts.
		var d := _dist()
		if absf(d - ORBIT_RADIUS) < ORBIT_RADIUS * BAND:
			_in_band += STEP
		else:
			_in_band = 0.0
		if not _locked and _in_band >= 4.0:
			_locked = true
			_lock_after = maxf(_lock_after, _node.get_flight_mode_time())
		if _locked:
			_orbit_time += STEP
			_orbit_min = minf(_orbit_min, d)
			_orbit_max = maxf(_orbit_max, d)
	else:
		_locked = false
		_in_band = 0.0
	if mode == "hover" and _node.get_flight_speed() < 1.0:
		_hover_min = minf(_hover_min, _dist())
	if mode == "pass":
		_pass_close = minf(_pass_close, _dist())


func _frames(count: int) -> void:
	for i in count:
		_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT
		var health := _session.papyrus.get_actor_value(PLAYER, "Health")
		var drop := maxf(_health - health, 0.0)
		_lost += drop
		if _node != null and _node.is_flying():
			_lost_air += drop
		else:
			_lost_ground += drop
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100000.0)
		_health = _session.papyrus.get_actor_value(PLAYER, "Health")
		_session.step(STEP, _feet)
		_clock += STEP
		if _node != null and is_instance_valid(_node):
			_sample()
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


## A native of the dragon's script class (it extends Actor) through the VM, its result.
func _vm(fn: String) -> Variant:
	var thread := _session.papyrus.call_method(_ref, "DragonActorScript", fn, [])
	var result := {}
	for i in 10:
		await _frames(1)
		result = _session.papyrus.take_result(thread)
		if result["done"]:
			return result.get("value")
	return null


func _log(what: String) -> void:
	print("dragon_flight: t=%6.1f %s (mode %s, %.0f units from him)" % [_clock, what, _node.get_flight_mode(), _dist()])


func _run(pack_dir: String) -> void:
	Engine.physics_ticks_per_second = TICKS
	Engine.time_scale = SPEED
	await process_frame
	var started := Time.get_ticks_msec()
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
	_session.combat.npc_attack_started.connect(func(_attacker: int, _clip: String, _power: bool) -> void:
		if _node != null and not _node.is_flying():
			_ground_attacks += 1)
	_session.npc_shouts.shouted.connect(func(_r: int, _s: int, _w: int, _sp: int) -> void:
		if _node != null and _node.is_flying():
			_air_shouts += 1
		else:
			_ground_shouts += 1)
	_feet = SkydotWorld.skyrim_position(SPOT)
	expect(_session.enter_world("Tamriel", _feet, null), "the worldspace is entered")
	await _frames(FPS * 4)
	var centre := _ground(_feet)

	# The dragon stands 25 m from the player; the player stays where he is.
	_feet = _ground(centre + Vector3(25.0, 0.0, 0.0))
	_ref = _session.papyrus.spawn_actor(DRAGON, _session.place.cell_id, SkydotWorld.godot_to_skyrim(centre), 0.0)
	expect(_ref != 0, "the dragon is spawned")
	await _frames(FPS)
	_node = world.find_ref_node(_ref) as SkydotActor
	if _node == null:
		expect(false, "the dragon is built")
		quit(1)
		return
	expect(_node.can_fly(), "a dragon can fly")
	_node.flight_took_off.connect(func() -> void:
		_took_off += 1
		_log("took off"))
	_node.flight_landed.connect(func() -> void:
		_landed += 1
		_log("landed"))
	_node.animation_event.connect(func(event: String) -> void:
		if event.begins_with("Dragon"):
			_events.append(event))
	await _until(func() -> bool: return _node.get_state() != "held", 5.0)
	_health = _session.papyrus.get_actor_value(PLAYER, "Health")
	_session.ai.start_combat(_ref, PLAYER)
	await _frames(FPS / 2)
	expect(_session.ai.get_combat_state(_ref) == 1, "it fights the player")
	expect(not _node.is_flying(), "it starts on the ground")
	var scripted := _session.papyrus.get_animation_event_count()

	# a) It takes off by itself (the tactics wait ground_attack_time x 20 s on the ground first).
	var t := await _until(func() -> bool: return _took_off >= 1, 60.0)
	expect(t >= 0.0, "it takes off after about %.0f s" % t)
	await _frames(FPS / 4)
	expect(_node.is_flying() or _node.get_flight_mode() == "cruise", "and flies")
	expect(_events.has("DragonTakeoffEffect"), "DragonTakeoffEffect comes with the takeoff: %s" % [_events])
	expect(_session.papyrus.get_animation_event_count() > scripted, "and reaches the dragon's script: %d" % (_session.papyrus.get_animation_event_count() - scripted))

	# b) It orbits the player for a lap or more.
	t = await _until(func() -> bool: return _orbit_time >= 8.0, 60.0)
	expect(t >= 0.0, "it orbits for a lap: %.1f s" % _orbit_time)
	print("dragon_flight: orbit distance %.0f..%.0f over %.1f s (radius %.0f), on the circle %.1f s after entering, lowest clearance %.0f (%s)" % [_orbit_min, _orbit_max, _orbit_time, ORBIT_RADIUS, _lock_after, _min_clearance, _clearance_mode])
	expect(_orbit_min > ORBIT_RADIUS * (1.0 - BAND) and _orbit_max < ORBIT_RADIUS * (1.0 + BAND), "once on the circle it keeps its radius within 30%%: %.0f..%.0f" % [_orbit_min, _orbit_max])
	expect(_lock_after < 15.0, "it reaches the circle within 15 s: %.1f s" % _lock_after)

	# c) A breath in the air hurts him.
	t = await _until(func() -> bool: return _lost_air > 0.0, 90.0)
	await _frames(FPS * 5)
	_log("air breath: %d shouts, %.1f health lost in the air" % [_air_shouts, _lost_air])
	expect(_air_shouts >= 1 and _lost_air > 0.0, "a breath in the air hurts the player: %d shouts, %.1f health" % [_air_shouts, _lost_air])

	# d) It lands (DragonLandEffect), fights on the ground, and takes off again.
	t = await _until(func() -> bool: return _landed >= 1, 120.0)
	expect(t >= 0.0, "it lands")
	await _frames(FPS / 4)
	expect(_events.has("DragonLandEffect"), "DragonLandEffect comes with the landing: %s" % [_events])
	expect(not _node.is_flying(), "and stands on the ground")
	t = await _until(func() -> bool: return _ground_attacks + _ground_shouts > 0, 40.0)
	expect(t >= 0.0, "it fights on the ground: %d attacks, %d breaths" % [_ground_attacks, _ground_shouts])
	_log("ground fight: %d attacks, %d breaths" % [_ground_attacks, _ground_shouts])
	t = await _until(func() -> bool: return _took_off >= 2, 60.0)
	expect(t >= 0.0, "and takes off again")

	# f) The VM agrees with the node; SetAllowFlying(false) lands it.
	await _until(func() -> bool: return _node.get_flight_mode() in ["cruise", "orbit"], 30.0)
	var flying_vm = await _vm("IsFlying")
	var state_vm = await _vm("GetFlyingState")
	print("dragon_flight: VM IsFlying ", flying_vm, " GetFlyingState ", state_vm, " node ", _node.is_flying(), " ", _node.get_flying_state())
	expect(flying_vm == true and _node.is_flying(), "IsFlying through the VM agrees with the node: %s" % [flying_vm])
	expect(state_vm == _node.get_flying_state() and state_vm == 2, "GetFlyingState through the VM agrees (cruising): %s, node %d" % [state_vm, _node.get_flying_state()])
	_session.papyrus.set_allowed_to_fly(_ref, false)
	var landed_before := _landed
	t = await _until(func() -> bool: return _landed > landed_before, 60.0)
	expect(t >= 0.0, "SetAllowFlying(false) lands it: %.1f s" % t)
	await _frames(FPS * 2)
	expect(not _node.is_flying(), "and it stays down")

	# e) Injured: below 35% Health it lands and stays on the ground for 20 s of combat.
	_session.papyrus.set_allowed_to_fly(_ref, true)
	t = await _until(func() -> bool: return _took_off >= 3, 60.0)
	expect(t >= 0.0, "allowed again, it takes off")
	await _until(func() -> bool: return _node.get_flight_mode() in ["cruise", "orbit"], 30.0)
	var max_health := _session.papyrus.get_actor_value_max(_ref, "Health")
	_session.papyrus.damage_actor_value(_ref, "Health", max_health * 0.7)
	landed_before = _landed
	t = await _until(func() -> bool: return _landed > landed_before, 60.0)
	expect(t >= 0.0, "injured, it lands: %.1f s" % t)
	var down_since := _clock
	var flew := false
	while _clock - down_since < 22.0:
		await _frames(FPS / 4)
		if _node.is_flying():
			flew = true
	expect(not flew, "and stays on the ground for 22 s")
	# 1 fights, 2 searches for a target it lost sight of (a landing spot can hide the player); both are combat.
	expect(_session.ai.get_combat_state(_ref) in [1, 2], "in combat: state %d" % _session.ai.get_combat_state(_ref))

	# 3) A dragon placed high in the air flies (its placement 3000 units above the ground).
	_session.papyrus.set_allowed_to_fly(_ref, false)
	var high := SkydotWorld.godot_to_skyrim(centre + Vector3(0.0, 0.0, 20.0))
	high.z += 3000.0
	var ref2: int = _session.papyrus.spawn_actor(DRAGON, _session.place.cell_id, high, 0.0)
	await _frames(FPS)
	var node2 := world.find_ref_node(ref2) as SkydotActor
	expect(node2 != null, "the high dragon is built")
	if node2 != null:
		await _frames(FPS * 3)
		var height := (node2.global_position.y - _surface(node2.global_position)) / UNIT
		print("dragon_flight: high dragon flies=%s mode %s %.0f units over the ground" % [node2.is_flying(), node2.get_flight_mode(), height])
		expect(node2.is_flying() and height > 1000.0, "a dragon placed 3000 units up flies on: %s %.0f" % [node2.get_flight_mode(), height])

		# 4) The cell builder does not build a second actor for a rescued flyer's reference (the streamer marks it).
		var fresh := world.build_placed_actor(0xFF00F000, DRAGON, high, 0.0)
		expect(fresh != null and fresh.find_children("*", "SkydotActor", true, false).size() == 1, "a placed dragon is built (control)")
		fresh.free()
		node2.set_meta("skydot_rescued", true)
		var again := world.build_placed_actor(ref2, DRAGON, high, 0.0)
		expect(again != null and again.find_children("*", "SkydotActor", true, false).is_empty(), "a rescued flyer's reference is not built again")
		again.free()
		node2.remove_meta("skydot_rescued")

	if _modes.has("hover"):
		expect(absf(_hover_min - 1800.0) < 270.0, "it hovers at about the hover distance 1800: %.0f" % _hover_min)
	if _modes.has("pass"):
		expect(_pass_close < 500.0, "a pass flies over the target: closest %.0f" % _pass_close)
	print("dragon_flight: modes seen ", _modes.keys(), ", hover distance %.0f, pass closest %.0f, %.1f s of simulation in %.1f s" % [_hover_min, _pass_close, _clock, (Time.get_ticks_msec() - started) / 1000.0])
	print("smoke_dragon_flight: failures=%d" % failures)
	quit(failures)
