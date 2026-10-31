# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

func test_facing_straight_up_is_a_valid_basis() -> void:
	for dir in [Vector3.UP, Vector3.DOWN]:
		var b := Projectiles.facing(dir)
		expect(b.is_finite(), "finite for %s" % dir)
		expect(absf(b.determinant() - 1.0) < 0.001, "a rotation for %s: det %s" % [dir, b.determinant()])
		expect(((-b.z) - dir).length() < 0.001, "faces %s, got %s" % [dir, -b.z])

func test_a_removal_during_the_step_skips_that_shot() -> void:
	var p := Projectiles.new()
	add_child(p)
	var a := Node3D.new()
	var b := Node3D.new()
	add_child(a)
	add_child(b)
	var sa := {"node": a, "age": 0.0, "stuck": true, "travelled": 0.0, "range": 1.0}
	var sb := {"node": b, "age": 0.0, "stuck": true, "travelled": 0.0, "range": 1.0}
	p._live.append(sa)
	p._live.append(sb)
	p._remove(1)  # as a callback of the first shot would
	p._physics_process(0.1)
	expect_eq(p._live.size(), 1, "one shot left")
	expect_eq(sa["age"], 0.1, "the first shot advanced once")
	expect(not sb["age"] > 0.0, "the removed shot was skipped")
