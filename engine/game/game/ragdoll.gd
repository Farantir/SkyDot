# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game's ragdoll of one skeleton, built from the race's skeleton model (`SkydotWorld.get_ragdoll`:
# formats/pack-format.md, "Ragdolls in meshes"). Every body of the data becomes a `PhysicalBone3D` on
# its bone (a bone without a body follows its parent), with the data's shapes, mass, damping, friction
# and restitution; every constraint a 6DOF joint made with PhysicsServer3D from the data's frames in
# each body (A the child, B the parent), so the limits do not depend on the pose the ragdoll starts
# from. Lengths and masses scale with the actor (the game's masses are linear in its scale).
#
# Joints (Jolt's generic 6DOF; godot-notes.md has what was measured):
# - ragdoll constraint: X the twist axis, limited to [twist_min, twist_max]; Y the plane axis, limited
#   to the cone (a turn about it keeps the twist axis in the plane); Z = twist x plane, limited to the
#   plane limits within the cone (a turn about it tilts the twist axis toward the plane axis). Havok's
#   cone is round and its plane limits are measured as asin(twist_A . plane_B); Godot's limits are per
#   axis, so the reachable set differs at the cone's diagonals (CONE_GAP_DEGREES).
# - limited hinge (and hinge): X the axle, limited to [min_angle, max_angle]; Y and Z locked. A 6DOF
#   rather than Godot's hinge, because only the 6DOF carries a motor force limit per axis, which is
#   what Havok's constraint friction is.
# - max_friction: an angular motor of target velocity 0 and force limit max_friction on every free axis.
# - ball and socket: the angular axes free; stiff spring and prismatic are not made (no vanilla actor
#   has one; `skipped` counts them).
#
# Self-collision is the game's: within one ragdoll a pair of bodies collides iff the biped table has
# the second's part number in the first's entry (BIPED_BITFIELDS); every other pair gets a collision
# exception. Ragdolls of different actors collide (the game's DEADBIP layer collides with itself), and
# with the world and clutter, not with the living.
class_name Ragdoll
extends RefCounted

## The game's dead-biped self-collision table: bhkCollisionFilter's biped_bitfields, read from Skyrim
## SE's running filter with SkyrimRemote `@ragdoll` (2026-11-08, skycompare-data/game-ragdoll/
## a_human_rest.json, filter.biped_bitfields; the filter's answer matched it on every pair of the
## human, wolf, spider and dragon). Entry a, bit b: parts a and b collide.
const BIPED_BITFIELDS: Array[int] = [
	0x00000000, 0x004230C0, 0x014030C0, 0x014030C0, 0x014230C0, 0x00403000, 0x0042791E, 0x0041FF1E,
	0x0145F0C0, 0x0145E080, 0x0144E080, 0x004000C0, 0x004241FE, 0x0045C7FE, 0x014437C0, 0x01442780,
	0x01442380, 0x00401052, 0x0041E700, 0x00000000, 0x00400000, 0x00400000, 0x0037FFFE, 0x00000000,
	0x0001C71C, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
]
## The part number in a body's Havok filter: bits 0-4 of flags_and_part (the game's filter info bits
## 8-12, as the @ragdoll dumps decode them).
const PART_MASK := 0x1F
## Measured (tests/gd/test_ragdoll_joints.gd, Godot 4.7.2 with Jolt, 2026-11-09): how far, in degrees,
## the twist axis of the 6DOF can stand from the set Havok's cone and plane allow (the human hip's:
## cone 45°, plane -50..15°). A child turned by a constant torque toward 189 directions stopped up to
## 31.9° past the cone and plane (at the cone's diagonals the per-axis limits add up to about 60°, and
## the plane limit gives way by up to 13°) and up to 21.7° short of a direction Havok allows. Twist,
## hinge limits and the friction motor hold exactly.
const CONE_GAP_DEGREES := 35.0
## Metres per game unit.
const METRES_PER_UNIT := 0.0142875
## A capsule shorter than this (metres) is a sphere.
const MIN_SEGMENT := 0.0001

var skeleton: Skeleton3D
var simulator: PhysicalBoneSimulator3D
## One per data body, in the data's order; null where the skeleton lacks the bone.
var bones: Array[PhysicalBone3D] = []
var data: Dictionary
## Lengths and masses are the data's times this (the actor's scale).
var scale := 1.0
## The joints made (PhysicsServer3D RIDs), freed by `stop`.
var joints: Array[RID] = []
## Constraints left out (a body missing, a kind not made).
var skipped := 0
## Each body's centre of mass in its own frame (metres), set by `start`.
var centres: Array[Vector3] = []
## Each body's parent body (the B of the constraint whose A it is), -1 for the root.
var parents: Array[int] = []
## The rigid-body controller (`set_drive`): the control data (empty: none), the bodies it drives, and
## per driven body what the last step wanted ({com, rot, v, w}).
var drive_values := PackedFloat32Array()
var driven: Array[int] = []
var _drive_last := {}


## The ragdoll of `skeleton` from `ragdoll` (SkydotWorld.get_ragdoll), or null if no body's bone is on
## it. Nothing is added to the scene until `start`.
static func make(on: Skeleton3D, ragdoll: Dictionary) -> Ragdoll:
	var bodies: Array = ragdoll.get("bodies", [])
	if on == null or bodies.is_empty():
		return null
	var out := Ragdoll.new()
	out.skeleton = on
	out.data = ragdoll
	var units := maxf(on.global_transform.basis.get_scale().x, 1.0e-6)  # metres per skeleton unit
	out.scale = units / METRES_PER_UNIT
	out.simulator = PhysicalBoneSimulator3D.new()
	out.simulator.name = "Ragdoll"
	var any := false
	for body: Dictionary in bodies:
		var bone := out._make_bone(body, units) if on.find_bone(String(body.get("bone", ""))) >= 0 else null
		out.bones.append(bone)
		any = any or bone != null
	if not any:
		out.simulator.free()
		return null
	out._exclude_pairs()
	return out


## Clutter bodies one shape of a ragdoll body wakes at most per frame.
const MAX_CONTACTS := 8


## The bodies made (bones with a body).
func body_count() -> int:
	var n := 0
	for bone in bones:
		if bone != null:
			n += 1
	return n


## Into the scene under the skeleton, simulating from the pose it has, joints made.
func start() -> void:
	skeleton.add_child(simulator)
	for bone in bones:
		if bone != null:
			simulator.add_child(bone)
	simulator.physical_bones_start_simulation()
	var bodies: Array = data.get("bodies", [])
	centres.resize(bones.size())
	for i in bones.size():
		var bone := bones[i]
		if bone == null:
			continue
		var body: Dictionary = bodies[i]
		# Godot's body has its centre of mass at the shapes' centre: the data's (bone frame, metres) here.
		var com: Vector3 = _body_frame(body).affine_inverse() * (Vector3(body.get("center_of_mass", Vector3.ZERO)) * scale)
		centres[i] = com
		PhysicsServer3D.body_set_param(bone.get_rid(), PhysicsServer3D.BODY_PARAM_CENTER_OF_MASS, com)
		var inertia: Basis = body.get("inertia", Basis())
		var diagonal := Vector3(inertia.x.x, inertia.y.y, inertia.z.z) * scale * scale * scale  # m * r^2: s * s^2
		if diagonal.x > 0.0 and diagonal.y > 0.0 and diagonal.z > 0.0:
			PhysicsServer3D.body_set_param(bone.get_rid(), PhysicsServer3D.BODY_PARAM_INERTIA, diagonal)
	parents.resize(bones.size())
	parents.fill(-1)
	for constraint: Dictionary in data.get("constraints", []):
		var child := int(constraint.get("body_a", -1))
		if child >= 0 and child < parents.size():
			parents[child] = int(constraint.get("body_b", -1))
		var joint := make_joint(constraint)
		if joint.is_valid():
			joints.append(joint)
		else:
			skipped += 1


## Out of the scene: the joints freed, the simulation stopped, the simulator freed.
func stop() -> void:
	for joint in joints:
		PhysicsServer3D.free_rid(joint)
	joints.clear()
	if is_instance_valid(simulator):
		simulator.physical_bones_stop_simulation()
		if simulator.get_parent() != null:
			simulator.get_parent().remove_child(simulator)
		simulator.free()


## The joint of `constraint` (one of the data's) between its two bodies, or an invalid RID.
func make_joint(constraint: Dictionary) -> RID:
	var a := int(constraint.get("body_a", -1))
	var b := int(constraint.get("body_b", -1))
	if a < 0 or b < 0 or a >= bones.size() or b >= bones.size() or bones[a] == null or bones[b] == null:
		return RID()
	var bodies: Array = data.get("bodies", [])
	return joint_between(constraint, bones[a].get_rid(), _body_frame(bodies[a]), bones[b].get_rid(), _body_frame(bodies[b]), scale)


## A 6DOF joint for `constraint` between bodies `a` (the child) and `b` (the parent), whose frames in
## their bones are `frame_a` and `frame_b` (metres, at the actor's scale); `scale` scales the pivots.
## Invalid for a kind that is not made. (Static, so the joint test makes the same joints.)
static func joint_between(constraint: Dictionary, a: RID, frame_a: Transform3D, b: RID, frame_b: Transform3D,
		size := 1.0) -> RID:
	var type := String(constraint.get("type", ""))
	var axes := []
	match type:
		"ragdoll":
			axes = ["twist", "plane"]
		"limited_hinge", "hinge":
			axes = ["axle", "perp_1"]
		"ball_and_socket":
			axes = []
		_:
			return RID()
	var local_a := frame_a.affine_inverse() * _frame(constraint.get("in_a", {}), axes, size)
	var local_b := frame_b.affine_inverse() * _frame(constraint.get("in_b", {}), axes, size)
	var joint := PhysicsServer3D.joint_create()
	PhysicsServer3D.joint_make_generic_6dof(joint, a, local_a, b, local_b)
	PhysicsServer3D.joint_disable_collisions_between_bodies(joint, true)
	var limits := {}  # axis -> [lower, upper], radians; an axis not here is free
	match type:
		"ragdoll":
			var cone := float(constraint.get("cone_max", 0.0))
			limits[Vector3.AXIS_X] = [float(constraint.get("twist_min", 0.0)), float(constraint.get("twist_max", 0.0))]
			limits[Vector3.AXIS_Y] = [-cone, cone]
			limits[Vector3.AXIS_Z] = [maxf(float(constraint.get("plane_min", 0.0)), -cone), minf(float(constraint.get("plane_max", 0.0)), cone)]
		"limited_hinge":
			limits[Vector3.AXIS_X] = [float(constraint.get("min_angle", 0.0)), float(constraint.get("max_angle", 0.0))]
			limits[Vector3.AXIS_Y] = [0.0, 0.0]
			limits[Vector3.AXIS_Z] = [0.0, 0.0]
		"hinge":
			limits[Vector3.AXIS_Y] = [0.0, 0.0]
			limits[Vector3.AXIS_Z] = [0.0, 0.0]
	var friction := float(constraint.get("max_friction", 0.0))
	for axis in [Vector3.AXIS_X, Vector3.AXIS_Y, Vector3.AXIS_Z]:
		# The pivots stay together.
		PhysicsServer3D.generic_6dof_joint_set_flag(joint, axis, PhysicsServer3D.G6DOF_JOINT_FLAG_ENABLE_LINEAR_LIMIT, true)
		PhysicsServer3D.generic_6dof_joint_set_param(joint, axis, PhysicsServer3D.G6DOF_JOINT_LINEAR_LOWER_LIMIT, 0.0)
		PhysicsServer3D.generic_6dof_joint_set_param(joint, axis, PhysicsServer3D.G6DOF_JOINT_LINEAR_UPPER_LIMIT, 0.0)
		var limited := limits.has(axis)
		PhysicsServer3D.generic_6dof_joint_set_flag(joint, axis, PhysicsServer3D.G6DOF_JOINT_FLAG_ENABLE_ANGULAR_LIMIT, limited)
		if limited:
			# Jolt measures A's frame against B's as Havok does (test_ragdoll_joints.gd): the limits go over as they are.
			PhysicsServer3D.generic_6dof_joint_set_param(joint, axis, PhysicsServer3D.G6DOF_JOINT_ANGULAR_LOWER_LIMIT, float(limits[axis][0]))
			PhysicsServer3D.generic_6dof_joint_set_param(joint, axis, PhysicsServer3D.G6DOF_JOINT_ANGULAR_UPPER_LIMIT, float(limits[axis][1]))
		var free_axis: bool = not limited or float(limits[axis][1]) > float(limits[axis][0])
		var motor := friction > 0.0 and free_axis
		PhysicsServer3D.generic_6dof_joint_set_flag(joint, axis, PhysicsServer3D.G6DOF_JOINT_FLAG_ENABLE_MOTOR, motor)
		if motor:
			PhysicsServer3D.generic_6dof_joint_set_param(joint, axis, PhysicsServer3D.G6DOF_JOINT_ANGULAR_MOTOR_TARGET_VELOCITY, 0.0)
			PhysicsServer3D.generic_6dof_joint_set_param(joint, axis, PhysicsServer3D.G6DOF_JOINT_ANGULAR_MOTOR_FORCE_LIMIT, friction)  # as stored: whether the game scales it is not known
	return joint


## A constraint's frame in its bone (`in_a`/`in_b`): X and Y the named axes, Z their cross product,
## at the pivot (times `size`).
static func _frame(frame: Dictionary, axes: Array, size: float) -> Transform3D:
	var pivot: Vector3 = Vector3(frame.get("pivot", Vector3.ZERO)) * size
	if axes.is_empty():
		return Transform3D(Basis.IDENTITY, pivot)
	var x: Vector3 = Vector3(frame.get(axes[0], Vector3.RIGHT)).normalized()
	var y: Vector3 = Vector3(frame.get(axes[1], Vector3.UP))
	y = (y - x * x.dot(y)).normalized()  # the data's axes are perpendicular to float precision
	return Transform3D(Basis(x, y, x.cross(y)), pivot)


## The body's frame in its bone, metres at the actor's scale.
func _body_frame(body: Dictionary) -> Transform3D:
	var t: Transform3D = body.get("body_transform", Transform3D.IDENTITY)
	return Transform3D(t.basis.orthonormalized(), t.origin * scale)


## The body of one data entry on its bone: shapes, mass, damping, friction and restitution.
func _make_bone(body: Dictionary, units: float) -> PhysicalBone3D:
	var bone := PhysicalBone3D.new()
	var bone_name := String(body.get("bone", ""))
	bone.name = bone_name.validate_node_name()
	bone.bone_name = bone_name
	bone.collision_layer = SkydotPlayer.LAYER_DEAD_BIPED
	bone.collision_mask = SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN | SkydotPlayer.LAYER_CLUTTER | SkydotPlayer.LAYER_DEAD_BIPED
	bone.mass = maxf(float(body.get("mass", 1.0)) * scale, 0.01)
	bone.linear_damp_mode = PhysicalBone3D.DAMP_MODE_REPLACE
	bone.angular_damp_mode = PhysicalBone3D.DAMP_MODE_REPLACE
	bone.linear_damp = float(body.get("linear_damping", 0.0))
	bone.angular_damp = float(body.get("angular_damping", 0.0))
	# Godot takes 0..1 only; a skeleton that stores more (seen in sse_combat_death) keeps 1.
	bone.friction = clampf(float(body.get("friction", 0.5)), 0.0, 1.0)
	bone.bounce = clampf(float(body.get("restitution", 0.0)), 0.0, 1.0)
	# The body's frame on the bone: the offset is in skeleton units (Godot takes the scale off the body,
	# so shapes and joints are in metres).
	var frame := _body_frame(body)
	bone.body_offset = Transform3D(frame.basis, frame.origin / units)
	var to_body := frame.affine_inverse()
	for shape: Dictionary in body.get("shapes", []):
		var collision := _shape(shape)
		if collision != null:
			collision.transform = to_body * collision.transform
			bone.add_child(collision)
	return bone


## A data shape (bone frame, metres) as a CollisionShape3D in the bone's frame at the actor's scale;
## null for a kind that has no Godot shape.
func _shape(shape: Dictionary) -> CollisionShape3D:
	var placed: Transform3D = shape.get("transform", Transform3D.IDENTITY)
	placed = Transform3D(placed.basis.orthonormalized(), placed.origin * scale)
	var radius := float(shape.get("radius", 0.0)) * scale
	var out := CollisionShape3D.new()
	match String(shape.get("kind", "")):
		"capsule", "cylinder":
			var a: Vector3 = Vector3(shape.get("point_a", Vector3.ZERO)) * scale
			var b: Vector3 = Vector3(shape.get("point_b", Vector3.ZERO)) * scale
			var along := b - a
			var length := along.length()
			var turn := Basis(Quaternion(Vector3.UP, along / length)) if length > MIN_SEGMENT else Basis.IDENTITY
			if String(shape["kind"]) == "capsule":
				var capsule := CapsuleShape3D.new()
				capsule.radius = radius
				capsule.height = length + 2.0 * radius
				out.shape = capsule
			else:
				var cylinder := CylinderShape3D.new()
				cylinder.radius = radius
				cylinder.height = maxf(length, MIN_SEGMENT)
				out.shape = cylinder
			out.transform = placed * Transform3D(turn, (a + b) * 0.5)
		"sphere":
			var sphere := SphereShape3D.new()
			sphere.radius = radius
			out.shape = sphere
			out.transform = placed
		"box":
			var box := BoxShape3D.new()
			box.size = (Vector3(shape.get("half_extents", Vector3.ZERO)) * scale + Vector3.ONE * radius) * 2.0
			out.shape = box
			out.transform = placed
		"convex_vertices":
			var hull := ConvexPolygonShape3D.new()
			var points := PackedVector3Array()
			for p: Vector3 in shape.get("vertices", PackedVector3Array()):
				points.append(p * scale)
			hull.points = points
			out.shape = hull
			out.transform = placed
		_:
			out.free()
			return null
	return out


## Collision exceptions for every pair of bodies the game's biped table keeps apart.
func _exclude_pairs() -> void:
	var bodies: Array = data.get("bodies", [])
	for i in bones.size():
		for j in range(i + 1, bones.size()):
			if bones[i] == null or bones[j] == null:
				continue
			if not collides(int(bodies[i].get("flags_and_part", 0)), int(bodies[j].get("flags_and_part", 0))):
				bones[i].add_collision_exception_with(bones[j])
				bones[j].add_collision_exception_with(bones[i])


## Whether two bodies of one ragdoll with these Havok filter flags (flags_and_part) collide.
static func collides(flags_a: int, flags_b: int) -> bool:
	var a := flags_a & PART_MASK
	var b := flags_b & PART_MASK
	return (BIPED_BITFIELDS[a] >> b) & 1 == 1


## The body nearest to `point` (global): the one a hit there lands on.
func nearest(point: Vector3) -> PhysicalBone3D:
	var best: PhysicalBone3D = null
	var best_distance := INF
	for bone in bones:
		if bone != null:
			var d := bone.global_position.distance_squared_to(point)
			if d < best_distance:
				best_distance = d
				best = bone
	return best


## The mass-weighted centre of the bodies (global).
func centre() -> Vector3:
	var sum := Vector3.ZERO
	var mass := 0.0
	for bone in bones:
		if bone != null:
			sum += bone.global_position * bone.mass
			mass += bone.mass
	return sum / mass if mass > 0.0 else skeleton.global_position


## Sets every body's velocity to the motion of one rigid body: `linear` at the centre of mass plus
## `spin` (rad/s, global axis) about it.
func set_motion(linear: Vector3, spin: Vector3) -> void:
	var at := centre()
	for bone in bones:
		if bone != null:
			var v := linear + spin.cross(bone.global_position - at)
			PhysicsServer3D.body_set_state(bone.get_rid(), PhysicsServer3D.BODY_STATE_LINEAR_VELOCITY, v)
			PhysicsServer3D.body_set_state(bone.get_rid(), PhysicsServer3D.BODY_STATE_ANGULAR_VELOCITY, spin)


## Frozen clutter (SkydotDynamicBody, static until woken) that a body touches or reaches within this
## frame's motion starts moving, as a sleeping Havok body is a dynamic one the moment a body hits it.
## Woken only on contact it would stand as a wall for that step (the Bleak Falls spider fell off its
## course on the bones in its shaft). Called once a frame with the frame's `delta`.
func wake_clutter(delta: float) -> void:
	var space := skeleton.get_world_3d().direct_space_state if skeleton.is_inside_tree() else null
	if space == null:
		return
	var query := PhysicsShapeQueryParameters3D.new()
	query.collision_mask = SkydotPlayer.LAYER_CLUTTER
	for bone in bones:
		if bone == null or not bone.is_inside_tree():
			continue
		query.margin = bone.linear_velocity.length() * delta
		for child in bone.get_children():
			var shape := child as CollisionShape3D
			if shape == null or shape.shape == null:
				continue
			query.shape = shape.shape
			query.transform = shape.global_transform
			for hit in space.intersect_shape(query, MAX_CONTACTS):
				var other: Object = hit.get("collider")
				if other is SkydotDynamicBody and (other as SkydotDynamicBody).freeze:
					(other as SkydotDynamicBody).wake()


# ---- the rigid-body controller

## The behaviour graph's ragdoll modifiers active now (SkydotWorld.get_ragdoll_controls, outermost
## first) drive the ragdoll as the game's hkbRagdollDriver does: the bones of the
## hkbKeyframeBonesModifiers (every bone if one lists none) are the "keyframed" ones, which the
## rigid-body controller (hkaRagdollRigidBodyController, the hkbRigidBodyRagdollControlsModifier's
## hkaKeyFrameHierarchyUtility control data; the outermost modifier writes last, so it wins) moves
## toward the animation's pose by setting the dynamic bodies' velocities; the other bones are limp.
## Only the graph's own states' modifiers: those behind an event (hkbEventDrivenModifier, e.g. the
## powered "PoweredRagdollMatching" of the get-up) are not decoded. `skeleton_rest` maps a ragdoll
## skeleton bone (hkx `Ragdoll_*` name) to a body: by name without the prefix, else the body whose bone
## stands nearest to it at rest. False, and no drive, without a rigid-body control or a driven body.
func set_drive(controls: Array) -> bool:
	drive_values = PackedFloat32Array()
	driven.clear()
	_drive_last.clear()
	var keyframed := {}
	var all := false
	var any_keyframes := false
	for control: Dictionary in controls:
		match String(control.get("kind", "")):
			"rigid_body":
				if drive_values.is_empty():
					drive_values = control.get("values", PackedFloat32Array())
			"keyframed_bones":
				any_keyframes = true
				var listed: Array = control.get("bones", [])
				if listed.is_empty():
					all = true
				for bone: Dictionary in listed:
					var body := _body_of_ragdoll_bone(String(bone.get("name", "")), Vector3(bone.get("position", Vector3.ZERO)))
					if body >= 0:
						keyframed[body] = true
	if drive_values.size() < 12 or not any_keyframes:
		drive_values = PackedFloat32Array()
		return false
	for i in bones.size():
		if bones[i] != null and (all or keyframed.has(i)):
			driven.append(i)
	return not driven.is_empty()


## The drive's end: the bodies go limp.
func end_drive() -> void:
	drive_values = PackedFloat32Array()
	driven.clear()
	_drive_last.clear()


## The body of the ragdoll skeleton bone `hkx_name` at `at` (model space, game units, Z up, as the
## skeleton's own rest poses); -1 if none.
func _body_of_ragdoll_bone(hkx_name: String, at: Vector3) -> int:
	var name := hkx_name.trim_prefix("Ragdoll_")
	var bodies: Array = data.get("bodies", [])
	for i in bones.size():
		if bones[i] != null and String(bodies[i].get("bone", "")) == name:
			return i
	var best := -1
	var best_distance := INF
	for i in bones.size():
		if bones[i] == null:
			continue
		var index := skeleton.find_bone(bones[i].bone_name)
		var d := skeleton.get_bone_global_rest(index).origin.distance_to(at)  # both Skyrim's model space
		if d < best_distance:
			best_distance = d
			best = i
	return best


## One step of the rigid-body controller toward `poses` (skeleton bone index -> the animation's global
## pose in the skeleton's space), `delta` seconds after the last. hkaKeyFrameHierarchyUtility, its
## ControlData in Havok units (metres, radians), as its members are documented and activeragdoll
## (github.com/adamhynek/activeragdoll, hkaKeyFrameHierarchyUtility_CalculateApplyKeyframeData)
## re-implements the target: a body's target is its animated pose, pulled by hierarchyGain toward its
## parent body's actual pose times the animated pose relative to the parent. Then its velocity:
## velocityDamping takes that share off, accelerationGain adds that share of the change of the
## target's velocity, velocityGain moves that share of the way to the target's velocity, positionGain
## adds that share of the position error per step (at most positionMax*Velocity), snapGain the same
## at full strength within snapMax*Distance and scaled down beyond (at most snapMax*Velocity).
func drive(poses: Dictionary, delta: float) -> void:
	if drive_values.is_empty() or delta <= 0.0:
		return
	var v := drive_values
	var hierarchy_gain := v[0]
	var targets := {}
	for i in driven:
		var bone := bones[i]
		var index := skeleton.find_bone(bone.bone_name)
		if not poses.has(index) or not is_instance_valid(bone) or not bone.is_inside_tree():
			continue
		targets[i] = _body_at(poses[index], bone)
	for i: int in targets:
		var bone := bones[i]
		var target: Transform3D = targets[i]
		var parent := parents[i] if i < parents.size() else -1
		if hierarchy_gain > 0.0 and parent >= 0 and targets.has(parent) and bones[parent] != null:
			var parent_target: Transform3D = targets[parent]
			var follow := bones[parent].global_transform * (parent_target.affine_inverse() * target)
			target = Transform3D(Basis(target.basis.get_rotation_quaternion().slerp(
				follow.basis.get_rotation_quaternion(), hierarchy_gain)), target.origin.lerp(follow.origin, hierarchy_gain))
		var com := target * centres[i]
		var rot := target.basis.get_rotation_quaternion()
		var last: Dictionary = _drive_last.get(i, {"com": com, "rot": rot, "v": Vector3.ZERO, "w": Vector3.ZERO})
		var want_v: Vector3 = (com - Vector3(last["com"])) / delta
		var want_w: Vector3 = _rotation_vector(last["rot"], rot) / delta
		var now := bone.global_transform
		var lin := _gain_velocity(bone.linear_velocity, want_v, last["v"], com - now * centres[i], delta, v, 5, 8, 10)
		var ang := _gain_velocity(bone.angular_velocity, want_w, last["w"],
			_rotation_vector(now.basis.get_rotation_quaternion(), rot), delta, v, 6, 9, 11)
		PhysicsServer3D.body_set_state(bone.get_rid(), PhysicsServer3D.BODY_STATE_LINEAR_VELOCITY, lin)
		PhysicsServer3D.body_set_state(bone.get_rid(), PhysicsServer3D.BODY_STATE_ANGULAR_VELOCITY, ang)
		_drive_last[i] = {"com": com, "rot": rot, "v": want_v, "w": want_w}


## The velocity the controller gives a body (linear or angular): `current`, the target's velocity
## `want` and its last `before`, the position (or rotation vector) error `error`; `max_at`, `snap_max_at`
## and `snap_distance_at` index the position, snap velocity and snap distance limits in `v`.
static func _gain_velocity(current: Vector3, want: Vector3, before: Vector3, error: Vector3, delta: float,
		v: PackedFloat32Array, max_at: int, snap_max_at: int, snap_distance_at: int) -> Vector3:
	var out := current * (1.0 - v[1])
	out += (want - before) * v[2]
	out += (want - out) * v[3]
	out += (error * (v[4] / delta)).limit_length(v[max_at])
	var distance := error.length()
	var snap := v[7] * (1.0 if distance <= v[snap_distance_at] else v[snap_distance_at] / distance)
	out += (error * (snap / delta)).limit_length(v[snap_max_at])
	return out


## The global transform the body of `bone` has when its bone has the skeleton-space pose `pose` (as
## the simulator places it: no scale).
func _body_at(pose: Transform3D, bone: PhysicalBone3D) -> Transform3D:
	var g := skeleton.global_transform
	var placed := pose * bone.body_offset
	return Transform3D(g.basis.orthonormalized() * placed.basis.orthonormalized(), g * placed.origin)


## The rotation (axis times angle, radians) that turns `from` into `to`, the short way.
static func _rotation_vector(from: Quaternion, to: Quaternion) -> Vector3:
	var turn := to * from.inverse()
	if turn.w < 0.0:
		turn = -turn
	var angle := turn.get_angle()
	return turn.get_axis() * angle if angle > 1.0e-6 else Vector3.ZERO


## Havok's angles of `constraint` with its bodies turned by `turn_a` and `turn_b` (their frames'
## global bases): ragdoll {cone, plane, twist}, hinge {angle}, radians. As the game measures them
## (checked on its rest poses, skycompare-data/game-ragdoll): cone the angle between the two twist
## axes, plane asin(twist_A . plane_B), twist and hinge angles A's turn against B's about A's axis.
static func angles(constraint: Dictionary, turn_a: Basis, turn_b: Basis) -> Dictionary:
	var in_a: Dictionary = constraint.get("in_a", {})
	var in_b: Dictionary = constraint.get("in_b", {})
	match String(constraint.get("type", "")):
		"ragdoll":
			var twist_a: Vector3 = (turn_a * Vector3(in_a.get("twist", Vector3.RIGHT))).normalized()
			var twist_b: Vector3 = (turn_b * Vector3(in_b.get("twist", Vector3.RIGHT))).normalized()
			var plane_a: Vector3 = turn_a * Vector3(in_a.get("plane", Vector3.UP))
			var plane_b: Vector3 = turn_b * Vector3(in_b.get("plane", Vector3.UP))
			return {"cone": acos(clampf(twist_a.dot(twist_b), -1.0, 1.0)),
				"plane": asin(clampf(twist_a.dot(plane_b.normalized()), -1.0, 1.0)),
				"twist": _turn_about(twist_a, plane_b, plane_a)}
		"limited_hinge", "hinge":
			var axle: Vector3 = (turn_a * Vector3(in_a.get("axle", Vector3.RIGHT))).normalized()
			return {"angle": _turn_about(axle, turn_b * Vector3(in_b.get("perp_1", Vector3.UP)),
				turn_a * Vector3(in_a.get("perp_1", Vector3.UP)))}
	return {}


## How far (radians) `measured` (from `angles`) lies outside the constraint's limits; 0 within them.
static func beyond_limits(constraint: Dictionary, measured: Dictionary) -> float:
	var out := 0.0
	if measured.has("cone"):
		out = maxf(out, float(measured["cone"]) - float(constraint.get("cone_max", 0.0)))
		out = maxf(out, maxf(float(constraint.get("plane_min", 0.0)) - float(measured["plane"]), float(measured["plane"]) - float(constraint.get("plane_max", 0.0))))
		out = maxf(out, maxf(float(constraint.get("twist_min", 0.0)) - float(measured["twist"]), float(measured["twist"]) - float(constraint.get("twist_max", 0.0))))
	if measured.has("angle") and String(constraint.get("type", "")) == "limited_hinge":
		out = maxf(out, maxf(float(constraint.get("min_angle", 0.0)) - float(measured["angle"]), float(measured["angle"]) - float(constraint.get("max_angle", 0.0))))
	return maxf(out, 0.0)


## The angle that turns `from` into `to` about `axis` (both projected onto the plane across it).
static func _turn_about(axis: Vector3, from: Vector3, to: Vector3) -> float:
	var p := (from - axis * axis.dot(from)).normalized()
	var q := (to - axis * axis.dot(to)).normalized()
	return atan2(p.cross(q).dot(axis), p.dot(q))
