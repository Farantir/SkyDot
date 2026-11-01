# SPDX-License-Identifier: GPL-3.0-or-later
class_name ActorRegistry
extends RefCounted
## The SkydotActor nodes under the host, mirrored from the tree's node_added / node_removed signals, and
## per-node caches of the shapes that are searched for (the head bone, the skinned meshes).
##
## One hook catches every way an actor gets built (streamer cells, Place, PlacedRefs) and every way it
## leaves (cell unload, queue_free, reparent). Membership is keyed by node; the reference is read from the
## `skydot_ref` meta at query time, as it may be set after the node entered the tree.
## Lifecycle: membership mirrors the tree, so `reset()` and `on_place_left()` leave it alone; both drop the
## shape caches, silently.

var _host: Node
var _actors := {}  # SkydotActor -> true
var _heads := {}  # Node3D -> {"skeleton": Skeleton3D, "bone": int}
var _meshes := {}  # Node3D -> Array[MeshInstance3D], the skinned ones


func _init(host: Node) -> void:
	_host = host
	if host.is_inside_tree():
		_connect()
	else:
		host.tree_entered.connect(_connect, CONNECT_ONE_SHOT)


func _connect() -> void:
	var tree := _host.get_tree()
	tree.node_added.connect(_on_added)
	tree.node_removed.connect(_on_removed)
	for node in _host.find_children("*", "SkydotActor", true, false):
		_actors[node] = true


func _on_added(node: Node) -> void:
	if node is SkydotActor and _host.is_ancestor_of(node):
		_actors[node] = true


func _on_removed(node: Node) -> void:
	if node is SkydotActor:
		_actors.erase(node)
	_heads.erase(node)
	_meshes.erase(node)


## The actors under the host, valid and inside the tree.
func all() -> Array[SkydotActor]:
	var out: Array[SkydotActor] = []
	for node: SkydotActor in _actors:
		if is_instance_valid(node) and node.is_inside_tree():
			out.append(node)
	return out


## Where a spell or breath leaves `node`: its head's magic node (else its head bone, else the top of its body)
## in engine space.
func head(node: Node3D) -> Vector3:
	var entry: Dictionary = _heads.get(node, {})
	var skeleton: Variant = entry.get("skeleton")
	if skeleton == null or not is_instance_valid(skeleton) or not (skeleton as Skeleton3D).is_inside_tree():
		entry = {}
		var found := node.find_child("Skeleton", true, false) as Skeleton3D
		if found != null:
			entry = {"skeleton": found, "bone": head_bone(found)}
		if node.is_inside_tree():  # a node that never enters the tree would never be dropped
			_heads[node] = entry
	if entry.is_empty() or int(entry["bone"]) < 0:
		return head_fallback(node)
	return head_position(entry["skeleton"], entry["bone"])


## The middle of the box of the skinned meshes of `node`, null if it is not in the tree.
func chest(node: Node3D) -> Variant:
	if not node.is_inside_tree():
		return null
	var meshes: Array = _meshes.get(node, [])
	for mesh in meshes:
		if not is_instance_valid(mesh) or not (mesh as MeshInstance3D).is_inside_tree():
			meshes = []
			break
	if meshes.is_empty():
		for mesh in node.find_children("*", "MeshInstance3D", true, false):
			if mesh.get_parent() is Skeleton3D:
				meshes.append(mesh)
		_meshes[node] = meshes
	var box := AABB()
	var any := false
	for body in meshes:
		var part: AABB = body.global_transform * body.get_aabb()
		box = part if not any else box.merge(part)
		any = true
	return box.get_center() if any else node.global_position + Vector3.UP * 1.2


## `node`'s parts were rebuilt: look its shapes up again.
func refresh(node: Node) -> void:
	_heads.erase(node)
	_meshes.erase(node)


func reset() -> void:
	_heads.clear()
	_meshes.clear()


func on_place_left() -> void:
	_heads.clear()
	_meshes.clear()


## The bone of `skeleton` that is the head: the first whose lowercased name contains "head magicnode", else
## "head"; -1 if none.
static func head_bone(skeleton: Skeleton3D) -> int:
	for want in ["head magicnode", "head"]:
		for bone in skeleton.get_bone_count():
			if skeleton.get_bone_name(bone).to_lower().contains(want):
				return bone
	return -1


static func head_position(skeleton: Skeleton3D, bone: int) -> Vector3:
	return skeleton.to_global(skeleton.get_bone_global_pose(bone).origin)


static func head_fallback(node: Node3D) -> Vector3:
	var height: float = node.get("height") if node.get("height") != null else Combat.PLAYER_HEIGHT
	return node.global_position + Vector3.UP * (height * Combat.HEAD_FRACTION)
