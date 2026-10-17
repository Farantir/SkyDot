# SPDX-License-Identifier: GPL-3.0-or-later
#
# What an actor's death looks like (GAMEPLAY-PLAN.md, 6d; SYSTEMS.md A7). No
# camera, input or overlay. When `SkydotPapyrus` says an actor died (or a
# place is built with a dead actor in it), its node stops (`SkydotActor.
# set_dead`: no AI, no walking, no animation, no collision) and a ragdoll
# takes its skeleton: a `PhysicalBoneSimulator3D` under the `Skeleton3D` with
# one capsule `PhysicalBone3D` per bone of the human set (pelvis, spine,
# head, arms, legs; draugr and the other humanoids share the bone names),
# cone joints, and the impulse of the hit that killed it. When the body rests
# (or after MAX_SECONDS) the ragdoll is frozen into the bones' poses and freed,
# and the corpse place is stored (`SkydotPapyrus.set_corpse_place`: space,
# position in Skyrim units, facing), which saves keep: a dead actor built
# later (the cell entered again, a load) is placed there and lies down the
# same way, without an impulse. A skeleton without the bones logs
# `death: no ragdoll for <skeleton>` and lies on its side.
#
# The corpse stays pickable for activation (Search): `pick_ref` finds a node
# by the `skydot_activatable` and `skydot_bounds` metadata, which this sets on
# the actor's node (its cylinder is not on a physics layer any more).
class_name Corpses
extends RefCounted

## The human ragdoll set: bone, the bone its capsule runs to (or "" for a
## fixed length), the capsule's radius in metres, the joint's swing and twist
## spans in degrees (0: the root, no joint). Placeholders: shapes and limits
## are sized by eye on the human skeleton, not from the game's ragdoll data.
const BONES := [
	["NPC Pelvis [Pelv]", "NPC Spine [Spn0]", 0.12, 0.0, 0.0],
	["NPC Spine [Spn0]", "NPC Spine1 [Spn1]", 0.11, 15.0, 10.0],
	["NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", 0.12, 20.0, 15.0],
	["NPC Spine2 [Spn2]", "NPC Neck [Neck]", 0.13, 20.0, 15.0],
	["NPC Head [Head]", "", 0.09, 45.0, 30.0],
	["NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", 0.05, 80.0, 40.0],
	["NPC L Forearm [LLar]", "NPC L Hand [LHnd]", 0.04, 60.0, 20.0],
	["NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", 0.05, 80.0, 40.0],
	["NPC R Forearm [RLar]", "NPC R Hand [RHnd]", 0.04, 60.0, 20.0],
	["NPC L Thigh [LThg]", "NPC L Calf [LClf]", 0.08, 60.0, 20.0],
	["NPC L Calf [LClf]", "NPC L Foot [Lft ]", 0.06, 60.0, 15.0],
	["NPC R Thigh [RThg]", "NPC R Calf [RClf]", 0.08, 60.0, 20.0],
	["NPC R Calf [RClf]", "NPC R Foot [Rft ]", 0.06, 60.0, 15.0],
]
## A capsule with no bone to run to, metres long (the head).
const HEAD_LENGTH := 0.12
## The body that takes the hit's impulse.
const IMPULSE_BONE := "NPC Spine2 [Spn2]"
## Placeholder: the impulse of a killing hit in kg m/s, and its share upward.
const HIT_IMPULSE := 3.0
const HIT_LIFT := 0.3
## Frozen when every body is slower than REST_SPEED (m/s) for REST_SECONDS, or
## after MAX_SECONDS (GAMEPLAY-PLAN: 5 s).
const REST_SPEED := 0.1
const REST_SECONDS := 0.5
const MAX_SECONDS := 5.0
## Placeholders: damping of every body and the friction on the floor.
const BONE_DAMPING := 0.5
const BONE_FRICTION := 1.0
## A dead actor built in a place waits this long for the ground under it.
const GROUND_SECONDS := 10.0
## A frozen body's pickable box is its bones' box grown by this (metres).
const BOUNDS_MARGIN := 0.3

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _host: Node3D
var _player_position: Callable  # () -> Vector3: where the player is, for the direction of its hits
var _warn := WarnOnce.new("death: ")
var _hits := {}  # target ref -> impulse vector of the last hit
var _pending := {}  # ref -> seconds waited: dead, not yet lying down
var _bodies: Array[Dictionary] = []  # {ref, node, skeleton, simulator, bones, time, still}


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, host: Node3D, player_position: Callable) -> void:
	_player_position = player_position
	_world = world
	_papyrus = papyrus
	_host = host


## A hit landed (Combat.hit_landed): its direction is what a death takes along.
func on_hit(target: int, result: Dictionary, _point: Vector3) -> void:
	var attacker_ref := int(result.get("attacker", 0))
	var node := _world.find_ref_node(target) as Node3D
	var from := _player_position.call() as Vector3
	if attacker_ref != SkydotPapyrus.PLAYER_REF:
		var attacker := _world.find_ref_node(attacker_ref) as Node3D
		if attacker == null:
			return
		from = attacker.global_position
	if node == null:
		return
	var away := node.global_position - from
	away.y = 0.0
	if away.length() < 0.001:
		return
	_hits[target] = (away.normalized() + Vector3.UP * HIT_LIFT).normalized() * HIT_IMPULSE


## An actor died: its node lies down with the impulse of the last hit, from the
## next `step` (the hit that killed it is reported after the death).
func on_died(ref: int) -> void:
	_queue(ref)


## A place was built: the dead in it lie where their corpse place says, with
## no impulse, once there is ground under them.
func on_place_built(root: Node, _cell: int = 0) -> void:
	for node in root.find_children("*", "SkydotActor", true, false):
		if node.has_meta("skydot_ref"):
			var ref: int = node.get_meta("skydot_ref")
			if _papyrus.is_actor_dead(ref):
				_queue(ref)


func on_actor_arrived(ref: int) -> void:
	if _papyrus.is_actor_dead(ref):
		_queue(ref)


func _queue(ref: int) -> void:
	if not _pending.has(ref):
		_pending[ref] = 0.0


## Number of ragdolls still simulating.
func simulating() -> int:
	return _bodies.size()


## The frame: freeze the ragdolls that rest or have run their time.
func step(delta: float) -> void:
	_start_pending(delta)
	for body in _bodies.duplicate():
		if not is_instance_valid(body["node"]) or not is_instance_valid(body["skeleton"]):
			_bodies.erase(body)
			continue
		body["time"] += delta
		var moving := false
		for bone: PhysicalBone3D in body["bones"]:
			if bone.linear_velocity.length() > REST_SPEED:
				moving = true
		body["still"] = 0.0 if moving else body["still"] + delta
		if body["still"] >= REST_SECONDS or body["time"] >= MAX_SECONDS:
			_freeze(body)
			_bodies.erase(body)


# ---- lying down

## Start the ragdolls of the queued dead whose ground is there (or after
## GROUND_SECONDS: a cell whose collision never comes).
func _start_pending(delta: float) -> void:
	for ref: int in _pending.keys():
		_pending[ref] += delta
		var node := _world.find_ref_node(ref) as Node3D
		if node == null or not node.is_inside_tree():
			if _pending[ref] > GROUND_SECONDS:
				_pending.erase(ref)
			continue
		if _pending[ref] < GROUND_SECONDS and not _has_ground(node.global_position):
			continue
		_pending.erase(ref)
		_lay_down(ref, _hits.get(ref, Vector3.ZERO))
		_hits.erase(ref)


func _has_ground(at: Vector3) -> bool:
	var space := _host.get_world_3d().direct_space_state
	var query := PhysicsRayQueryParameters3D.create(at + Vector3.UP * 0.5, at + Vector3.DOWN * 2.0,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
	return not space.intersect_ray(query).is_empty()


func _lay_down(ref: int, impulse: Vector3) -> void:
	var node := _world.find_ref_node(ref) as SkydotActor
	if node == null or not node.is_inside_tree() or node.is_dead():
		return
	node.set_dead(true)
	var found := node.find_children("*", "Skeleton3D", true, false)
	var skeleton: Skeleton3D = found.front() if not found.is_empty() else null
	var ragdoll := _make_ragdoll(skeleton) if skeleton != null else {}
	if ragdoll.is_empty():
		var name := String(_world.get_actor_plan(ref).get("skeleton", "no skeleton"))
		_warn.once(name, "no ragdoll for %s" % name)
		node.rotate_object_local(Vector3.BACK, PI / 2.0)  # on its side
		_finish(ref, node, skeleton)
		return
	var simulator: PhysicalBoneSimulator3D = ragdoll["simulator"]
	skeleton.add_child(simulator)
	for bone: PhysicalBone3D in ragdoll["bones"]:
		simulator.add_child(bone)
	# A joint is made when the type changes in the tree, not when it enters it.
	for bone: PhysicalBone3D in ragdoll["bones"]:
		var type := bone.joint_type
		if type != PhysicalBone3D.JOINT_TYPE_NONE:
			bone.joint_type = PhysicalBone3D.JOINT_TYPE_NONE
			bone.joint_type = type
	simulator.physical_bones_start_simulation()
	if impulse != Vector3.ZERO:
		var target: PhysicalBone3D = ragdoll["impulse_bone"]
		if target != null:
			target.apply_central_impulse(impulse)
	_set_place(ref, node, node.global_position)
	_bodies.append({"ref": ref, "node": node, "skeleton": skeleton, "simulator": simulator,
		"bones": ragdoll["bones"], "time": 0.0, "still": 0.0})
	_mark_pickable(node, skeleton)


## Freeze the ragdoll into the bones' poses, free it, store the corpse place.
func _freeze(body: Dictionary) -> void:
	var skeleton: Skeleton3D = body["skeleton"]
	var node: Node3D = body["node"]
	var simulator: PhysicalBoneSimulator3D = body["simulator"]
	var poses := {}
	for bone: PhysicalBone3D in body["bones"]:
		poses[skeleton.find_bone(bone.bone_name)] = bone_pose(skeleton, bone)
	var pelvis := skeleton.find_bone(BONES[0][0])
	var pelvis_at := skeleton.global_transform * (poses[pelvis] as Transform3D).origin
	simulator.physical_bones_stop_simulation()
	skeleton.remove_child(simulator)
	simulator.free()
	# Local poses against parents worked out here (the skeleton's cached global
	# poses are stale while it simulates): a bone with a body takes the
	# simulated pose, any other follows its parent with the pose it had.
	var globals := {}
	for index in skeleton.get_bone_count():  # parents come before their children
		var parent := skeleton.get_bone_parent(index)
		var parent_global: Transform3D = globals[parent] if parent >= 0 else Transform3D.IDENTITY
		if poses.has(index):
			globals[index] = poses[index]
			skeleton.set_bone_pose(index, parent_global.affine_inverse() * poses[index])
		else:
			globals[index] = parent_global * skeleton.get_bone_pose(index)
	_set_place(body["ref"], node, pelvis_at)
	_mark_pickable(node, skeleton)


## Where the simulation has the bone, in the skeleton's space and units: the
## body's transform (no scale: Godot takes it off) put back on the bone.
static func bone_pose(skeleton: Skeleton3D, bone: PhysicalBone3D) -> Transform3D:
	var inverse := skeleton.global_transform.affine_inverse()
	var body := bone.global_transform
	var placed := Transform3D(inverse.basis.orthonormalized() * body.basis, inverse * body.origin)
	return placed * bone.body_offset.affine_inverse()


## A frozen or lying body: store where it is.
func _finish(ref: int, node: Node3D, skeleton: Skeleton3D) -> void:
	_set_place(ref, node, node.global_position)
	_mark_pickable(node, skeleton)


## The corpse place: the floor under `at`, in Skyrim units, and the facing.
## A dead actor built again stands there and falls the same way.
func _set_place(ref: int, node: Node3D, at: Vector3) -> void:
	var floor_at := at
	var space := _host.get_world_3d().direct_space_state
	var query := PhysicsRayQueryParameters3D.create(at + Vector3.UP * 0.5, at + Vector3.DOWN * 3.0,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
	var hit := space.intersect_ray(query)
	if not hit.is_empty():
		floor_at = hit["position"]
	var cell: int = _world.get_actor(ref).get("cell", 0)
	_papyrus.set_corpse_place(ref, _world.get_cell_space(cell), SkydotWorld.godot_to_skyrim(floor_at), node.rotation.y)


## The dead stay pickable: a box round the skeleton's bones in the node's own space.
func _mark_pickable(node: Node3D, skeleton: Skeleton3D) -> void:
	var bounds := AABB()
	var any := false
	if skeleton != null:
		var inverse := node.global_transform.affine_inverse()
		for i in skeleton.get_bone_count():
			var local := skeleton.get_bone_global_pose(i).origin
			if absf(local.x) > 1.0e4 or absf(local.y) > 1.0e4 or absf(local.z) > 1.0e4:
				continue  # a bone parked far away for a prop
			var at := inverse * (skeleton.global_transform * local)
			bounds = AABB(at, Vector3.ZERO) if not any else bounds.expand(at)
			any = true
	if not any:
		bounds = AABB(Vector3(-0.4, 0.0, -0.4), Vector3(0.8, 1.8, 0.8))
	node.set_meta("skydot_bounds", bounds.grow(BOUNDS_MARGIN))
	node.set_meta("skydot_activatable", true)


# ---- the ragdoll

## {simulator, bones, impulse_bone} for `skeleton`, or {} if it lacks the human
## bones. Nothing is added to the scene.
func _make_ragdoll(skeleton: Skeleton3D) -> Dictionary:
	for entry: Array in BONES:
		if skeleton.find_bone(entry[0]) < 0:
			return {}
	var simulator := PhysicalBoneSimulator3D.new()
	simulator.name = "Ragdoll"
	var units := _units_scale(skeleton)  # metres per skeleton unit
	var bones: Array[PhysicalBone3D] = []
	var impulse_bone: PhysicalBone3D = null
	for entry: Array in BONES:
		var bone := _make_bone(skeleton, entry, units)
		bones.append(bone)
		if entry[0] == IMPULSE_BONE:
			impulse_bone = bone
	# The bones do not collide with each other (capsules overlap at the
	# joints), nor with the player: only with the world.
	for i in bones.size():
		for j in range(i + 1, bones.size()):
			bones[i].add_collision_exception_with(bones[j])
			bones[j].add_collision_exception_with(bones[i])
	return {"simulator": simulator, "bones": bones, "impulse_bone": impulse_bone}


## One capsule from the bone to its child.
func _make_bone(skeleton: Skeleton3D, entry: Array, units: float) -> PhysicalBone3D:
	var index := skeleton.find_bone(entry[0])
	var from := skeleton.get_bone_global_rest(index)
	var length := HEAD_LENGTH / units
	var along := Vector3.UP
	if entry[1] != "":
		var child := skeleton.find_bone(entry[1])
		var vector := skeleton.get_bone_global_rest(child).origin - from.origin
		if child >= 0 and vector.length() > 0.001:
			length = vector.length()
			along = (from.basis.inverse() * vector).normalized()  # in the bone's frame
	var radius: float = entry[2]
	var bone := PhysicalBone3D.new()
	bone.name = String(entry[0]).get_slice(" [", 0).replace(" ", "")
	bone.bone_name = entry[0]
	bone.collision_layer = 0
	bone.collision_mask = SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN
	bone.linear_damp = BONE_DAMPING
	bone.angular_damp = BONE_DAMPING
	bone.friction = BONE_FRICTION
	bone.bounce = 0.0
	# Godot takes the scale off a body, so the shape and the joint are in
	# metres; the offset that places the body on the bone is in skeleton units.
	var length_m := length * units
	var shape := CapsuleShape3D.new()
	shape.radius = radius
	shape.height = maxf(length_m, 2.0 * radius + 0.001)
	var collision := CollisionShape3D.new()
	collision.shape = shape
	bone.add_child(collision)
	# The capsule's axis (Y) along the bone, its middle halfway to the child.
	var align := Basis(Quaternion(Vector3.UP, along))
	# (The body sits on the bone itself, with no offset in skeleton units: the
	# joints then agree with the bodies. The capsule is moved inside it, in metres.)
	collision.transform = Transform3D(align, along * (length_m * 0.5))
	var swing: float = entry[3]
	if swing > 0.0:
		bone.joint_type = PhysicalBone3D.JOINT_TYPE_CONE
		# The joint sits at the bone's origin, its twist axis (X) along the capsule.
		bone.set("joint_constraints/swing_span", swing)
		bone.set("joint_constraints/twist_span", entry[4])
		bone.joint_offset = Transform3D(Basis(Vector3.BACK, PI / 2.0), Vector3.ZERO)
	return bone


## Metres per unit of the skeleton: the scale of its node (the actor's units node).
static func _units_scale(skeleton: Skeleton3D) -> float:
	return maxf(skeleton.global_transform.basis.get_scale().x, 0.0001)
