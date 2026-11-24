# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rest (game/rest.gd) owns a rest's hours: they pass in Rest.step however the Sleep/Wait menu is closed, a load
# ends a rest under way, and a foe left behind (its node gone without leaving combat) no longer refuses one. The session is never opened: a clock, the fake VM and no world.
extends "gd_test.gd"
const Fakes := preload("fakes.gd")

const SCALE := 20.0
const FOE := 0xFF000077

var _session: GameSession
var _view: SleepWaitView


func before_each() -> void:
	_session = GameSession.new()
	_session.clock = SkydotClock.new()
	_session.clock.setup(10.0, SCALE, false)
	_session.papyrus = Fakes.papyrus()
	_session.rest = Rest.new(_session)
	add_child(_session)
	_view = SleepWaitView.new(_session)
	add_child(_view)


func _waits(hours: int) -> void:
	_view.open(false)
	_view.set_value(float(hours))
	_view.accept()


func test_the_rest_finishes_when_the_menu_is_closed_early() -> void:
	_waits(3)
	expect(_session.rest.is_resting() and _session.clock.get_time_scale() == 0.0, "the wait began, the clock stands")
	_view.close()
	for i in 3:
		_session.rest.step(Rest.HOUR_SECONDS)
	expect(not _session.rest.is_resting(), "the rest is over")
	expect_eq(_session.clock.get_time_scale(), SCALE, "the clock runs again")
	expect_eq(_session.rest.hours_left, 0, "no hours left")


func test_the_menu_shows_the_rest_and_closes_after_it() -> void:
	_waits(2)
	_session.rest.step(Rest.HOUR_SECONDS)
	_view._process(Rest.HOUR_SECONDS)
	expect(_view.is_open() and _view.hours_left == 1, "an hour left: %d" % _view.hours_left)
	_session.rest.step(Rest.HOUR_SECONDS)
	_view._process(Rest.HOUR_SECONDS)
	expect(not _view.is_open(), "the menu closes after the last hour")


func test_a_reset_ends_the_rest_and_restores_the_time_scale() -> void:
	_waits(5)
	_session.rest.reset()
	expect(not _session.rest.is_resting(), "the rest is over")
	expect_eq(_session.clock.get_time_scale(), SCALE, "the clock runs again")
	_view._process(0.0)
	expect(not _view.is_open(), "the menu closes")


func test_a_reset_keeps_the_time_scale_a_load_set() -> void:
	_waits(5)
	_session.clock.set_time_scale(6.0)  # the save's
	_session.rest.reset()
	expect_eq(_session.clock.get_time_scale(), 6.0, "the loaded time scale stays")


func test_a_foe_left_behind_no_longer_refuses_a_rest() -> void:
	_session.world = Fakes.world()
	var body := Node3D.new()
	add_child(body)
	_session.world.tag_runtime_ref(FOE, 0, body)
	_session.rest.on_combat_state(FOE, SkydotPapyrus.PLAYER_REF, 1)
	expect(_session.rest.fighting(), "a foe with its body fights")
	expect_eq(_session.rest.refusal(false), Rest.FALLBACK["sNoWaitHostileActorsNear"], "it refuses a wait")
	body.free()  # detached: freed with its cell, still in combat
	expect(not _session.rest.fighting(), "a detached foe fights no more")
	expect_eq(_session.rest.refusal(false), "", "a wait is allowed")
	_waits(2)
	_session.rest.step(Rest.HOUR_SECONDS)
	expect(_session.rest.is_resting(), "nor does it stop a rest")
	_session.rest.finish(false)
	var back := Node3D.new()
	add_child(back)
	_session.world.tag_runtime_ref(FOE, 0, back)
	expect(_session.rest.fighting(), "back in the player's place, still in combat, it counts again")
