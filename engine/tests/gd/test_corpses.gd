# SPDX-License-Identifier: GPL-3.0-or-later
#
# The ragdoll builder on data written here (the shape of SkydotWorld.get_ragdoll) and the laws
# Corpses takes from the game: the push, the killing hit's force, the self-collision table.
extends "gd_test.gd"

const Fakes := preload("fakes.gd")

## Two capsules: the pelvis (part 2) and a thigh (part 8) under it, joined by a ragdoll constraint.
func _data() -> Dictionary:
	var capsule := func(length: float) -> Dictionary:
		return {"kind": "capsule", "transform": Transform3D.IDENTITY, "radius": 0.1,
			"point_a": Vector3.ZERO, "point_b": Vector3(0, 0, length)}
	var body := func(bone: String, part: int, mass: float, length: float) -> Dictionary:
		return {"bone": bone, "body_transform": Transform3D.IDENTITY, "shapes": [capsule.call(length)], "mass": mass,
			"center_of_mass": Vector3(0, 0, length * 0.5), "inertia": Basis.IDENTITY * 0.1, "linear_damping": 0.1,
			"angular_damping": 0.05, "friction": 0.3, "restitution": 0.8, "flags_and_part": part, "constraints": PackedInt32Array()}
	return {"bodies": [body.call("Pelvis", 2, 5.0, 0.1), body.call("Thigh", 8, 6.0, 0.4), body.call("Hand", 7, 4.0, 0.1)],
		"constraints": [{"type": "ragdoll", "body_a": 1, "body_b": 0,
			"in_a": {"pivot": Vector3.ZERO, "twist": Vector3.RIGHT, "plane": Vector3.UP},
			"in_b": {"pivot": Vector3(0, 0, -0.1), "twist": Vector3.RIGHT, "plane": Vector3.UP},
			"cone_max": 0.7, "plane_min": -0.8, "plane_max": 0.2, "twist_min": -0.1, "twist_max": 0.1, "max_friction": 10.0}]}


func _skeleton(units: float) -> Skeleton3D:
	var skeleton := Skeleton3D.new()
	skeleton.add_bone("Pelvis")
	skeleton.add_bone("Thigh")
	skeleton.set_bone_parent(1, 0)
	skeleton.scale = Vector3.ONE * units
	add_child(skeleton)
	return skeleton


func test_bodies_take_the_datas_shapes_and_masses_at_the_actors_scale() -> void:
	var skeleton := _skeleton(Ragdoll.METRES_PER_UNIT * 2.0)  # an actor of scale 2
	var ragdoll := Ragdoll.make(skeleton, _data())
	expect(ragdoll != null, "a ragdoll is made")
	expect_eq(ragdoll.bones.size(), 3, "one entry per data body")
	expect(ragdoll.bones[2] == null, "a body whose bone the skeleton lacks is left out")
	expect_eq(ragdoll.body_count(), 2, "two bodies made")
	var thigh := ragdoll.bones[1]
	expect(is_equal_approx(thigh.mass, 12.0), "mass linear in the actor's scale: %f" % thigh.mass)
	var capsule := (thigh.get_child(0) as CollisionShape3D).shape as CapsuleShape3D
	expect(is_equal_approx(capsule.radius, 0.2) and is_equal_approx(capsule.height, 1.2), "the capsule scaled: %f %f" % [capsule.radius, capsule.height])
	expect(is_equal_approx(thigh.bounce, 0.8) and is_equal_approx(thigh.friction, 0.3), "friction and restitution from the data")
	expect(thigh.linear_damp_mode == PhysicalBone3D.DAMP_MODE_REPLACE and is_equal_approx(thigh.angular_damp, 0.05), "damping from the data")
	expect(thigh.collision_layer == SkydotPlayer.LAYER_DEAD_BIPED, "on the dead-biped layer")
	expect(thigh.get_collision_exceptions().has(ragdoll.bones[0]), "pelvis and thigh do not collide (the game's table)")
	ragdoll.start()
	expect_eq(ragdoll.joints.size(), 1, "the constraint becomes a joint")
	ragdoll.stop()
	skeleton.queue_free()


func test_no_body_on_the_skeleton_no_ragdoll() -> void:
	var skeleton := Skeleton3D.new()
	skeleton.add_bone("Other")
	add_child(skeleton)
	expect(Ragdoll.make(skeleton, _data()) == null, "none")
	expect(Ragdoll.make(skeleton, {}) == null, "none without data")
	skeleton.queue_free()


func test_the_games_biped_table() -> void:
	expect(not Ragdoll.collides(2, 8), "the torso and a thigh do not collide")
	expect(Ragdoll.collides(7, 2) and Ragdoll.collides(2, 7), "a hand and the torso do")
	expect(not Ragdoll.collides(0, 0), "part 0 (a creature's bodies) collides with nothing of its own")


func test_a_push_is_the_games_velocity_law() -> void:
	var motion := Corpses.push_motion(Vector3.BACK, 15.0, true)
	expect(motion["linear"].is_equal_approx(Vector3(0, 0.25 * 15.0, 0.70 * 15.0)), "0.70 m/s along, 0.25 up per unit: %s" % motion["linear"])
	expect(is_equal_approx(motion["spin"].length(), 0.30 * 15.0) and absf(motion["spin"].dot(Vector3.BACK)) < 1e-6, "tipping over across the push")
	expect(Corpses.push_motion(Vector3.BACK, 15.0, false)["linear"].y == 0.0, "no lift for a killing hit")


func test_the_killing_hits_force_follows_the_gmsts() -> void:
	var host := Node3D.new()
	var registry_host := Node3D.new()
	var corpses := Corpses.new(Fakes.world(), Fakes.papyrus(), host, func() -> Vector3: return Vector3.ZERO, ActorRegistry.new(registry_host))
	expect(is_equal_approx(corpses.death_force(1.0, false), 4.0) and is_equal_approx(corpses.death_force(40.0, false), 12.0), "melee 1-40 -> 4-12")
	expect(is_equal_approx(corpses.death_force(100.0, true), 30.0), "ranged held at its maximum")
	expect(is_equal_approx(corpses.death_force(25.5, true), 20.0), "ranged halfway")
	host.free()
	registry_host.free()
