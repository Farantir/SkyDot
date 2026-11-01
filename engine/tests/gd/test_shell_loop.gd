# SPDX-License-Identifier: GPL-3.0-or-later
#
# ShellLoop (viewer/shell_loop.gd): the commands, paralysis and death both shells share. The
# session is a GameSession subclass (GDScript methods can be overridden) that records the calls
# the loop makes and is never opened; the rig and the controls are the real ones.
extends "gd_test.gd"


class StubSession extends GameSession:
	var calls: Array[String] = []
	var reload_ok := true

	func activate_in_view(force: bool) -> void:
		calls.append("activate %s" % force)

	func toggle_weapon() -> void:
		calls.append("toggle_weapon")

	func save_game(path: String) -> bool:
		calls.append("save " + path)
		return true

	func load_game(path: String) -> bool:
		calls.append("load " + path)
		return true

	func reload_or_restart() -> bool:
		calls.append("reload_or_restart")
		return reload_ok


var _session: StubSession
var _controls: PlayerInput
var _loop: ShellLoop
var _notes: Array[String] = []


func before_each() -> void:
	var settings := ViewerSettings.from_arguments([])
	var rig := PlayerRig.new(settings, SkydotImageSpace.new())
	add_child(rig.camera)
	add_child(rig.player)
	_session = StubSession.new()
	_session.player_state = PlayerState.new(PlayerStateData.new(SkydotPapyrus.new()))
	add_child(_session)
	_controls = PlayerInput.new(rig)
	add_child(_controls)
	_loop = ShellLoop.new(self, _session, rig, _controls, settings, SkydotImageSpace.new(),
		func(text: String) -> void: _notes.append(text))


func test_shared_commands_reach_the_session() -> void:
	_controls.command.emit(PlayerInput.ACTIVATE, true)
	_controls.command.emit(PlayerInput.READY_WEAPON, false)
	_controls.command.emit(PlayerInput.QUICK_SAVE, false)
	_controls.command.emit(PlayerInput.QUICK_LOAD, false)
	expect_eq(_session.calls, ["activate true", "toggle_weapon", "save " + SaveService.QUICKSAVE,
		"load " + SaveService.QUICKSAVE], "the session's calls")


func test_fly_toggle_is_noted() -> void:
	_controls.command.emit(PlayerInput.TOGGLE_FLY, false)
	_controls.command.emit(PlayerInput.TOGGLE_FLY, false)
	expect_eq(_notes, ["flying", "walking"], "the notes")


func test_other_actions_are_the_shells() -> void:
	var heard := watch(_loop, "command")
	var inventory := watch(_loop, "inventory_requested")
	var magic := watch(_loop, "magic_requested")
	_controls.command.emit(PlayerInput.SHOW_POSITION, true)
	_controls.command.emit(PlayerInput.INVENTORY, false)
	_controls.command.emit(PlayerInput.MAGIC, false)
	expect_eq(heard, [[PlayerInput.SHOW_POSITION, true]], "an unknown action is re-emitted")
	expect_eq(inventory.size(), 1, "the inventory is requested")
	expect_eq(magic.size(), 1, "the magic menu is requested")
	expect(_session.calls.is_empty(), "the session heard none of it")


func test_messages_go_to_the_note() -> void:
	_session.message.emit("hello", false)
	expect_eq(_notes, ["hello"], "the notes")


func test_paralysis_reaches_the_controls() -> void:
	_session.player_state.set_paralyzed(true)
	_loop.drive(0.1)
	expect(_controls.paralyzed, "paralyzed controls")
	_session.player_state.set_paralyzed(false)
	_loop.drive(0.1)
	expect(not _controls.paralyzed, "free controls")


func test_death_blocks_and_a_reload_frees() -> void:
	var died := watch(_loop, "died")
	_session.player_died.emit(7)
	expect(_controls.blocked, "blocked at death")
	expect_eq(died, [[7]], "the shell is told")
	expect(_loop.reload_after_death(), "the reload works")
	expect(not _controls.blocked, "free after the reload")


func test_a_failed_reload_keeps_the_controls_blocked() -> void:
	_session.player_died.emit(7)
	_session.reload_ok = false
	expect(not _loop.reload_after_death(), "the reload fails")
	expect(_controls.blocked, "still blocked")


func test_quit_countdown_after_activations() -> void:
	expect_eq(_loop.quit_in, -1, "not counting")
	_session.activations_finished.emit()
	expect_eq(_loop.quit_in, 5, "five frames when --quit-after is not given")
