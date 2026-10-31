# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

func test_copy_meta_leaves_the_watch_guards() -> void:
	var old := Node3D.new()
	var fresh := Node3D.new()
	old.set_meta("skydot_cell", 7)
	for key in PlacedRefs.WATCH_GUARDS:
		old.set_meta(key, true)
	PlacedRefs.copy_meta(old, fresh)
	expect_eq(fresh.get_meta("skydot_cell", 0), 7, "tags are copied")
	for key in ["skydot_sounds", "skydot_notifies", "skydot_blows", "skydot_breath_events"]:
		expect(not fresh.has_meta(key), "%s is not copied" % key)
	old.free()
	fresh.free()
