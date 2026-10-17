# SPDX-License-Identifier: GPL-3.0-or-later
#
# Actor values through GameSession on the real SE pack (SKYDOT_SSE_PACK): the
# player starts at Health, Magicka and Stamina 100 (NPC_ 0x7 on NordRace), a
# damage lowers a value and the maximum stays, regeneration brings it back, and
# a player object that sprints loses Stamina (7 a second) and may not sprint
# once it is out, until Stamina is back above 0. Headless; skips without the
# pack.
extends SceneTree

const PLAYER := 0x14

var failures := 0
var _session: GameSession
var _player: FakePlayer


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false
	var sprinting := false
	var allowed_calls: Array[bool] = []

	func place_at(_at: Vector3, new_yaw: float, new_pitch: float) -> void:
		yaw = new_yaw
		pitch = new_pitch

	func set_view_distance(_metres: float) -> void:
		pass

	func save_section() -> PackedByteArray:
		return PackedByteArray()

	func load_section(_bytes: PackedByteArray) -> bool:
		return true

	func is_sprinting() -> bool:
		return sprinting

	func set_sprint_allowed(allowed: bool) -> void:
		allowed_calls.append(allowed)


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_actor_values: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _near(a: float, b: float, tolerance := 0.01) -> bool:
	return absf(a - b) < tolerance


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	_player = FakePlayer.new()
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, viewpoint, _player), "the session opens")
	if _session.world == null:
		quit(1)
		return
	var deaths: Array[int] = []
	_session.actor_died.connect(func(ref: int) -> void: deaths.append(ref))
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")

	# The barrow's entrance, a load door: "Open", named after where it leads.
	var door := 0x37603
	var door_prompt := ActivationPrompt.new(_session.world, _session.papyrus, _session.host, viewpoint)\
		.describe(_session.world.get_ref_cell(door), door)
	print("door prompt: ", door_prompt)
	expect(not door_prompt.is_empty() and door_prompt["verb"] == "Open" and door_prompt["name"] != ""
		and door_prompt["lock_level"] == -1, "the barrow door's prompt is Open with a name")

	for name in ["Health", "Magicka", "Stamina"]:
		expect(_near(_session.actor_value(PLAYER, name), 100.0), "the player's %s is 100: %f" % [name, _session.actor_value(PLAYER, name)])
		expect(_near(_session.actor_value_max(PLAYER, name), 100.0), "and so is its maximum")
	expect(_near(_session.actor_value(PLAYER, "health"), 100.0), "names ignore case")
	expect(_session.actor_value(PLAYER, "NoSuchValue") == 0.0, "an unknown name reads 0")

	_session.papyrus.damage_actor_value(PLAYER, "Health", 40.0)
	expect(_near(_session.actor_value(PLAYER, "Health"), 60.0), "damage lowers Health")
	expect(_near(_session.actor_value_max(PLAYER, "Health"), 100.0), "not its maximum")
	_session.papyrus.update(10.0)
	expect(_session.actor_value(PLAYER, "Health") > 60.0, "Health regenerates")
	_session.papyrus.restore_actor_value(PLAYER, "Health", 1000.0)
	expect(_near(_session.actor_value(PLAYER, "Health"), 100.0), "restore takes the damage back")
	_session.papyrus.damage_actor_value(PLAYER, "Health", 1000.0)
	expect(_near(_session.actor_value(PLAYER, "Health"), 0.0) and deaths.is_empty(), "the player stays at 0 and does not die")

	# Sprinting: 7 a second, then no more sprinting; stamina back, sprinting again.
	_player.sprinting = true
	var feet := Vector3.ZERO
	_session.step(1.0, feet)
	expect(_near(_session.actor_value(PLAYER, "Stamina"), 93.0, 0.5), "a second of sprint costs about 7 Stamina: %f" % _session.actor_value(PLAYER, "Stamina"))
	expect(_player.allowed_calls.is_empty(), "sprinting stays allowed")
	# Frame-sized steps: each drain renews the regeneration delay (0.5 s in the
	# game's data), as it does in play.
	for i in 300:
		_session.step(0.1, feet)
	expect(_session.actor_value(PLAYER, "Stamina") <= 0.001, "Stamina runs out")
	expect(_player.allowed_calls == [false], "and sprinting is forbidden once: %s" % [_player.allowed_calls])
	_player.sprinting = false
	_session.papyrus.update(60.0)
	_session.step(0.0, feet)
	expect(_session.actor_value(PLAYER, "Stamina") > 0.0, "Stamina comes back")
	expect(_player.allowed_calls == [false, true], "and sprinting is allowed again: %s" % [_player.allowed_calls])
	print("smoke_actor_values: failures=%d" % failures)
	quit(failures)
