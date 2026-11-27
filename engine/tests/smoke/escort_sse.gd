# SPDX-License-Identifier: GPL-3.0-or-later
#
# Escort on the real SE pack (SKYDOT_SSE_PACK): DA16ErandurEscortToTemple (wait distance 512) pushed on
# Erandur, placed 1500 units south of its destination outside Nightcaller Temple. The player keeps behind him
# on his trail: he leads on; four times the player falls behind: he stops and waits; the player catches
# up: he goes on (a wait is not one of the walks after which a spot counts as reached); at the
# destination the escort ends. Headless; skips without the pack.
extends SceneTree

const ESCORT := 0x35068  # DA16ErandurEscortToTemple
const DESTINATION := 0x24301
const OFFSET := Vector3(0, -1500, 0)  # south of it; from the east the walk sticks between ice walls
const WAIT := 512.0
const ROUNDS := 4
const LAG := 1.5  # seconds the player is behind him on his trail

var failures := 0
var _ai: SkydotAi
var _papyrus: SkydotPapyrus
var _actor: Node3D
var _ref := 0
var _player := Vector3()
var _trail: Array[Vector3] = []
var _way: Array[Vector3] = []  # every frame's position since the escort began


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_escort_sse: SKIP (SKYDOT_SSE_PACK unset)")
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


func _escorting() -> bool:
	return _ai.get_actor_state(_ref).get("procedure", "") == "escort"


# One frame; a following player is where he was LAG seconds ago.
func _frame(following: bool) -> void:
	_trail.append(_at())
	_way.append(_at())
	if _trail.size() > int(LAG * 60.0):
		_trail.pop_front()
	if following:
		_player = _trail[0]
	_ai.set_player(_player, true)
	_ai.update(1.0 / 60.0)
	_papyrus.update(1.0 / 60.0)
	await physics_frame


func _run(pack_dir: String) -> void:
	await process_frame
	var pack := SkydotPack.new()
	if pack.open(pack_dir) != OK:
		printerr("smoke_escort_sse: ", pack.get_error())
		quit(1)
		return
	var world := pack.open_world()
	_papyrus = SkydotPapyrus.new()
	_papyrus.setup(pack, world)
	_papyrus.start_game_enabled_quests()
	_ai = SkydotAi.new()
	_ai.setup(world, _papyrus)
	_ai.set_combat(false)
	_ai.days = 12.0 / 24.0
	_ai.time_scale = 1.0
	var space := world.find_world("Tamriel")
	_ai.set_space(space)
	_ai.place_actors()
	_ref = world.find_actor_of(world.find_npc("Erandur"))
	expect(_ref != 0, "Erandur is found")
	var goal: Vector3 = world.get_ref_info(world.get_ref_cell(DESTINATION), DESTINATION).get("position", Vector3())
	var start: Vector3 = world.nearest_nav_point(space, goal + OFFSET, 600.0)
	expect(_flat(start, goal) > 900.0, "there is a start %.0f units off" % _flat(start, goal))
	world.set_actor_place(_ref, space, start, 0.0)
	var root3d := Node3D.new()
	root.add_child(root3d)
	var gx := floori(start.x / SkydotWorld.CELL_UNITS)
	var gy := floori(start.y / SkydotWorld.CELL_UNITS)
	for dx in range(-1, 2):
		for dy in range(-1, 2):
			var cell := world.build_exterior(space, gx + dx, gy + dy)
			if cell != null:
				root3d.add_child(cell)
	_ai.attach_built(root3d)
	for actor in root3d.find_children("*", "SkydotActor", true, false):
		if actor.get_meta("skydot_ref", 0) == _ref:
			_actor = actor
	expect(_actor != null, "Erandur 0x%X is built" % _ref)
	if _actor == null:
		quit(1)
		return
	for i in 30:
		await _frame(false)
	_player = _at()
	_ai.push_package(_ref, ESCORT)
	expect(_escorting(), "he escorts (%s)" % _ai.get_actor_state(_ref))
	var start_left := _flat(_at(), goal)
	# Four times (more than the walks after which a spot counts as reached): followed, he leads on;
	# the player drops back beyond the wait distance (to a point of his way that far behind), he stops
	# and waits; the player catches up, he goes on.
	var waits := 0
	var resumed := 0
	for round in ROUNDS:
		for i in (360 if round == 0 else 90):
			await _frame(true)
		if round == 0:
			expect(_escorting() and _actor.is_walking(), "followed, he leads on (%s)" % _actor.get_state())
		_player = _way[0]
		for k in range(_way.size() - 1, -1, -1):
			if _flat(_way[k], _at()) > WAIT + 100.0:
				_player = _way[k]
				break
		for i in 60 * 2:
			await _frame(false)
		var waited_at := _at()
		for i in 60:
			await _frame(false)
		if _escorting() and not _actor.is_walking() and _at().distance_to(waited_at) < 8.0:
			waits += 1
		else:
			printerr("  round %d: %.0f units behind, %s, moved %.1f" % [round, _flat(_at(), _player), _actor.get_state(), _at().distance_to(waited_at)])
		_trail.clear()
		_player = _at()
		for i in 60:
			await _frame(true)
		if _at().distance_to(waited_at) > 32.0:
			resumed += 1
	expect(waits == ROUNDS, "left behind, he stops and waits (%d of %d times)" % [waits, ROUNDS])
	expect(resumed == ROUNDS, "caught up with, he goes on (%d of %d times)" % [resumed, ROUNDS])
	# Followed to the end, the escort ends at the destination.
	var elapsed := 0.0
	while _escorting() and elapsed < 240.0:
		await _frame(true)
		elapsed += 1.0 / 60.0
	var state := _ai.get_actor_state(_ref)
	expect(not _escorting(), "the escort ends (%.0f s; %s step %s of %s)" % [elapsed, state.get("procedure"),
		state.get("step"), state.get("steps")])
	expect(_flat(_at(), goal) < 200.0, "he is at the destination (%.0f units off, from %.0f)" % [_flat(_at(), goal), start_left])
	print("smoke_escort_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)
