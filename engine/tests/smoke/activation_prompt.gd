# SPDX-License-Identifier: GPL-3.0-or-later
#
# The activation prompt's rules that need no names (game/activation_prompt.gd):
# lock level texts, and no prompt without a name. The words of a real door are
# checked in actor_values.gd.
extends SceneTree

const CELL := "TestpackInterior"
const INSIDE_DOOR := 0x303

var failures := 0


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false

	func place_at(_at: Vector3, _yaw: float, _pitch: float) -> void:
		pass

	func set_view_distance(_metres: float) -> void:
		pass

	func save_section() -> PackedByteArray:
		return PackedByteArray()

	func load_section(_bytes: PackedByteArray) -> bool:
		return true


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
	if pack_dir == "":
		print("smoke_activation_prompt: SKIP (SKYDOT_TESTPACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.quests = false
	config.preload_doors = false
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	var session := GameSession.new()
	session.interactive = false
	root.add_child(session)
	expect(session.open(config, viewpoint, FakePlayer.new()), "the session opens")
	expect(session.start_game(), "the game starts")
	expect(session.enter_cell(CELL, SkydotWorld.skyrim_position(Vector3(0, -100, 150)), null), "the interior is entered")
	var prompt := ActivationPrompt.new(session.world, session.papyrus, session.host, viewpoint)
	var cell := session.world.get_ref_cell(INSIDE_DOOR)

	expect(ActivationPrompt.lock_text(-1) == "" and ActivationPrompt.lock_text(0) == "Novice"
		and ActivationPrompt.lock_text(25) == "Apprentice" and ActivationPrompt.lock_text(50) == "Adept"
		and ActivationPrompt.lock_text(75) == "Expert" and ActivationPrompt.lock_text(100) == "Master"
		and ActivationPrompt.lock_text(255) == "Requires key", "lock level names")

	# The test pack's doors have no FULL name, nor do their destinations, so
	# the prompt rules give no prompt (the real pack's doors are in
	# actor_values.gd).
	session.papyrus.set_locked(INSIDE_DOOR, true)
	expect(prompt.describe(cell, INSIDE_DOOR).is_empty(), "a door without a name has no prompt")
	expect(prompt.describe(cell, 0x1).is_empty(), "nor a reference that is nothing")
	print("smoke_activation_prompt: failures=%d" % failures)
	quit(failures)
