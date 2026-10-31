# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

const Fakes := preload("fakes.gd")

const BEHAVIOR := """
extends Node
signal event_raised(name: String)
"""


func _model() -> Node3D:
	var model := Node3D.new()
	var script := GDScript.new()
	script.source_code = BEHAVIOR
	script.reload()
	var behavior: Node = script.new()
	behavior.name = "SkydotBehavior"
	model.add_child(behavior)
	return model


func test_copy_meta_copies_every_tag() -> void:
	var old := Node3D.new()
	var fresh := Node3D.new()
	old.set_meta("skydot_cell", 7)
	old.set_meta("skydot_ref", 9)
	PlacedRefs.copy_meta(old, fresh)
	expect_eq(fresh.get_meta("skydot_cell", 0), 7, "the cell tag is copied")
	expect_eq(fresh.get_meta("skydot_ref", 0), 9, "the ref tag is copied")
	old.free()
	fresh.free()


func test_a_swapped_model_is_watched_by_sounds_once() -> void:
	var sounds := Sounds.new(Fakes.world(), null, Callable(), Callable())
	add_child(sounds)
	var old := _model()
	add_child(old)
	old.set_meta("skydot_ref", 5)
	sounds.watch(old, 5)
	sounds.watch(old, 5)
	var behavior := old.get_node("SkydotBehavior")
	expect_eq(behavior.get_signal_connection_list("event_raised").size(), 1, "watched once, not twice")
	# The swap: the fresh model has the old one's tags and is announced by model_swapped.
	var placed := PlacedRefs.new(Fakes.world(), Fakes.papyrus())
	add_child(placed)
	placed.model_swapped.connect(sounds.watch)
	var fresh := _model()
	add_child(fresh)
	PlacedRefs.copy_meta(old, fresh)
	placed.model_swapped.emit(fresh, 5)
	expect_eq(fresh.get_node("SkydotBehavior").get_signal_connection_list("event_raised").size(), 1,
		"the swapped-in model is watched")
