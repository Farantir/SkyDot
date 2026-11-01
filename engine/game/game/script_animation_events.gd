# SPDX-License-Identifier: GPL-3.0-or-later
#
# Animation events of placed models: what a script plays reaches the model's
# behaviour graph or clip, and what the model raises (text keys, a clip's end,
# graph events) goes back to the scripts that registered for it.
class_name ScriptAnimationEvents
extends RefCounted

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _clutter: ClutterPhysics


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, clutter: ClutterPhysics) -> void:
	_world = world
	_papyrus = papyrus
	_clutter = clutter


func on_play_animation(ref: int, animation: String) -> void:
	var node := _world.find_ref_node(ref)
	if node == null:
		print("0x%08X is not built, so it plays no %s animation" % [ref, animation])
	elif not play_event(node, ref, animation):
		var graph := node.find_child("SkydotBehavior", true, false)
		if graph != null:
			print("0x%08X: its behaviour graph (state %s) and its clips ignore %s" % [ref, graph.get_state(), animation])
		else:
			print("0x%08X has no %s animation" % [ref, animation])


## The one way an animation event reaches a placed model: its behaviour
## graph, else the NIF clip of that name. What the model then raises (a text
## key, the clip's end, a graph event) releases scripts waiting in
## PlayAnimationAndWait. False if nothing played.
func play_event(node: Node, ref: int, event: String) -> bool:
	_connect_notifies(node, ref)
	if not SkydotBehavior.play_event(node, event):
		return false
	_clutter.wake_around(node)
	return true


## A script registered for animation events of `ref` (RegisterForAnimationEvent).
func on_animation_events_wanted(ref: int) -> void:
	var node := _world.find_ref_node(ref)
	if node != null:
		_connect_notifies(node, ref)


func on_sub_graph_animation(ref: int, event_name: String) -> void:
	print("sub graph animation '%s' on 0x%08X (placeholder: only SkinFadeOut shows)" % [event_name, ref])


func _connect_notifies(node: Node, ref: int) -> void:
	var animator := node.get_node_or_null("SkydotAnimator")
	var player := node.find_child("SkydotBehavior", true, false)
	var on_text_key := _notify_text_key.bind(ref)
	var on_name := _notify.bind(ref)
	# Once per node: the first connection tells.
	if animator != null:
		if animator.text_key.is_connected(on_text_key):
			return
	elif player != null:
		if player.event_raised.is_connected(on_name):
			return
	elif node is SkydotActor and node.animation_event.is_connected(on_name):
		return
	if animator != null:
		animator.text_key.connect(on_text_key)
		animator.finished.connect(on_name)
	if player != null:
		player.event_raised.connect(on_name)
	# An actor's clips raise their annotations (FootLeft, HitFrame, SoundPlay.*) as they play.
	if node is SkydotActor:
		node.animation_event.connect(on_name)


func _notify_text_key(_clip: String, key: String, ref: int) -> void:
	_papyrus.notify_animation_event(ref, key)


func _notify(event: String, ref: int) -> void:
	_papyrus.notify_animation_event(ref, event)
