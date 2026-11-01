# SPDX-License-Identifier: GPL-3.0-or-later
#
# The behaviour graphs of placed references and the store that keeps them for
# saves: what was just built gets its graphs put where the saves left them (an
# opened gate stays open), and the store stays current while a graph changes or
# its cell unloads.
class_name ScriptGraphs
extends RefCounted

var _papyrus: SkydotPapyrus


func _init(papyrus: SkydotPapyrus) -> void:
	_papyrus = papyrus


## Put the behaviour graphs of what was just built where the saves left them,
## before scripts see the references, and keep the store current when a graph
## changes or its cell unloads.
func resume(root: Node) -> void:
	for node in Place.ref_nodes(root):
		var player := node.find_child("SkydotBehavior", true, false)
		if player == null:
			continue
		var ref: int = node.get_meta("skydot_ref")
		var bytes := _papyrus.get_animation_state(ref)
		if not bytes.is_empty() and not player.restore_snapshot(bytes):
			print("0x%08X: its saved graph state does not fit the graph, so it starts over" % ref)
		player.state_changed.connect(_record.bind(ref, player))
		player.leaving.connect(_record.bind(ref, player))


func _record(ref: int, player: Node) -> void:
	_papyrus.set_animation_state(ref, player.get_snapshot())


## Record every live graph (for a save).
func record_all() -> void:
	for player in Engine.get_main_loop().get_nodes_in_group("skydot_behaviors"):
		_record(player.get_ref(), player)


## The live graphs are about to be replaced by a loaded save: what they hold
## is stale and must not reach the store when their cell unloads.
func retire_all() -> void:
	for player in Engine.get_main_loop().get_nodes_in_group("skydot_behaviors"):
		player.retire()
