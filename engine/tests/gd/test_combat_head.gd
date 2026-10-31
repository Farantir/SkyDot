# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

func test_head_point_is_the_head_bone() -> void:
	var actor := Node3D.new()
	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton"  # as the cell builder names it
	skeleton.add_bone("Root")
	skeleton.add_bone("NPC Head [Head]")
	skeleton.set_bone_parent(1, 0)
	skeleton.set_bone_rest(1, Transform3D(Basis(), Vector3(0, 1.5, 0)))
	skeleton.reset_bone_poses()
	actor.add_child(skeleton)
	add_child(actor)
	actor.position = Vector3(10, 0, 0)
	var p := Combat.head_point_in(actor)
	expect(p.distance_to(Vector3(10, 1.5, 0)) < 0.001, "the head bone's place, got %s" % p)
