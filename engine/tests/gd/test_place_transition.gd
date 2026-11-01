# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

const Fakes := preload("fakes.gd")

func _fade_to_travel(t: PlaceTransition) -> void:
	t.begin_fade({"ref": 1})
	for i in 10:
		t.step(0.1)
		if t.phase == PlaceTransition.Phase.TRAVEL:
			break

func test_cancel_stops_travel() -> void:
	var t := PlaceTransition.new(null, null)  # travel with no place would be a script error
	_fade_to_travel(t)
	expect_eq(t.phase, PlaceTransition.Phase.TRAVEL, "the fade reached TRAVEL")
	t.cancel()
	expect(not t.is_fading(), "cancel ends the transition")
	for i in 5:
		if t.is_fading():
			t.step(0.1)
	expect(not t.is_fading(), "still idle")

func test_the_fade_rises_to_black_and_cancel_clears_it() -> void:
	var t := PlaceTransition.new(null, null)
	var heard := watch(t, "fade_changed")
	expect_eq(t.fade, 0.0, "clear at first")
	_fade_to_travel(t)
	expect_eq(t.fade, 1.0, "black at TRAVEL")
	expect(not heard.is_empty() and heard[0][0] > 0.0, "the first emission is a rise")
	var last: float = 0.0
	for emission: Array in heard:
		expect(emission[0] >= last, "the fade only rises on the way out")
		last = emission[0]
	expect_eq(last, 1.0, "the last emission is black")
	t.cancel()
	expect_eq(t.fade, 0.0, "cancel clears the fade")
	expect_eq(heard[heard.size() - 1][0], 0.0, "cancel emits 0")
	var count := heard.size()
	t.cancel()
	expect_eq(heard.size(), count, "no emission when nothing changes")

func test_the_fade_falls_back_after_the_travel() -> void:
	var streamer := SkydotStreamer.new()  # world_id 0: nothing to wait for
	var place := Place.new(Fakes.world(), Node3D.new(), null, null, PlayerBody.new(), null, SkydotClock.new(), streamer)
	var t := PlaceTransition.new(place, streamer)
	var heard := watch(t, "fade_changed")
	t.begin_fade({"ref": 1, "arrival": Transform3D(), "destination_interior": false, "destination_world": 0})
	for i in 60:
		t.step(0.1)
	expect(not t.is_fading(), "the transition is over")
	expect_eq(t.fade, 0.0, "clear again")
	expect(heard.size() > 3, "the fade was announced on the way")
	expect_eq(heard[heard.size() - 1][0], 0.0, "the last emission is clear")
