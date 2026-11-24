# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rest (game/rest.gd) owns a rest's hours: they pass in Rest.step however the Sleep/Wait menu is closed, and a load
# ends a rest under way. The session is never opened: a clock, the fake VM and no world.
extends "gd_test.gd"
const Fakes := preload("fakes.gd")

const SCALE := 20.0

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
