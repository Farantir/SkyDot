# SPDX-License-Identifier: GPL-3.0-or-later
#
# Measures what the Game layer's actor lookups cost: enters WhiterunWorld (the market, where many NPCs
# are built), waits for streaming, then keeps about 20 arrows in flight (each arrow asks Combat.segment_hit
# for the actors in its way every physics tick) and prints the actor and node counts, the arrows in
# flight, the physics and process time per tick and the wall time of the initial build.
#
#   godot4.7 --headless --fixed-fps 60 --path game --script res://tools/arrow_load.gd -- \
#       --pack <pack> [--ticks 400] [--arrows 20] [--x 26500 --y 1800] [--settle 600] [--combat N]
extends SceneTree

const PLAYER := 0x14
const IRON_ARROW := 0x1397D
const STEP := 1.0 / 60.0

var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO


func _initialize() -> void:
	var argv := OS.get_cmdline_user_args()
	var args := {}
	for i in range(0, argv.size() - 1):
		args[argv[i]] = argv[i + 1]
	_run(args)


func _frame() -> void:
	_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT
	_session.papyrus.restore_actor_value(PLAYER, "Health", 100000.0)
	_session.step(STEP, _feet)


func _run(args: Dictionary) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = args.get("--pack", OS.get_environment("SKYDOT_SSE_PACK"))
	config.quests = false
	config.preload_doors = false
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: printerr("arrow_load: ", text))
	root.add_child(_session)
	if not _session.open(config, _camera, PlayerBody.new()) or not _session.start_game():
		quit(1)
		return
	_feet = SkydotWorld.skyrim_position(Vector3(float(args.get("--x", 26500.0)), float(args.get("--y", 1800.0)), 0.0))
	var t0 := Time.get_ticks_usec()
	if not _session.enter_world("WhiterunWorld", _feet, null):
		quit(1)
		return
	# Streaming is settled when the cells stop arriving: the actor count holds for 60 frames.
	var last := -1
	var stable := 0
	var frames := 0
	while (stable < 60 or last == 0) and frames < int(args.get("--settle", 600)):
		_frame()
		await process_frame
		frames += 1
		var n := _session.host.find_children("*", "SkydotActor", true, false).size()
		stable = stable + 1 if n == last else 0
		last = n
	print("arrow_load: build+settle %.0f ms (%d frames)" % [(Time.get_ticks_usec() - t0) / 1000.0, frames])
	var space := _camera.get_world_3d().direct_space_state
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(_feet + Vector3.UP * 200.0, _feet - Vector3.UP * 400.0,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN))
	if not hit.is_empty():
		_feet = hit["position"]
	for i in 30:
		_frame()
		await process_frame

	await _start_combat(int(args.get("--combat", 0)))
	var ammo_data := _session.world.get_ammo(IRON_ARROW)
	var proj := _session.world.get_projectile(ammo_data.get("projectile", 0))
	var combat := _session.combat
	var want := int(args.get("--arrows", 20))
	var ticks := int(args.get("--ticks", 400))
	var rng := RandomNumberGenerator.new()
	rng.seed = 1
	var physics: Array[float] = []
	var process: Array[float] = []
	var in_flight := 0
	var last_physics := Engine.get_physics_frames()
	var wall_start := Time.get_ticks_usec()
	for tick in ticks:
		var flying := 0
		for shot: Dictionary in _session.projectiles._live:
			if not shot["stuck"]:
				flying += 1
		while flying < want:
			var yaw := rng.randf() * TAU
			var pitch := deg_to_rad(rng.randf_range(15.0, 50.0))
			var dir := Vector3(sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch))
			var origin := _feet + Vector3.UP * 1.6 + dir * 0.5
			_session.projectiles.shoot(IRON_ARROW, origin, dir, proj, ammo_data,
				func(from: Vector3, to: Vector3) -> Dictionary: return combat.segment_hit(PLAYER, from, to),
				func(_ref: int, _point: Vector3) -> void: pass)
			flying += 1
		in_flight += flying
		_frame()
		await process_frame
		if Engine.get_physics_frames() != last_physics:
			last_physics = Engine.get_physics_frames()
			physics.append(Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0)
			process.append(Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0)
	var wall := (Time.get_ticks_usec() - wall_start) / 1000.0 / ticks
	physics.sort()
	process.sort()
	print("arrow_load: actors %d, nodes %d, arrows in flight avg %.1f" % [
		_session.host.find_children("*", "SkydotActor", true, false).size(),
		int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)), float(in_flight) / ticks])
	print("arrow_load: physics ms mean %.3f p95 %.3f (%d ticks), process ms mean %.3f p95 %.3f" % [
		_mean(physics), physics[int(physics.size() * 0.95)], physics.size(), _mean(process), process[int(process.size() * 0.95)]])
	print("arrow_load: wall ms per frame %.3f" % wall)
	quit(0)


## Make the `count` actors nearest to the player fight the player.
func _start_combat(count: int) -> void:
	if count <= 0:
		return
	var actors: Array = []
	for node in _session.host.find_children("*", "SkydotActor", true, false):
		if node.has_meta("skydot_ref") and node is Node3D:
			actors.append(node)
	actors.sort_custom(func(a: Node3D, b: Node3D) -> bool:
		return a.global_position.distance_squared_to(_feet) < b.global_position.distance_squared_to(_feet))
	var started := 0
	for node: Node3D in actors:
		if started >= count:
			break
		if _session.start_fight(int(node.get_meta("skydot_ref"))):
			started += 1
	print("arrow_load: %d of %d actors in combat with the player" % [started, actors.size()])
	for i in 30:
		_frame()
		await process_frame


func _mean(values: Array[float]) -> float:
	var sum := 0.0
	for v in values:
		sum += v
	return sum / maxf(values.size(), 1)
