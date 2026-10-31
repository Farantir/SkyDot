# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

func test_facing_straight_up_is_a_valid_basis() -> void:
	for dir in [Vector3.UP, Vector3.DOWN]:
		var b := Projectiles.facing(dir)
		expect(b.is_finite(), "finite for %s" % dir)
		expect(absf(b.determinant() - 1.0) < 0.001, "a rotation for %s: det %s" % [dir, b.determinant()])
		expect(((-b.z) - dir).length() < 0.001, "faces %s, got %s" % [dir, -b.z])
