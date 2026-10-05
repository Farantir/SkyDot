# SPDX-License-Identifier: GPL-3.0-or-later
#
# A place built ahead of its use is held: in the scene, hidden, without
# physics (disabled bodies are not in the space) and off the navigation map.
# What Godot creates for its nodes then happens within the preparation's
# budget, not on arrival. DoorPreloader holds, the places and the streamer
# release.
class_name HeldPlace
extends RefCounted


static func hold(host: Node, node: Node3D) -> void:
	node.visible = false
	node.process_mode = Node.PROCESS_MODE_DISABLED
	host.add_child(node)
	_set_regions(node, false)


## Undo hold on arrival.
static func release(node: Node3D) -> void:
	node.process_mode = Node.PROCESS_MODE_INHERIT
	node.visible = true
	_set_regions(node, true)


static func _set_regions(root: Node, enabled: bool) -> void:
	for child in root.get_children():
		if child is NavigationRegion3D:
			child.enabled = enabled
