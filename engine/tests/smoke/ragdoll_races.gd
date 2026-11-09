# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game's ragdoll on every race of the comparison (human, wolf, draugr, Frostbite Spider, dragon) on
# the real SE pack (SKYDOT_SSE_PACK), headless, set up as the game measurements were
# (skycompare-data/game-ragdoll: SkyrimRemote @ragdoll, 2026-11-08): the actor at the game's place and
# facing, killed with no push, left until its ragdoll rests. Then: one body per data body, every joint
# within its limits (plus Ragdoll.CONE_GAP_DEGREES), no body inside another the game lets it collide
# with by more than its radius, and the engine's skeleton against the NIF bind (printed). Each rest
# pose is written in SkyrimRemote's shape (RAGDOLL_DUMP_DIR, default user://ragdoll) and compared with
# the game's rest dump (RAGDOLL_GAME_DIR, default ../skycompare-data/game-ragdoll beside the repo) when
# it is there. RAGDOLL_ONLY=<name> runs one. Skips without the pack.
extends SceneTree

const Checks := preload("ragdoll_checks.gd")
const STEP := 1.0 / 60.0
## name, place (cell editor ID or "Tamriel"), placed ref or 0, base to spawn, Skyrim position, facing
## (degrees), the game's rest dump, seconds to wait for rest at most.
const RACES := [
	["human", "WhiterunDragonsreach", 0, 0x1347F, Vector3(-448, 300, -318), 90.0, "a_human_rest.json", 20.0],
	["wolf", "WhiterunDragonsreach", 0, 0x23ABE, Vector3(-448, 300, -318), 90.0, "a_wolf_rest.json", 20.0],
	["draugr", "WhiterunDragonsreach", 0, 0x1FB8F, Vector3(-452.21, 289.83, -318), 90.0, "a_draugr_rest.json", 20.0],
	["spider", "BleakFallsBarrow01", 0x3A1E1, 0, Vector3(3246.6, -3356, -621.76), 288.43, "a_spider_rest.json", 25.0],
	["dragon", "Tamriel", 0, 0x1CA03, Vector3(25560, -21217, -3598), 90.0, "a_dragon_rest.json", 40.0],
]

## Where the game's pushes were measured (c_*_f*.result.json): the player at PUSH_FROM on the Whiterun
## plain (Tamriel), the actor moved 300 units ahead of it, at PUSH_AT.
const PUSH_FROM := Vector3(11690, -13844, -4700)
const PUSH_AT := Vector3(11896, -13823, -4793)

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_ragdoll_races: SKIP (SKYDOT_SSE_PACK unset)")
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


func _open_session(pack_dir: String) -> bool:
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
	return _session.open(config, _camera, PlayerBody.new()) and _session.start_game()


func _game_dir() -> String:
	var dir := OS.get_environment("RAGDOLL_GAME_DIR")
	if dir == "":
		dir = ProjectSettings.globalize_path("res://").path_join("../../../skycompare-data/game-ragdoll").simplify_path()
	return dir


func _run(pack_dir: String) -> void:
	await process_frame
	expect(_open_session(pack_dir), "the session opens")
	var only := OS.get_environment("RAGDOLL_ONLY")
	var out_dir := OS.get_environment("RAGDOLL_DUMP_DIR")
	if out_dir == "":
		out_dir = ProjectSettings.globalize_path("user://ragdoll")
	DirAccess.make_dir_recursive_absolute(out_dir)
	for race: Array in RACES:
		if only == "" or only == race[0]:
			await _race(race, out_dir)
	print("smoke_ragdoll_races: failures=%d" % failures)
	quit(failures)


func _race(race: Array, out_dir: String) -> void:
	var name: String = race[0]
	var push := float(OS.get_environment("RAGDOLL_PUSH")) if OS.has_environment("RAGDOLL_PUSH") else 0.0
	if push > 0.0:
		race = race.duplicate()
		race[1] = "Tamriel"
		race[2] = 0
		race[4] = PUSH_AT
		if race[3] == 0:
			return  # a placed actor (the spider) is not moved
	var at: Vector3 = race[4]
	_feet = SkydotWorld.skyrim_position(PUSH_FROM if push > 0.0 else at + Vector3(0, 300, 0))
	_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT  # an exterior streams around the camera
	var entered := _session.enter_world("Tamriel", _feet, null) if race[1] == "Tamriel" \
		else _session.enter_cell(race[1], _feet, null)
	expect(entered, "%s: %s is entered" % [name, race[1]])
	await _frames(120)
	# Wait for the ground under the place (an exterior's terrain collision comes with its cell).
	var floor_hit := {}
	var ground := func() -> bool:
		var from := SkydotWorld.skyrim_position(at)
		var query := PhysicsRayQueryParameters3D.create(from + Vector3.UP * 200.0, from + Vector3.DOWN * 200.0,
			SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
		floor_hit = _camera.get_world_3d().direct_space_state.intersect_ray(query)
		return not floor_hit.is_empty()
	expect(await _until(ground, 60.0) >= 0.0, "%s: there is ground under the place" % name)
	if race[2] == 0 and not floor_hit.is_empty():
		var floor_at := SkydotWorld.godot_to_skyrim(floor_hit["position"])
		if absf(floor_at.z - at.z) > 50.0:
			print("%s: the floor under the game's place is at %.0f, not %.0f: spawned on it" % [name, floor_at.z, at.z])
			at.z = floor_at.z
	var ref: int = race[2]
	if ref == 0:
		ref = _session.papyrus.spawn_actor(race[3], _session.place.cell_id, at, deg_to_rad(race[5]))
		if race[0] == "dragon":
			_session.papyrus.set_allowed_to_fly(ref, false)
	var built := await _until(func() -> bool: return _session.world.find_ref_node(ref) != null, 30.0)
	var node := _session.world.find_ref_node(ref) as SkydotActor
	expect(built >= 0.0 and node != null, "%s: 0x%X is built" % [name, ref])
	if node == null:
		return
	await _frames(60)
	var data := _session.world.get_ragdoll(ref) if race[2] != 0 else _session.world.get_ragdoll(ref, race[3])
	var bodies: Array = data.get("bodies", [])
	expect(not bodies.is_empty(), "%s has ragdoll data" % name)
	var skeleton := node.find_children("*", "Skeleton3D", true, false).front() as Skeleton3D
	var bind := Checks.bind_against_rest(skeleton, data)
	print("%s: engine skeleton against the NIF bind: %.2f cm (%s), %.1f deg" % [name, bind["cm"], bind["at"], bind["degrees"]])

	if push > 0.0:
		await _push(name, ref, node, push)
		return
	_session.kill_actor(ref)
	var counted := -1
	var started := await _until(func() -> bool: return node.find_child("Ragdoll", true, false) != null, 15.0)
	expect(started >= 0.0, "%s: a ragdoll runs" % name)
	var ragdoll := node.find_child("Ragdoll", true, false)
	if ragdoll != null:
		counted = ragdoll.get_child_count()
	var expected := 0
	for b: Dictionary in bodies:
		if skeleton.find_bone(String(b["bone"])) >= 0:
			expected += 1
	expect(counted == expected and expected == bodies.size(), "%s: one body per data body: %d made, %d data, %d on bones" % [name, counted, bodies.size(), expected])
	# The centre of mass while it falls: when it last moved more than 2 cm in 0.1 s (the game's "COM
	# still after").
	var centres: Array[Vector3] = []
	var follow := func() -> bool:
		var body := _session.corpses._body_of(ref)
		if not body.is_empty():
			centres.append((body["ragdoll"] as Ragdoll).centre())
		return node.find_child("Ragdoll", true, false) == null
	var rested := await _until(follow, race[7])
	print("%s: rested after %.1f s" % [name, rested + maxf(started, 0.0)])
	var moving_until := 0.0
	for i in range(6, centres.size()):
		if centres[i].distance_to(centres[i - 6]) > 0.02:
			moving_until = i * STEP
	print("%s: the centre of mass moved until %.1f s" % [name, moving_until + maxf(started, 0.0)])
	expect(rested >= 0.0, "%s: the ragdoll rests within %.0f s" % [name, race[7]])

	var limits := Checks.limits(skeleton, data)
	print("%s: %d joints, the worst %.1f deg past its limits (%s)" % [name, limits["checked"], limits["worst"], limits["at"]])
	expect(limits["worst"] <= Ragdoll.CONE_GAP_DEGREES + 5.0, "%s: every joint within its limits and the measured gap: %.1f deg at %s" % [name, limits["worst"], limits["at"]])
	var overlaps := Checks.overlaps(skeleton, data)
	print("%s: deepest overlap of colliding bodies past the radius %.3f m (%s)" % [name, overlaps["worst"], overlaps["pair"]])
	expect(overlaps["worst"] <= 0.0, "%s: no body inside another it collides with by more than its radius: %.3f m %s" % [name, overlaps["worst"], overlaps["pair"]])

	var dump := Checks.dump(ref, node, skeleton, data)
	var file := FileAccess.open(out_dir.path_join("skydot_%s_rest.json" % name), FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(dump, " "))
		file.close()
	var game_path := _game_dir().path_join(race[6])
	if FileAccess.file_exists(game_path):
		var game: Variant = JSON.parse_string(FileAccess.get_file_as_string(game_path))
		if game is Dictionary:
			var frame: Dictionary = game["frames"][-1] if game.has("frames") else game
			var c := Checks.compare(dump, frame, at.z)
			print("%s against the game: %d bones, position error median %.0f max %.0f units; centre of mass %.0f vs %.0f units up, top %.0f vs %.0f, extents %s vs %s"
				% [name, c["matched"], c["median"], c["max"], c["ours"]["com_height"], c["game"]["com_height"],
					c["ours"]["top"], c["game"]["top"], c["ours"]["extent"].round(), c["game"]["extent"].round()])


## RAGDOLL_PUSH=<force>: the player (300 units away) pushes the actor (PushActorAway) instead; the
## root body is followed until the actor gets up: its first velocity, peak above where it stood,
## touchdown and travel (the game's c_<race>_f<force> measurements).
func _push(name: String, ref: int, node: SkydotActor, force: float) -> void:
	var start := node.global_position
	expect(_session.corpses.knock_down(ref, SkydotPapyrus.PLAYER_REF, force), "%s is knocked down" % name)
	var samples: Array[Vector3] = []
	var body: Dictionary = {}
	for b in _session.corpses._bodies:
		if b["ref"] == ref:
			body = b
	var rise := 0.0
	while _session.corpses.is_down(ref) and samples.size() < 900:
		var root: PhysicalBone3D = body["bones"][0]
		samples.append(root.global_position)
		await _frames(1)
	var peak := 0.0
	var touchdown := -1.0
	var standing := samples[0].y - start.y if not samples.is_empty() else 0.0
	for i in samples.size():
		var up := samples[i].y - start.y
		peak = maxf(peak, up - standing)
		if touchdown < 0.0 and i > 3 and up < standing * 0.5:
			touchdown = i * STEP
	var first := (samples[2] - samples[0]) / (2.0 * STEP) if samples.size() > 2 else Vector3.ZERO
	var travel := Vector2(samples.back().x - start.x, samples.back().z - start.z).length() if not samples.is_empty() else 0.0
	print("%s pushed with %.0f: root body first %.2f m/s along, %.2f up; peak +%.2f m; down to half its height at %.2f s; travel %.1f m; up after %.1f s"
		% [name, force, Vector2(first.x, first.z).length(), first.y, peak, touchdown, travel, samples.size() * STEP])
