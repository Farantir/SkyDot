# SPDX-License-Identifier: GPL-3.0-or-later
#
# Follow on the real SE pack (SKYDOT_SSE_PACK): DefaultFollowPlayer (min 128, max 256) pushed on Faendal in
# Riverwood. He comes up to the player and stops within the max distance, does not crowd in closer than
# about the min, and after the player walks 1000 units away he catches up and stops again. Headless; skips
# without the pack.
extends SceneTree

const FAENDAL := 0x1348C
const FOLLOW := 0x4B037  # DefaultFollowPlayer
const NEAR := 128.0
const FAR := 256.0

var failures := 0
var _ai: SkydotAi
var _papyrus: SkydotPapyrus
var _world: SkydotWorld
var _space := 0
var _actor: Node3D
var _player := Vector3()


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_follow_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


func _at() -> Vector3:
	return SkydotWorld.godot_to_skyrim(_actor.global_position)


func _flat(a: Vector3, b: Vector3) -> float:
	return Vector2(a.x, a.y).distance_to(Vector2(b.x, b.y))


func _frames(count: int) -> void:
	for i in count:
		_ai.set_player(_player, true)
		_ai.update(1.0 / 60.0)
		_papyrus.update(1.0 / 60.0)
		await physics_frame


# Runs until he has stood still for a second (or the time is up); returns the seconds it took.
func _settle(limit: float) -> float:
	var still := 0.0
	var elapsed := 0.0
	var last := _at()
	while elapsed < limit and still < 1.0:
		await _frames(1)
		elapsed += 1.0 / 60.0
		still = still + 1.0 / 60.0 if _at().distance_to(last) < 0.05 else 0.0
		last = _at()
	return elapsed


func _run(pack_dir: String) -> void:
	await process_frame
	var pack := SkydotPack.new()
	if pack.open(pack_dir) != OK:
		printerr("smoke_follow_sse: ", pack.get_error())
		quit(1)
		return
	_world = pack.open_world()
	_papyrus = SkydotPapyrus.new()
	_papyrus.setup(pack, _world)
	_papyrus.start_game_enabled_quests()
	_ai = SkydotAi.new()
	_ai.setup(_world, _papyrus)
	_ai.set_combat(false)
	_ai.days = 10.0 / 24.0
	_ai.time_scale = 1.0
	_space = _world.find_world("Tamriel")
	_ai.set_space(_space)
	_ai.place_actors()
	var place: Dictionary = _world.get_actor_place(FAENDAL)
	expect(place.get("space", 0) == _space, "Faendal is placed outside (%s)" % place)
	var start: Vector3 = place.get("position", Vector3())
	var root3d := Node3D.new()
	root.add_child(root3d)
	var gx := floori(start.x / SkydotWorld.CELL_UNITS)
	var gy := floori(start.y / SkydotWorld.CELL_UNITS)
	for dx in range(-1, 2):
		for dy in range(-1, 2):
			var cell := _world.build_exterior(_space, gx + dx, gy + dy)
			if cell != null:
				root3d.add_child(cell)
	_ai.attach_built(root3d)
	for actor in root3d.find_children("*", "SkydotActor", true, false):
		if actor.get_meta("skydot_ref", 0) == FAENDAL:
			_actor = actor
	expect(_actor != null, "Faendal 0x%X is built" % FAENDAL)
	if _actor == null:
		quit(1)
		return
	await _frames(30)
	# The player stands 600 units off on the navmesh.
	_player = _world.nearest_nav_point(_space, _at() + Vector3(600, 0, 0), 400.0)
	_ai.push_package(FAENDAL, FOLLOW)
	var state := _ai.get_actor_state(FAENDAL)
	expect(state.get("procedure", "") == "follow", "he follows (%s, %s)" % [state.get("package_editor_id"), state.get("procedure")])
	var took := await _settle(30.0)
	var d := _flat(_at(), _player)
	expect(d <= FAR + 16.0 and d >= NEAR - 48.0, "he comes up and stops %.0f units away (%.1f s)" % [d, took])
	# Standing near the player, he keeps still across thinks.
	var held := _at()
	await _frames(60 * 5)
	expect(_at().distance_to(held) < 8.0, "he keeps still beside the standing player (%.1f units)" % _at().distance_to(held))
	# The player runs (240 units/s) 1000 units away along the navmesh; he runs to catch up and stops
	# within the max distance again.
	var from := _player
	var to: Vector3 = _world.nearest_nav_point(_space, from + (from - _at()).normalized() * 1000.0, 400.0)
	expect(_flat(from, to) > 800.0, "the player has somewhere to go (%.0f units)" % _flat(from, to))
	var map: RID = _actor.get_world_3d().navigation_map
	var route := NavigationServer3D.map_get_path(map, SkydotWorld.skyrim_position(from), SkydotWorld.skyrim_position(to), true)
	var trail: Array[Vector3] = [from]
	for p in route:
		trail.append(SkydotWorld.godot_to_skyrim(p))
	var farthest := 0.0
	var ran := false
	for k in range(1, trail.size()):
		var a: Vector3 = trail[k - 1]
		var leg := maxi(1, int(a.distance_to(trail[k]) / 4.0))
		for i in leg:
			_player = a.lerp(trail[k], float(i + 1) / leg)
			await _frames(1)
			farthest = maxf(farthest, _flat(_at(), _player))
			ran = ran or _actor.get_state() == "run"
	took = await _settle(30.0)
	d = _flat(_at(), _player)
	expect(ran, "he runs to catch up")
	expect(farthest < 600.0, "he keeps up (at most %.0f units behind)" % farthest)
	expect(d <= FAR + 16.0 and d >= NEAR - 48.0, "after the player walked away he stops %.0f units off again (%.1f s)" % [d, took])
	state = _ai.get_actor_state(FAENDAL)
	expect(state.get("procedure", "") == "follow", "he still follows (%s)" % state.get("procedure"))
	_ai.pop_package(FAENDAL, FOLLOW)
	state = _ai.get_actor_state(FAENDAL)
	expect(state.get("package", 0) != FOLLOW, "popped, his own package runs (%s)" % state.get("package_editor_id"))
	print("smoke_follow_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)
