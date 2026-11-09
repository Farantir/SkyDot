# SPDX-License-Identifier: GPL-3.0-or-later
#
# The joints Ragdoll makes, driven in Jolt and measured the way Havok measures its limits
# (Ragdoll.angles): which way round Jolt's 6DOF limits go, how close the per-axis limits come to
# Havok's round cone with asymmetric plane limits (Ragdoll.CONE_GAP_DEGREES), and whether the
# angular motor holds as Havok's constraint friction does. A parent body is fixed, a child capsule
# on it is turned by a torque toward one direction; many pairs run side by side.
extends "gd_test.gd"

const STEPS := 120
const TORQUE := 40.0

## A ragdoll constraint: twist along X, plane axis Y, cone 45°, plane -50..15°, twist ±10° (the
## human hip's), frames at the origin of both bodies.
const HIP := {"type": "ragdoll", "body_a": 1, "body_b": 0,
	"in_a": {"pivot": Vector3.ZERO, "twist": Vector3.RIGHT, "plane": Vector3.UP, "motor": Vector3.BACK},
	"in_b": {"pivot": Vector3.ZERO, "twist": Vector3.RIGHT, "plane": Vector3.UP, "motor": Vector3.BACK},
	"cone_max": 0.785398, "plane_min": -0.879646, "plane_max": 0.261799, "twist_min": -0.174533, "twist_max": 0.174533}
## A limited hinge about X, -110..0° (the human knee's).
const KNEE := {"type": "limited_hinge", "body_a": 1, "body_b": 0,
	"in_a": {"pivot": Vector3.ZERO, "axle": Vector3.RIGHT, "perp_1": Vector3.UP, "perp_2": Vector3.BACK},
	"in_b": {"pivot": Vector3.ZERO, "axle": Vector3.RIGHT, "perp_1": Vector3.UP, "perp_2": Vector3.BACK},
	"min_angle": -1.919862, "max_angle": 0.0}

var _space: RID


func before_each() -> void:
	_space = get_viewport().world_3d.space
	PhysicsServer3D.area_set_param(_space, PhysicsServer3D.AREA_PARAM_GRAVITY, 0.0)


func after_each() -> void:
	PhysicsServer3D.area_set_param(_space, PhysicsServer3D.AREA_PARAM_GRAVITY, 9.8)


## A fixed parent and a free child capsule along X at `at`, joined by `constraint`.
func _pair(constraint: Dictionary, at: Vector3, friction := 0.0) -> Dictionary:
	var parent := StaticBody3D.new()
	var child := RigidBody3D.new()
	child.gravity_scale = 0.0
	child.mass = 5.0
	child.linear_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
	child.angular_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
	child.angular_damp = 2.0
	var shape := CollisionShape3D.new()
	var capsule := CapsuleShape3D.new()
	capsule.radius = 0.05
	capsule.height = 0.4
	shape.shape = capsule
	shape.transform = Transform3D(Basis(Vector3.BACK, -PI / 2.0), Vector3(0.2, 0, 0))
	child.add_child(shape)
	parent.collision_layer = 0
	child.collision_layer = 0
	child.collision_mask = 0
	parent.position = at
	child.position = at
	add_child(parent)
	add_child(child)
	var c := constraint.duplicate(true)
	c["max_friction"] = friction
	var joint := Ragdoll.joint_between(c, child.get_rid(), Transform3D.IDENTITY, parent.get_rid(), Transform3D.IDENTITY)
	return {"parent": parent, "child": child, "joint": joint, "constraint": c}


func _free(pairs: Array) -> void:
	for p: Dictionary in pairs:
		PhysicsServer3D.free_rid(p["joint"])
		p["child"].queue_free()
		p["parent"].queue_free()


## Directions over the sphere (Fibonacci).
func _directions(count: int) -> Array[Vector3]:
	var out: Array[Vector3] = []
	for i in count:
		var y := 1.0 - 2.0 * (i + 0.5) / count
		var r := sqrt(1.0 - y * y)
		var phi := i * PI * (3.0 - sqrt(5.0))
		out.append(Vector3(cos(phi) * r, y, sin(phi) * r))
	return out


## Whether Havok's cone and plane allow the twist axis to point along `d` (the parent's frame).
func _allowed(d: Vector3) -> bool:
	return acos(clampf(d.x, -1.0, 1.0)) <= HIP["cone_max"] + 1e-4 and asin(clampf(d.y, -1.0, 1.0)) >= HIP["plane_min"] - 1e-4 \
		and asin(clampf(d.y, -1.0, 1.0)) <= HIP["plane_max"] + 1e-4


func test_ragdoll_constraint_reaches_what_havok_allows() -> void:
	# Directions in front of the parent (a body behind its joint is not driven there by a torque):
	# each child is turned toward one. Inside Havok's set the child should get there; outside it, it
	# should stop at the set's edge.
	var targets: Array[Vector3] = []
	for d in _directions(400):
		if d.x > 0.05:
			targets.append(d)
	var pairs := []
	for i in targets.size():
		pairs.append(_pair(HIP, Vector3(i * 3.0, 50.0, 0.0)))
	for step in STEPS:
		for i in pairs.size():
			var child: RigidBody3D = pairs[i]["child"]
			var twist := child.global_transform.basis.x
			child.apply_torque(twist.cross(targets[i]).normalized() * TORQUE)
		await get_tree().physics_frame
	var short := 0.0  # inside Havok's set, how far from the target the child stopped
	var beyond := 0.0  # how far past Havok's cone and plane it went
	for i in pairs.size():
		var child: RigidBody3D = pairs[i]["child"]
		var reached := child.global_transform.basis.x.normalized()
		var measured := Ragdoll.angles(pairs[i]["constraint"], child.global_transform.basis, Basis.IDENTITY)
		measured["twist"] = 0.0  # the twist is measured on its own (below)
		var past := rad_to_deg(Ragdoll.beyond_limits(pairs[i]["constraint"], measured))
		beyond = maxf(beyond, past)
		var gap := 0.0
		if _allowed(targets[i]):
			gap = rad_to_deg(acos(clampf(reached.dot(targets[i]), -1.0, 1.0)))
			short = maxf(short, gap)
		if OS.has_environment("RAGDOLL_JOINT_DEBUG"):
			print("  target %s reached %s short %.1f past %.1f cone %.1f plane %.1f" % [targets[i], reached, gap, past,
				rad_to_deg(measured["cone"]), rad_to_deg(measured["plane"])])
	print("ragdoll joint over %d directions: short of Havok's set by up to %.1f deg, past its cone and plane by up to %.1f deg"
		% [targets.size(), short, beyond])
	expect(short <= Ragdoll.CONE_GAP_DEGREES, "the 6DOF reaches Havok's set within %.0f deg: %.1f" % [Ragdoll.CONE_GAP_DEGREES, short])
	expect(beyond <= Ragdoll.CONE_GAP_DEGREES, "and does not pass Havok's limits by more: %.1f" % beyond)
	_free(pairs)


func test_twist_and_hinge_limits_go_the_way_havok_measures() -> void:
	var pairs := []
	for sign in [-1.0, 1.0]:
		pairs.append(_pair(HIP, Vector3(sign * 10.0, 80.0, 0.0)))
		pairs.append(_pair(KNEE, Vector3(sign * 10.0, 90.0, 0.0)))
	for step in STEPS:
		for i in pairs.size():
			var child: RigidBody3D = pairs[i]["child"]
			child.apply_torque(child.global_transform.basis.x * TORQUE * (-1.0 if i < 2 else 1.0))
		await get_tree().physics_frame
	var twist_low: float = Ragdoll.angles(pairs[0]["constraint"], pairs[0]["child"].global_transform.basis, Basis.IDENTITY)["twist"]
	var knee_low: float = Ragdoll.angles(pairs[1]["constraint"], pairs[1]["child"].global_transform.basis, Basis.IDENTITY)["angle"]
	var twist_high: float = Ragdoll.angles(pairs[2]["constraint"], pairs[2]["child"].global_transform.basis, Basis.IDENTITY)["twist"]
	var knee_high: float = Ragdoll.angles(pairs[3]["constraint"], pairs[3]["child"].global_transform.basis, Basis.IDENTITY)["angle"]
	print("twist reached %.1f..%.1f deg (limits -10..10), knee %.1f..%.1f deg (limits -110..0)"
		% [rad_to_deg(twist_low), rad_to_deg(twist_high), rad_to_deg(knee_low), rad_to_deg(knee_high)])
	expect(absf(rad_to_deg(twist_low) + 10.0) < 3.0 and absf(rad_to_deg(twist_high) - 10.0) < 3.0, "the twist stops at its limits")
	expect(absf(rad_to_deg(knee_low) + 110.0) < 3.0 and absf(rad_to_deg(knee_high)) < 3.0, "the knee bends one way only, to -110")
	_free(pairs)


func test_constraint_friction_holds_below_its_torque() -> void:
	var weak := _pair(KNEE, Vector3(0, 120, 0), 10.0)
	var strong := _pair(KNEE, Vector3(5, 120, 0), 10.0)
	for step in STEPS:
		weak["child"].apply_torque(Vector3.RIGHT * -5.0)
		strong["child"].apply_torque(Vector3.RIGHT * -40.0)
		await get_tree().physics_frame
	var held: float = rad_to_deg(Ragdoll.angles(KNEE, weak["child"].global_transform.basis, Basis.IDENTITY)["angle"])
	var moved: float = rad_to_deg(Ragdoll.angles(KNEE, strong["child"].global_transform.basis, Basis.IDENTITY)["angle"])
	print("friction 10 N m: 5 N m turns the knee %.1f deg, 40 N m %.1f deg" % [held, moved])
	expect(absf(held) < 2.0, "a torque under the friction does not turn it: %.1f deg" % held)
	expect(moved < -20.0, "a torque over it does: %.1f deg" % moved)
	_free([weak, strong])
