# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

const Fakes := preload("fakes.gd")
const CUBE := 0x100  # TestpackCube, a STAT
const REF := 0x00F00001

func test_remove_in_the_frame_of_place() -> void:
	var world := testpack_world()
	if world == null:
		return
	var placed := PlacedRefs.new(world, Fakes.papyrus())
	add_child(placed)
	placed.place(REF, CUBE, 0, Vector3.ZERO, Vector3.ZERO, 1.0, false)
	placed.remove(REF)
	await get_tree().process_frame
	await get_tree().process_frame
	expect(world.find_ref_node(REF) == null, "a reference removed before it was built is not built")
	placed.place(REF, CUBE, 0, Vector3.ZERO, Vector3.ZERO, 1.0, false)
	await get_tree().process_frame
	expect(world.find_ref_node(REF) != null, "and placed again it is built")
