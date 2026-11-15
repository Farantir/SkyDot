# SPDX-License-Identifier: GPL-3.0-or-later
#
# The journal's System tab saves and loads, round trip, on the test pack: a GameSession built headless (as
# game_save.gd does) with the JournalView over it. [NEW SAVE] writes save001 with its INFO (place, level), the game
# changes (a stat, a log entry, the hour), then LOAD is chosen with the menu keys (down, down, Enter: the characters;
# Enter: the saves; Enter: "Load this game?"; Enter: yes) and the saved state is back. The menu's keys also walk the
# panels (SETTINGS, CONTROLS, QUIT's question answered No) without errors.
extends SceneTree

const CELL := "TestpackInterior"
const DIRECTORY := "user://smoke/journal/"

var failures := 0
var _session: GameSession
var _view: JournalView


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
	if pack_dir == "":
		print("smoke_journal: SKIP (SKYDOT_TESTPACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _key(action: StringName) -> void:
	var event := InputEventAction.new()
	event.action = action
	event.pressed = true
	_view._input(event)


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.time = 14.0
	config.navigation = false
	config.actors = false
	config.quests = false
	config.preload_doors = false
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	root.add_child(_session)
	expect(_session.open(config, viewpoint, PlayerBody.new()), "the session opens on the test pack")
	if _session.world == null:
		_finish()
		return
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(DIRECTORY))
	for file in DirAccess.get_files_at(DIRECTORY):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(DIRECTORY + file))
	_session.saves.directory = DIRECTORY
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell(CELL, SkydotWorld.skyrim_position(Vector3(0, -100, 150)), null), "the interior is entered")
	for i in 3:
		await process_frame
	_view = JournalView.new(_session)
	root.add_child(_view)

	# ---- the state at save time, saved from the System tab
	_session.journal.on_quest_stage(0x123, 10, "A first entry.")
	_session.journal.count("Bribes", 2)
	var hour := _session.clock.get_hour()
	_view.open(JournalView.SYSTEM)
	expect(_view.is_open() and _view.tab == JournalView.SYSTEM, "the journal opens on System")
	_view.system.debug_state("save-new")
	expect(not _view.is_open(), "[NEW SAVE] saves and closes the journal")
	var saves := _session.saves.list_saves()
	expect(saves.size() == 1 and String(saves[0]["path"]).ends_with("save001.skydot"), "save001 is listed: %s" % [saves])
	if not saves.is_empty():
		expect(saves[0]["number"] == "001", "its number")
		expect(String(saves[0]["location"]) != "", "its place: '%s'" % saves[0].get("location", ""))
		expect(int(saves[0]["level"]) >= 1, "its level")
	expect(_session.saves.next_save_path().ends_with("save002.skydot"), "the next save is save002")

	# ---- the game goes on
	_session.journal.on_quest_stage(0x123, 20, "A later entry.")
	_session.journal.count("Bribes", 5)
	_session.clock.shift(3.0)
	expect(_session.journal.description(0x123) == "A later entry.", "the log moved on")

	# ---- the other panels by key, then LOAD
	_view.open(JournalView.SYSTEM)
	for panel in [5, 6, 8]:  # SETTINGS, CONTROLS, QUIT
		_view.system.main_index = panel
		_key(&"ui_accept")
		expect(_view.system.state != JournalSystem.State.MAIN, "category %d opens its panel" % panel)
		if panel == 5:
			_key(&"ui_accept")
			expect(_view.system.state == JournalSystem.State.OPTIONS, "Gameplay shows its options")
			_key(&"ui_right")
			_key(&"ui_cancel")
			expect(_view.system.state == JournalSystem.State.SETTINGS, "cancel goes back to the categories")
		if panel == 8:
			_key(&"ui_down")
			_key(&"ui_accept")
			expect(_view.system.state == JournalSystem.State.CONFIRM, "Desktop asks first")
			_key(&"ui_cancel")
			expect(_view.system.state == JournalSystem.State.QUIT_LIST, "No goes back to the list")
		_key(&"ui_cancel")
		expect(_view.system.state == JournalSystem.State.MAIN, "cancel goes back to the categories")
	_view.system.main_index = 0
	_key(&"ui_down")
	_key(&"ui_down")
	_key(&"ui_accept")
	expect(_view.system.state == JournalSystem.State.CHARACTERS, "LOAD lists the characters")
	_key(&"ui_accept")
	expect(_view.system.state == JournalSystem.State.LOAD_LIST, "then the saves")
	_key(&"ui_accept")
	expect(_view.system.state == JournalSystem.State.CONFIRM, "Load this game? is asked")
	_key(&"ui_accept")
	expect(not _view.is_open(), "loading closes the journal")
	for i in 3:
		await process_frame
	expect(_session.journal.description(0x123) == "A first entry.", "the log is as saved: '%s'" % _session.journal.description(0x123))
	expect(_session.journal.value("Bribes") == 2, "the stats are as saved: %s" % [_session.journal.value("Bribes")])
	expect(absf(_session.clock.get_hour() - hour) < 0.05, "the hour is as saved: %.2f / %.2f" % [_session.clock.get_hour(), hour])
	_finish()


func _finish() -> void:
	for file in DirAccess.get_files_at(DIRECTORY):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(DIRECTORY + file))
	print("smoke_journal: %s" % ("OK" if failures == 0 else "%d failures" % failures))
	quit(0 if failures == 0 else 1)
