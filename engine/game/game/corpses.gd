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
# `death: no ragdoll for <skeleton>` and lies on its side. Another skeleton (a
# creature's) gets a generic ragdoll: a capsule along every bone that has a child
# more than GENERIC_MIN_LENGTH away, cone joints of generic limits between a body and
# the nearest body above it; `death: ragdoll for <skeleton> (N bodies)` says how many.
#
# A living actor can be knocked down too (`knock_down`: a shout's push, PushActorAway): the same
# ragdoll with an impulse along source to target, but it does not die, is not a corpse (no corpse
# place, not pickable) and gets up: after GET_UP_SECONDS or once it rests, it stands where its
# pelvis lay, collision, animation and AI back (`push: 0x… gets up`). While it is down its node
# is "dead" for SkydotActor (nothing moves it) and the AI is held (`SkydotAi.hold`). If it dies
# meanwhile, the ragdoll goes on as a corpse's.
#
# The corpse stays pickable for activation (Search): `pick_ref` finds a node
# by the `skydot_activatable` and `skydot_bounds` metadata, which this sets on
# the actor's node (its cylinder is not on a physics layer any more).
class_name Corpses
extends RefCounted

## Writes the simulated bodies' poses into their bones without the skeleton's scale (see `_lay_down`).
class RagdollPose:
	extends SkeletonModifier3D
	var bones: Array[PhysicalBone3D] = []

	func _process_modification_with_delta(_delta: float) -> void:
		var skeleton := get_skeleton()
		if skeleton == null:
			return
		for bone in bones:  # parents come before their children
			if is_instance_valid(bone) and bone.is_inside_tree():
				var index := skeleton.find_bone(bone.bone_name)
				if index >= 0:
					skeleton.set_bone_global_pose(index, Corpses.bone_pose(skeleton, bone))


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
## The generic ragdoll (creatures): a bone gets a capsule when a child is farther than this
## (metres); the capsule's radius is this share of its length, within the limits; joint
## limits in degrees; at most this many bodies. Placeholders, sized by eye.
const GENERIC_MIN_LENGTH := 0.05
const GENERIC_RADIUS_SHARE := 0.25
const GENERIC_MIN_RADIUS := 0.03
const GENERIC_MAX_RADIUS := 0.5
const GENERIC_SWING := 45.0
const GENERIC_TWIST := 20.0
const GENERIC_MAX_BODIES := 128
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
## Placeholders: a push throws every body of the ragdoll at this many m/s per unit of its Papyrus force
## (PushActorAway's aiKnockbackForce, 15 for Unrelenting Force: 6 m/s), with this share of lift. The force's
## unit in Skyrim's Havok is unknown.
const PUSH_SPEED_PER_FORCE := 0.4
const PUSH_LIFT := 0.35
## Placeholder (the plan's guess): a knocked-down actor gets up after this long, or once it rests,
## but not before KNOCK_MIN_SECONDS.
const GET_UP_SECONDS := 2.0
const KNOCK_MIN_SECONDS := 1.0
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
var _live := {}  # ref -> true: died while built (not a body built dead): its death plays
var _bodies: Array[Dictionary] = []  # {ref, node, skeleton, simulator, bones, time, still, knocked}
## The AI to hold while an actor is down (the session sets it; null without one).
var ai: SkydotAi


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
	for body in _bodies:
		if body["ref"] == ref and body["knocked"]:
			body["knocked"] = false  # it dies where it lies: its ragdoll goes on as a corpse's
			body["time"] = 0.0
			return
	_live[ref] = true
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


## Reanimate: the body gets up. Its ragdoll (running or frozen into the bones' poses) is
## taken away, the skeleton goes back to its rest pose, it is no longer pickable as a corpse and
## the actor node lives again (`SkydotActor.set_dead(false)`); the AI is told by
## `SkydotAi.revive`. False if the actor is not built.
func stand_up(ref: int) -> bool:
	_pending.erase(ref)
	_hits.erase(ref)
	for body in _bodies.duplicate():
		if body["ref"] == ref:
			if is_instance_valid(body["simulator"]):
				body["simulator"].physical_bones_stop_simulation()
				if is_instance_valid(body["skeleton"]):
					body["skeleton"].remove_child(body["simulator"])
				body["simulator"].free()
			_bodies.erase(body)
	var node := _world.find_ref_node(ref) as SkydotActor
	if node == null or not node.is_inside_tree():
		return false
	for skeleton: Skeleton3D in node.find_children("*", "Skeleton3D", true, false):
		skeleton.reset_bone_poses()
	node.rotation.x = 0.0
	node.rotation.z = 0.0
	node.set_meta("skydot_activatable", false)
	node.remove_meta("skydot_bounds")
	node.set_dead(false)
	print("death: 0x%08X stands up" % ref)
	return true


# ---- knocked down

## `ref`, a living actor, is thrown away from `source` (an actor, or the player) with `force` (the
## PushActorAway force): it falls as a ragdoll and gets up later. False (and nothing happens) if it
## is dead, not built, already down, or has no ragdoll.
func knock_down(ref: int, source: int, force: float) -> bool:
	if _papyrus.is_actor_dead(ref) or is_down(ref):
		return false
	var node := _world.find_ref_node(ref) as Node3D
	if node == null or not node.is_inside_tree():
		return false
	var from: Vector3 = _player_position.call()
	if source != SkydotPapyrus.PLAYER_REF:
		var origin := _world.find_ref_node(source) as Node3D
		if origin != null:
			from = origin.global_position
	var away := node.global_position - from
	away.y = 0.0
	if away.length() < 0.001:
		away = -node.global_transform.basis.z  # on top of it: along where it looks
	away = away.normalized()
	var speed := (away + Vector3.UP * PUSH_LIFT).normalized() * force * PUSH_SPEED_PER_FORCE
	if not _lay_down(ref, speed, true):
		return false
	if ai != null:
		ai.hold(ref, GET_UP_SECONDS + MAX_SECONDS)  # until it stands again
	print("push: 0x%08X pushed by 0x%08X (force %s)" % [ref, source, force])
	return true


## Whether `ref` lies knocked down (alive).
func is_down(ref: int) -> bool:
	for body in _bodies:
		if body["ref"] == ref and body["knocked"]:
			return true
	return false


## The knocked-down body stands where its pelvis lay: the ragdoll goes, the bones are at rest, the
## node is alive for SkydotActor and the AI again.
func _get_up(body: Dictionary) -> void:
	var ref: int = body["ref"]
	var node: SkydotActor = body["node"]
	var skeleton: Skeleton3D = body["skeleton"]
	var simulator: PhysicalBoneSimulator3D = body["simulator"]
	var at := node.global_position
	for bone: PhysicalBone3D in body["bones"]:
		if bone.bone_name == body["root"]:
			at = skeleton.global_transform * bone_pose(skeleton, bone).origin
	simulator.physical_bones_stop_simulation()
	skeleton.remove_child(simulator)
	simulator.free()
	skeleton.reset_bone_poses()
	var query := PhysicsRayQueryParameters3D.create(at + Vector3.UP * 0.5, at + Vector3.DOWN * 3.0,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
	var hit := _host.get_world_3d().direct_space_state.intersect_ray(query)
	node.global_position = hit["position"] if not hit.is_empty() else Vector3(at.x, node.global_position.y, at.z)
	node.rotation.x = 0.0
	node.rotation.z = 0.0
	node.set_dead(false)
	if ai != null:
		ai.hold(ref, 0.0)
	print("push: 0x%08X gets up" % ref)


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
		if body["knocked"]:
			if body["time"] >= GET_UP_SECONDS or (body["time"] >= KNOCK_MIN_SECONDS and body["still"] >= REST_SECONDS):
				_get_up(body)
				_bodies.erase(body)
			continue
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
		# A race with a death clip does not wait for ground: it may die in the air and falls first.
		var posed := node is SkydotActor and (node as SkydotActor).has_death_clip()
		if not posed and _pending[ref] < GROUND_SECONDS and not _has_ground(node.global_position):
			continue
		_pending.erase(ref)
		_lay_down(ref, _hits.get(ref, Vector3.ZERO))
		_hits.erase(ref)


func _has_ground(at: Vector3) -> bool:
	var space := _host.get_world_3d().direct_space_state
	var query := PhysicsRayQueryParameters3D.create(at + Vector3.UP * 0.5, at + Vector3.DOWN * 2.0,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
	return not space.intersect_ray(query).is_empty()


## `impulse` is the hit that killed it, applied to one body; for `knocked` it is a velocity every body gets.
func _lay_down(ref: int, impulse: Vector3, knocked := false) -> bool:
	var node := _world.find_ref_node(ref) as SkydotActor
	if node == null or not node.is_inside_tree() or node.is_dead():
		return false
	if not knocked and node.has_death_clip():
		return _pose_death(ref, node)
	node.set_dead(true)
	return _ragdoll_body(ref, node, impulse, knocked, false)


## The generic ragdoll of `node`'s skeleton from the pose it has (`posed`: it plays or holds a death
## clip; the animation is let go of and a body without a ragdoll stays as it is).
func _ragdoll_body(ref: int, node: SkydotActor, impulse: Vector3, knocked: bool, posed: bool) -> bool:
	var found := node.find_children("*", "Skeleton3D", true, false)
	var skeleton: Skeleton3D = found.front() if not found.is_empty() else null
	if posed:
		var anim := node.get_node_or_null("AnimationPlayer") as AnimationPlayer
		if anim != null:
			anim.active = false  # the skeleton keeps the pose; the simulation takes it from there
	var ragdoll := _make_ragdoll(skeleton) if skeleton != null else {}
	if ragdoll.is_empty():
		var name := String(_world.get_actor_plan(ref).get("skeleton", "no skeleton"))
		_warn.once(name, "no ragdoll for %s" % name)
		if knocked:
			node.set_dead(false)  # no body to throw: it only staggers
			return false
		if not posed:
			node.rotate_object_local(Vector3.BACK, PI / 2.0)  # on its side
		_finish(ref, node, skeleton)
		return true
	var simulator: PhysicalBoneSimulator3D = ragdoll["simulator"]
	skeleton.add_child(simulator)
	for bone: PhysicalBone3D in ragdoll["bones"]:
		simulator.add_child(bone)
	# Godot's simulator writes a body's pose into the bone through the skeleton's inverse transform,
	# scale included: under the actor's units node (about 0.0147) a bone's basis comes out 68 times
	# too large and the skin explodes into huge planes. This modifier runs after it (it is the
	# skeleton's next child) and writes the right pose (`bone_pose`: no scale) over it; it goes when
	# the simulator does.
	var fix := RagdollPose.new()
	fix.bones = ragdoll["bones"]
	skeleton.add_child(fix)
	simulator.tree_exiting.connect(func() -> void:
		fix.active = false
		fix.queue_free())
	# A joint is made when the type changes in the tree, not when it enters it.
	for bone: PhysicalBone3D in ragdoll["bones"]:
		var type := bone.joint_type
		if type != PhysicalBone3D.JOINT_TYPE_NONE:
			bone.joint_type = PhysicalBone3D.JOINT_TYPE_NONE
			bone.joint_type = type
	simulator.physical_bones_start_simulation()
	if knocked:
		for bone: PhysicalBone3D in ragdoll["bones"]:
			bone.apply_central_impulse(impulse * bone.mass)
	elif impulse != Vector3.ZERO:
		var target: PhysicalBone3D = ragdoll["impulse_bone"]
		if target != null:
			target.apply_central_impulse(impulse)
	if not knocked:
		_set_place(ref, node, node.global_position)
	_bodies.append({"ref": ref, "node": node, "skeleton": skeleton, "simulator": simulator,
		"bones": ragdoll["bones"], "time": 0.0, "still": 0.0, "root": ragdoll["root"], "knocked": knocked})
	var skeleton_name := String(_world.get_actor_plan(ref).get("skeleton", "?"))
	_warn.once("ragdoll" + skeleton_name, "ragdoll for %s (%d bodies)" % [skeleton_name, ragdoll["bones"].size()])
	if not knocked:
		_mark_pickable(node, skeleton)
	return true


## A race with a death clip (the dragon's mtdeathintro) plays it once and holds the last frame; a flyer
## crashes first. A body built dead is posed at the end of the clip directly. When the pose is held
## (`SkydotActor.death_pose_done`) the generic ragdoll takes the body from that pose, which then rests,
## freezes and is stored as any creature's.
func _pose_death(ref: int, node: SkydotActor) -> bool:
	var rebuilt := not _live.has(ref)
	_live.erase(ref)
	node.death_pose_done.connect(_on_death_pose_done.bind(ref, node), CONNECT_ONE_SHOT)
	if not rebuilt:
		print("death: 0x%08X plays %s%s" % [ref, node.get_death_clip().get_file(), " after a crash" if node.is_flying() else ""])
	if not node.start_death(rebuilt):
		node.death_pose_done.disconnect(_on_death_pose_done.bind(ref, node))
		return false
	return true


func _on_death_pose_done(ref: int, node: SkydotActor) -> void:
	if not is_instance_valid(node) or not node.is_inside_tree():
		return
	var space := _host.get_world_3d().direct_space_state
	var query := PhysicsRayQueryParameters3D.create(node.global_position + Vector3.UP * 0.5,
		node.global_position + Vector3.DOWN * 200.0, SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
	var hit := space.intersect_ray(query)
	var above: float = node.global_position.y - float(hit["position"].y) if not hit.is_empty() else INF
	print("death: 0x%08X holds the death pose, %.2f m above the ground" % [ref, above])
	# The VM knows an NPC only where it was placed: tell it where the body lies (a crashed flyer is far
	# from where it flew; scripts measure distances to it).
	_papyrus.update_actor(ref, SkydotWorld.godot_to_skyrim(node.global_position))
	# The clip is only the intro: the body collapses and rests as other creatures' do, without a push.
	_ragdoll_body(ref, node, Vector3.ZERO, false, true)


## Freeze the ragdoll into the bones' poses, free it, store the corpse place.
func _freeze(body: Dictionary) -> void:
	var skeleton: Skeleton3D = body["skeleton"]
	var node: Node3D = body["node"]
	var simulator: PhysicalBoneSimulator3D = body["simulator"]
	var poses := {}
	for bone: PhysicalBone3D in body["bones"]:
		poses[skeleton.find_bone(bone.bone_name)] = bone_pose(skeleton, bone)
	var pelvis := skeleton.find_bone(body.get("root", BONES[0][0]))
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
	_papyrus.finish_dying(body["ref"])  # a body with a death clip: OnDeath comes now


## Where the simulation has the bone, in the skeleton's space and units: the
## body's transform (no scale: Godot takes it off) put back on the bone.
static func bone_pose(skeleton: Skeleton3D, bone: PhysicalBone3D) -> Transform3D:
	var inverse := skeleton.global_transform.affine_inverse()
	var body := bone.global_transform
	var placed := Transform3D(inverse.basis.orthonormalized() * body.basis, inverse * body.origin)
	return placed * bone.body_offset.affine_inverse()


## A frozen or lying body: store where it is.
func _finish(ref: int, node: Node3D, skeleton: Skeleton3D) -> void:
	_papyrus.finish_dying(ref)
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
	var entries := BONES
	var root_name: String = BONES[0][0]
	for entry: Array in BONES:
		if skeleton.find_bone(entry[0]) < 0:
			entries = _generic_entries(skeleton)
			if entries.is_empty():
				return {}
			root_name = entries[0][0]
			break
	var simulator := PhysicalBoneSimulator3D.new()
	simulator.name = "Ragdoll"
	var units := _units_scale(skeleton)  # metres per skeleton unit
	var bones: Array[PhysicalBone3D] = []
	var impulse_bone: PhysicalBone3D = null
	var longest := -1.0
	for entry: Array in entries:
		var bone := _make_bone(skeleton, entry, units)
		bones.append(bone)
		if entry[0] == IMPULSE_BONE:
			impulse_bone = bone
		elif entries != BONES and bone.get_child(0) is CollisionShape3D:
			var height: float = ((bone.get_child(0) as CollisionShape3D).shape as CapsuleShape3D).height
			if height > longest:
				longest = height
				impulse_bone = bone
	# The bones do not collide with each other (capsules overlap at the
	# joints), nor with the player: only with the world.
	for i in bones.size():
		for j in range(i + 1, bones.size()):
			bones[i].add_collision_exception_with(bones[j])
			bones[j].add_collision_exception_with(bones[i])
	return {"simulator": simulator, "bones": bones, "impulse_bone": impulse_bone, "root": root_name}


## The entries (as BONES') of a generic ragdoll: every bone with a child more than
## GENERIC_MIN_LENGTH away, a capsule to the farthest such child, a joint to the nearest
## body above (none for a bone with no body above it).
func _generic_entries(skeleton: Skeleton3D) -> Array:
	var units := _units_scale(skeleton)
	var out: Array = []
	var has_body := {}  # bone index -> true
	for index in skeleton.get_bone_count():  # parents come before their children
		var here := skeleton.get_bone_global_rest(index).origin
		if absf(here.x) > 1.0e4 or absf(here.y) > 1.0e4 or absf(here.z) > 1.0e4:
			continue  # a bone parked far away for a prop
		var far := -1
		var far_length := GENERIC_MIN_LENGTH
		for child in skeleton.get_bone_children(index):
			var length := (skeleton.get_bone_global_rest(child).origin - here).length() * units
			if length > far_length:
				far_length = length
				far = child
		if far < 0 or out.size() >= GENERIC_MAX_BODIES:
			continue
		var above := false
		var parent := skeleton.get_bone_parent(index)
		while parent >= 0 and not above:
			above = has_body.has(parent)
			parent = skeleton.get_bone_parent(parent)
		has_body[index] = true
		var radius := clampf(far_length * GENERIC_RADIUS_SHARE, GENERIC_MIN_RADIUS, GENERIC_MAX_RADIUS)
		out.append([skeleton.get_bone_name(index), skeleton.get_bone_name(far), radius,
			GENERIC_SWING if above else 0.0, GENERIC_TWIST if above else 0.0])
	return out


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
