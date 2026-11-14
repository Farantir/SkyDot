# SPDX-License-Identifier: GPL-3.0-or-later
#
# Placed models that are ragdolls of their own: a web-wrapped corpse (effects/fxspiderwebkitdinnerhumangnd.nif,
# a "Dessicated Corpse" CONT), whose NIF hangs movable rigid bodies on skinned bones and joins them with
# ragdoll constraints. ModelCollision marks such a reference (`skydot_ragdoll`) when the game simulates it
# from load (RefFlags.dont_havok_settle: the Bleak Falls Barrow sacs fall from where the editor hung them);
# the ragdoll of the model's skeleton is built here with the same builder as the actors' (Ragdoll), its
# bodies on the clutter layer, and simulates until it sleeps. A copy the editor settled keeps its bodies
# fixed as placed (ModelCollision): it is not woken by touch yet.
class_name PropRagdolls
extends RefCounted

const META := "skydot_ragdoll"

var _world: SkydotWorld
var _live: Array[Dictionary] = []  # {node, ragdoll}


func _init(world: SkydotWorld) -> void:
	_world = world


## A place was built: the marked references under `root` get their ragdolls, simulating from now on.
func on_place_built(root: Node) -> void:
	_prune()
	var pending: Array[Node] = [root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node.has_meta(META) and node is Node3D:
			_build(node as Node3D)
			continue
		pending.append_array(node.get_children())


func _build(node: Node3D) -> void:
	node.remove_meta(META)
	var found := node.find_children("*", "Skeleton3D", true, false)
	if found.is_empty():
		return
	var skeleton := found.front() as Skeleton3D
	var ragdoll := Ragdoll.make(skeleton, SkydotWorld.get_model_ragdoll(node))
	if ragdoll == null:
		return
	for bone in ragdoll.bones:
		if bone != null:
			bone.collision_layer = SkydotPlayer.LAYER_CLUTTER
			bone.collision_mask = SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN | SkydotPlayer.LAYER_CLUTTER \
				| SkydotPlayer.LAYER_ACTOR | SkydotPlayer.LAYER_DEAD_BIPED
	ragdoll.start()
	# The simulated poses go into the bones without the skeleton's scale (Corpses.RagdollPose).
	var fix := Corpses.RagdollPose.new()
	for bone in ragdoll.bones:
		if bone != null:
			fix.bones.append(bone)
	skeleton.add_child(fix)
	var entry := {"node": node, "ragdoll": ragdoll}
	_live.append(entry)
	# The joints go before the bodies they join (a joint left on a freed body crashes Jolt).
	node.tree_exiting.connect(func() -> void:
		ragdoll.stop()
		_live.erase(entry), CONNECT_ONE_SHOT)


## Ragdolls whose model is gone free their joints.
func _prune() -> void:
	for entry in _live.duplicate():
		if not is_instance_valid(entry["node"]) or not (entry["node"] as Node).is_inside_tree():
			(entry["ragdoll"] as Ragdoll).stop()
			_live.erase(entry)


## Number of prop ragdolls built and still in the world.
func count() -> int:
	_prune()
	return _live.size()
