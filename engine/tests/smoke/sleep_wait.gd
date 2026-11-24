# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Sleep/Wait menu on the test pack: a GameSession built headless (as journal.gd does) with the SleepWaitView over
# it. A wait of 3 hours (the slider moved by the menu's keys, Enter) moves the clock 3 hours, counts Hours Waiting,
# lets the actors' Health recover and restores the time scale; a sleep in the free bed counts Hours Slept; the
# actor's bed (XOWN) is refused with sNoSleepInOwnedBed, a foe refuses a wait and interrupts a rest under way.
extends SceneTree

const CELL := "TestpackInterior"
const FREE_BED := 0x308
const OWNED_BED := 0x309
const ACTOR := 0x307

var failures := 0
var _session: GameSession
var _view: SleepWaitView


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
	if pack_dir == "":
		print("smoke_sleep_wait: SKIP (SKYDOT_TESTPACK unset)")
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


## The rest under way runs to its end (or its interruption), an hour per HOUR_SECONDS.
func _rest() -> void:
	for i in 30:
		if not _view.is_open():
			return
		_hour()


## One hour of the rest's time (the shell's loop steps it), then the menu's frame.
func _hour() -> void:
	_session.rest.step(Rest.HOUR_SECONDS)
	_view._process(Rest.HOUR_SECONDS)


func _hours_between(before: float, after: float) -> float:
	return fposmod(after - before, 24.0)


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.time = 14.0
	config.navigation = false
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
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell(CELL, SkydotWorld.skyrim_position(Vector3(0, -100, 150)), null), "the interior is entered")
	for i in 3:
		await process_frame
	_view = SleepWaitView.new(_session)
	root.add_child(_view)
	var refusals: Array[String] = []
	_session.message.connect(func(text: String, _box: bool) -> void: refusals.append(text))

	# ---- ownership
	expect(int(_session.papyrus.get_owner(FREE_BED)["owner"]) == 0, "the free bed has no owner")
	var owner := _session.papyrus.get_owner(OWNED_BED)
	expect(int(owner["owner"]) == 0x164 and not owner["faction"] and not owner["player"],
		"the actor owns the other bed: %s" % [owner])
	expect(_session.rest.refusal(true, OWNED_BED) == "You cannot sleep in an owned bed.", "an owned bed is refused")
	expect(_session.rest.refusal(true, FREE_BED) == "", "the free bed is not")

	# ---- a wait of 3 hours
	var hour := _session.clock.get_hour()
	var scale := _session.clock.get_time_scale()
	# The test pack's actors have no Health of their own: the actor gets 100 and HealRate 0.7 (the game's player).
	_session.papyrus.set_actor_value(ACTOR, "Health", 100.0)
	_session.papyrus.set_actor_value(ACTOR, "HealRate", 0.7)
	_session.papyrus.set_actor_value(ACTOR, "HealRateMult", 100.0)
	_session.papyrus.damage_actor_value(ACTOR, "Health", 50.0)
	var damaged := _session.papyrus.get_actor_value(ACTOR, "Health")
	_view.open(false)
	expect(_view.is_open() and not _view.sleeping and _view.hours() == 1, "T opens the menu to wait, at 1 hour")
	_key(&"ui_right")
	_key(&"ui_right")
	expect(_view.hours() == 3, "right moves the slider an hour each: %d" % _view.hours())
	_key(&"ui_accept")
	expect(_view.hours_left == 3 and _session.clock.get_time_scale() == 0.0, "OK begins the wait, the clock stands")
	_key(&"ui_left")
	expect(_view.hours_left == 3, "the slider is disabled while waiting")
	_rest()
	expect(not _view.is_open(), "the menu closes after the last hour")
	expect(absf(_hours_between(hour, _session.clock.get_hour()) - 3.0) < 0.01,
		"the clock moved 3 hours: %.2f -> %.2f" % [hour, _session.clock.get_hour()])
	expect(_session.clock.get_time_scale() == scale, "the time runs again")
	expect(_session.journal.value("Hours Waiting") == 3, "Hours Waiting counts 3: %s" % [_session.journal.value("Hours Waiting")])
	expect(_session.journal.value("Hours Slept") == 0, "no hours slept")
	var health := _session.papyrus.get_actor_value(ACTOR, "Health")
	expect(health > damaged, "the actor recovered while waiting: %.1f -> %.1f" % [damaged, health])

	# ---- a sleep of 2 hours in the free bed; the cancel key closes without resting
	_view.open(true, FREE_BED)
	expect(_view.is_open() and _view.sleeping, "a free bed opens the menu to sleep")
	_key(&"ui_cancel")
	expect(not _view.is_open(), "cancel closes it")
	_view.open(true, FREE_BED)
	_view.set_value(2.0)
	_key(&"ui_accept")
	_rest()
	expect(_session.journal.value("Hours Slept") == 2, "Hours Slept counts 2: %s" % [_session.journal.value("Hours Slept")])
	refusals.clear()
	_view.open(true, OWNED_BED)
	expect(not _view.is_open() and refusals == ["You cannot sleep in an owned bed."], "the owned bed says why: %s" % [refusals])

	# ---- a foe: no wait, and a rest under way stops
	_session.rest.on_combat_state(ACTOR, SkydotPapyrus.PLAYER_REF, 1)
	refusals.clear()
	_view.open(false)
	expect(not _view.is_open() and refusals == ["You cannot wait when enemies are nearby."], "a foe refuses a wait: %s" % [refusals])
	_session.rest.on_combat_state(ACTOR, SkydotPapyrus.PLAYER_REF, 0)
	_view.open(false)
	_view.set_value(5.0)
	_key(&"ui_accept")
	_hour()
	_session.rest.on_combat_state(ACTOR, SkydotPapyrus.PLAYER_REF, 1)
	_hour()
	expect(not _view.is_open() and _session.journal.value("Hours Waiting") == 5,
		"a foe stops the wait after its hour: %s" % [_session.journal.value("Hours Waiting")])
	expect(_session.clock.get_time_scale() == scale, "the time runs again after the interruption")
	_finish()


func _finish() -> void:
	print("smoke_sleep_wait: %s" % ("OK" if failures == 0 else "%d failures" % failures))
	quit(0 if failures == 0 else 1)
