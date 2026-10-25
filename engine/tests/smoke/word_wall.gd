# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Bleak Falls Barrow word wall through GameSession on the real SE pack (SKYDOT_SSE_PACK), headless,
# 7b-4: no engine special case, the vanilla scripts run. The trigger 0x709D2 carries WordWallTriggerScript
# and WordWallTriggerBleakFallsScript (the reference removes the base's WordWallTrigger02Script); the
# wall 0x709D1 is animated by its behaviour graph, whose events are the names the scripts send (Dark1Wild,
# Dark2Wild, Learned...). In the trigger with the back to the wall's look target (0x709D4) Actor.HasLOS is
# false: the wall only glows (Dark1State) and nothing is taught; turned to the target between the inner
# and the middle radius HasLOS is true and the wall goes to Dark3State, within the inner radius Game.TeachWord
# tells "Word of Power Learned: Fus", the wall lights (LightState) and WordFus 0x13E22 is known and not
# unlocked; the quest stage the trigger names is set. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const WALL := 0x709D1
const TRIGGER := 0x709D2
const LOOK_TARGET := 0x709D4
const UNRELENTING_FORCE := 0x13E07
const WORD_FUS := 0x13E22
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _messages: Array[String] = []
var _events: Array[String] = []  # animation events the scripts sent the wall
var _states: Array[String] = []  # the wall's graph states in the order they were entered


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
		print("smoke_word_wall: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _frames(count: int) -> void:
	var graph := _graph()
	for i in count:
		_session.step(STEP, _feet)
		await physics_frame
		if graph != null:
			var state: String = graph.get_state()
			if state != "" and (_states.is_empty() or _states[-1] != state):
				_states.append(state)


## Whether the wall's graph entered `state` (it names its machines: "BlendState00,Dark1State").
func _entered(state: String) -> bool:
	return _states.any(func(s: String) -> bool: return s.ends_with(state))


func _graph() -> Node:
	var wall := _session.world.find_ref_node(WALL)
	return wall.find_child("SkydotBehavior", true, false) if wall != null else null


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


## The player stands about `distance` metres from the look target, on a floor with a clear line from the
## eyes to it, and looks along `yaw_offset` from it (0: at it, PI: away).
func _stand_near(target: Vector3, distance: float, yaw_offset: float) -> void:
	var space := _camera.get_world_3d().direct_space_state
	for scale in [1.0, 0.75, 0.5, 1.25]:
		for step in 16:
			var dir := Vector3.FORWARD.rotated(Vector3.UP, step * TAU / 16.0)
			var above: Vector3 = target + dir * distance * scale
			var ground := space.intersect_ray(PhysicsRayQueryParameters3D.create(above, above - Vector3.UP * 4.0, 1 | 2 | 8))
			if ground.is_empty():
				continue
			var feet: Vector3 = ground["position"]
			var eye := feet + Vector3.UP * GameSession.EYE_HEIGHT
			if space.intersect_ray(PhysicsRayQueryParameters3D.create(eye, target, 1 | 2 | 8)).is_empty():
				_feet = feet
				_camera.global_position = eye
				_camera.look_at(Vector3(target.x, eye.y, target.z), Vector3.UP)
				_camera.rotate_y(yaw_offset)
				return
	expect(false, "no clear place to stand")


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.quests = false
	config.preload_doors = false
	config.wander = false
	config.combat = false
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
	var world := _session.world
	var papyrus := _session.papyrus
	var cell := world.find_cell("BleakFallsBarrow02")
	var target_info := world.get_ref_info(cell, LOOK_TARGET)
	var trigger_info := world.get_ref_info(cell, TRIGGER)
	expect(not target_info.is_empty() and not trigger_info.is_empty(), "the look target and the trigger are placed")
	var target_at: Vector3 = target_info["position"]
	# Far from the trigger first: it is entered by walking in.
	expect(_session.enter_cell("BleakFallsBarrow02", SkydotWorld.skyrim_position(target_at + Vector3(0, 0, 300)), null),
		"the interior is entered")
	_session.message.connect(func(text: String, _box: bool) -> void: _messages.append(text))
	papyrus.play_animation.connect(func(ref: int, event: String) -> void:
		if ref == WALL:
			_events.append(event))
	await _until(func() -> bool: return world.find_ref_node(WALL) != null, 20.0)
	await _frames(60)
	var wall := world.find_ref_node(WALL) as Node3D
	expect(wall != null, "the wall is built")
	if wall == null:
		quit(1)
		return
	expect(_graph() != null, "the wall has its behaviour graph")
	print("word wall: the trigger runs ", papyrus.get_scripts(TRIGGER))
	var fus := func() -> Dictionary: return papyrus.shout_info(PLAYER, UNRELENTING_FORCE)["words"][0]
	expect(not fus.call()["known"], "Fus is not known yet")
	var quest := 0
	var stage := 0
	for script in trigger_info["scripts"]:
		if script["name"] == "WordWallTriggerScript" and script["from_ref"]:
			quest = int(script["properties"]["myQuest"])
			stage = int(script["properties"]["myStage"])
	expect(quest != 0 and stage != 0, "the trigger names a quest stage: %X, %d" % [quest, stage])
	var stage_before: int = papyrus.get_quest_state(quest).get("stage", -1)
	expect(stage_before != stage, "the quest is not at that stage yet: %d" % stage_before)

	# ---- in the trigger, with the back to the target: no line of sight, nothing is taught
	var look_at := SkydotWorld.skyrim_position(target_at)
	_stand_near(look_at, 2.5, PI)
	await _frames(10)
	var ai := _session.ai
	expect(not ai.has_los(PLAYER, LOOK_TARGET), "looking away: no line of sight")
	await _frames(240)
	var trigger := papyrus.get_triggers().filter(func(t: Dictionary) -> bool: return t["ref"] == TRIGGER)
	expect(trigger.size() == 1 and trigger[0]["inside"] >= 1, "the player is in the trigger: %s" % [trigger])
	expect(_events.has("Dark1Wild") and _entered("Dark1State"), "the wall glows (Dark1Wild): %s, %s" % [_events, _states])
	expect(not fus.call()["known"] and not _events.has("Learned") and not _entered("Dark3State"),
		"and nothing is taught while looking away: %s" % [_events])

	# ---- turned to the target between the inner and the middle radius: the closer glow
	_stand_near(look_at, 6.0, 0.0)
	await _frames(2)
	expect(ai.has_los(PLAYER, LOOK_TARGET), "looking at it: line of sight")
	await _until(func() -> bool: return _entered("Dark3State"), 10.0)
	expect(_events.has("Dark3Wild") and _entered("Dark3State"), "the wall goes to its closer glow: %s, %s" % [_events, _states])
	expect(not fus.call()["known"], "too far to learn the word")

	# ---- within the inner radius: learned
	_stand_near(look_at, 2.5, 0.0)
	await _frames(2)
	var taught := await _until(func() -> bool: return fus.call()["known"], 20.0)
	expect(taught >= 0.0, "the word is taught: %s" % [_events])
	await _frames(120)
	expect(_messages.has("Word of Power Learned: Fus"), "the player is told: %s" % [_messages])
	expect(fus.call()["known"] and not fus.call()["unlocked"], "Fus is known and locked")
	expect(_events.has("Learned") and _entered("LightState"), "the wall lights: %s, %s" % [_events, _states])
	var after: int = papyrus.get_quest_state(quest).get("stage", -1)
	expect(after == stage, "the quest stage is set: %d" % after)
	print("word wall: events ", _events)
	print("word wall: graph states ", _states)
	print("word wall: quest 0x%X stage %d -> %d" % [quest, stage_before, after])
	print("smoke_word_wall: ", "OK" if failures == 0 else "%d failure(s)" % failures)
	quit(1 if failures > 0 else 0)
