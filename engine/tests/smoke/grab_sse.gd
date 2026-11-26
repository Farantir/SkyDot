# SPDX-License-Identifier: GPL-3.0-or-later
#
# Holding Activate on loose clutter on the real SE pack (SKYDOT_SSE_PACK), through GameSession: a
# short press on a loose item still picks it up; held past ObjectGrab.HOLD_SECONDS a body is
# grabbed, follows a point ahead of the camera as it turns, falls when Activate goes up, and the
# attack button throws a held body along the view. Headless; skips without the pack.
extends SceneTree

const PLAYER := 0x14
const CELL := "BleakFallsBarrow01"
const AT := Vector3(5006.8, -893.3, -636)

var failures := 0
var _session: GameSession
var _camera: Camera3D


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_grab_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _ticks(count: int) -> void:
	for i in count:
		await physics_frame


func _ref_of(body: Node) -> int:
	var node := body
	while node != null and not node.has_meta("skydot_ref"):
		node = node.get_parent()
	return int(node.get_meta("skydot_ref")) if node != null else 0


func _count(form: int) -> int:
	for row in _session.inventory(PLAYER):
		if int(row["form"]) == form:
			return int(row["count"])
	return 0


## Put the camera 1.2 m from `body`, level with it, looking at it from the side `side` (radians);
## true if the grab ray and the activation pick both find it from there.
func _aim(body: SkydotDynamicBody, ref: int, side: float) -> bool:
	var centre := body.global_position
	_camera.global_position = centre + Vector3(cos(side), 0.25, sin(side)) * 1.2
	_camera.look_at(centre)
	if _session.grab.aimed_body() != body:
		return false
	var to := _camera.global_position - _camera.global_transform.basis.z * ScriptActivation.REACH
	return int(_session.world.pick_ref(_session.host, _camera.global_position, to).get("ref", 0)) == ref


## A loose body the camera can be aimed at, with its ref; [] if none. `items` asks for one the
## player can pick up (a MISC, weapon, ingredient...: in the world's item list).
func _find(skip: Array, items: bool) -> Array:
	for body in _session.host.find_children("*", "SkydotDynamicBody", true, false):
		var ref := _ref_of(body)
		if ref == 0 or ref in skip or body.mass > _session.grab.setting("fZKeyHeavyWeight"):
			continue
		var base := _session.papyrus.get_ref_base(ref)
		if items != _session.papyrus.item_info(base).has("found"):
			continue
		for side in [0.0, PI / 2, PI, -PI / 2]:
			if _aim(body, ref, side):
				return [body, ref, base]
	return []


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, _camera, PlayerBody.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell(CELL, SkydotWorld.skyrim_position(AT), null), "the interior is entered")
	await _ticks(10)
	print("smoke_grab_sse: fZKeyMaxForce %.0f, fZKeyMaxForceWeightHigh %.0f (from the pack)" % [
		_session.grab.setting("fZKeyMaxForce"), _session.grab.setting("fZKeyMaxForceWeightHigh")])

	# A short press picks a loose item up, as before.
	var item := _find([], true)
	expect(not item.is_empty(), "a loose item in %s the camera can aim at" % CELL)
	if not item.is_empty():
		var before := _count(item[2])
		_session.activate_pressed(false)
		await _ticks(3)
		expect(_count(item[2]) == before, "nothing is picked up while Activate is down")
		_session.activate_released()
		await _ticks(2)
		expect(_count(item[2]) == before + 1, "a short press on 0x%08X picks it up" % item[1])
		expect(not _session.grab.is_holding(), "a short press grabs nothing")

	# Held, a body is grabbed and follows the view.
	var held := _find([item[1]] if not item.is_empty() else [], true)
	if held.is_empty():
		held = _find([], false)
	expect(not held.is_empty(), "a second loose body to grab")
	if held.is_empty():
		_finish()
		return
	var body: SkydotDynamicBody = held[0]
	var before := _count(held[2])
	_session.activate_pressed(false)
	await _ticks(int(ObjectGrab.HOLD_SECONDS * 60) + 3)
	expect(_session.grab.held_body() == body, "holding Activate grabs 0x%08X" % held[1])
	var distance := _camera.global_position.distance_to(body.global_position)
	_camera.global_position.y += 0.4  # lift it off what it lay on
	_camera.rotate_y(0.6)  # and turn the view: the body swings round with it
	await _ticks(60)
	var target := _camera.global_position - _camera.global_transform.basis.z * distance
	var off := body.global_position.distance_to(target)
	print("smoke_grab_sse: held 0x%08X (%.1f kg) %.3f m from the hold point" % [held[1], body.mass, off])
	expect(off < 0.1, "the held body follows the view: %.3f m off" % off)
	var height := body.global_position.y
	_session.activate_released()
	await _ticks(10)
	expect(not _session.grab.is_holding(), "letting go of Activate drops it")
	expect(_count(held[2]) == before, "a hold does not pick it up")
	expect(body.global_position.y < height - 0.05, "the dropped body falls: %.3f -> %.3f" % [height, body.global_position.y])

	# Grabbed again and thrown with the attack button.
	await _ticks(60)
	if _aim(body, held[1], 0.0) or _aim(body, held[1], PI / 2) or _aim(body, held[1], PI) or _aim(body, held[1], -PI / 2):
		_session.activate_pressed(false)
		await _ticks(int(ObjectGrab.HOLD_SECONDS * 60) + 30)
		expect(_session.grab.is_holding(), "grabbed again")
		_camera.global_position.y += 0.5
		await _ticks(30)
		var start := body.global_position
		var ahead := -_camera.global_transform.basis.z
		_session.attack_pressed()
		_session.attack_released()
		await _ticks(10)
		expect(not _session.grab.is_holding(), "the attack lets go")
		var flown := (body.global_position - start).dot(ahead)
		expect(flown > 0.3, "the attack throws it along the view: %.2f m" % flown)
		var count := _count(held[2])
		_session.activate_released()
		await _ticks(2)
		expect(_count(held[2]) == count, "letting go of Activate after a throw activates nothing")
	else:
		expect(false, "the dropped body can be aimed at again")
	_finish()


func _finish() -> void:
	print("smoke_grab_sse: failures=%d" % failures)
	quit(failures)
