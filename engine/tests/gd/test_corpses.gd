# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

const Fakes := preload("fakes.gd")

func test_bone_without_its_child_bone() -> void:
	var corpses := Corpses.new(Fakes.world(), Fakes.papyrus(), Node3D.new(), func() -> Vector3: return Vector3.ZERO, ActorRegistry.new(Node3D.new()))
	var skeleton := Skeleton3D.new()
	skeleton.add_bone("Arm")
	var bone := corpses._make_bone(skeleton, ["Arm", "Hand", 0.05, 0.0, 0.0], 1.0)
	expect(bone != null, "a bone is made without the child")
	bone.free()
	skeleton.free()
