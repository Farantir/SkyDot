# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

func test_cancel_stops_travel() -> void:
	var t := PlaceTransition.new(null, null)  # travel with no place would be a script error
	add_child(t)
	t.begin_fade({"ref": 1})
	for i in 10:
		t.step(0.1)
		if t.phase == PlaceTransition.Phase.TRAVEL:
			break
	expect_eq(t.phase, PlaceTransition.Phase.TRAVEL, "the fade reached TRAVEL")
	t.cancel()
	expect(not t.is_fading(), "cancel ends the transition")
	for i in 5:
		if t.is_fading():
			t.step(0.1)
	expect(not t.is_fading(), "still idle")
