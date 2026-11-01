# SPDX-License-Identifier: GPL-3.0-or-later
#
# PlayerBody (game/player_body.gd): the Game layer's contract for the shell's player object, and
# the stand-in the tests hand out. Defaults, round trips and a Place that uses one.
extends "gd_test.gd"

const Fakes := preload("fakes.gd")


class Recording:
	extends PlayerBody
	var flying_calls: Array[bool] = []

	func set_flying(on: bool) -> void:
		flying_calls.append(on)
		super.set_flying(on)


func test_flying_round_trip() -> void:
	var body := PlayerBody.new()
	expect(not body.flying, "walks at first")
	body.flying = true
	expect(body.flying, "flying set")
	body.flying = false
	expect(not body.flying, "flying cleared")


func test_a_subclass_sees_the_flying_property() -> void:
	var body := Recording.new()
	body.flying = true
	expect_eq(body.flying_calls, [true], "set_flying is the hook")
	expect(body.flying, "the property reads is_flying")


func test_place_at_sets_the_look() -> void:
	var body := PlayerBody.new()
	body.place_at(Vector3(1, 2, 3), 0.5, -0.25)
	expect_eq(body.position, Vector3(1, 2, 3), "position")
	expect_eq(body.yaw, 0.5, "yaw")
	expect_eq(body.pitch, -0.25, "pitch")


func test_speeds_round_trip() -> void:
	var body := PlayerBody.new()
	body.set_walk_speed(1.0)
	body.set_run_speed(2.0)
	body.set_sprint_speed(3.0)
	body.set_swim_speed(4.0)
	body.set_swim_walk_speed(5.0)
	body.set_jump_height(6.0)
	body.set_back_walk_ratio(0.5)
	body.set_back_run_ratio(0.25)
	expect_eq([body.get_walk_speed(), body.get_run_speed(), body.get_sprint_speed(), body.get_swim_speed(),
		body.get_swim_walk_speed(), body.get_jump_height(), body.get_back_walk_ratio(), body.get_back_run_ratio()],
		[1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 0.5, 0.25], "the speeds")


func test_sprint_and_shove_defaults() -> void:
	var body := PlayerBody.new()
	expect(not body.is_sprinting(), "not sprinting")
	body.set_sprint_allowed(false)
	expect(not body.impulse(Vector3.UP), "no body to push")


func test_save_section_round_trip() -> void:
	var body := PlayerBody.new()
	body.place_at(Vector3(4, 5, 6), 1.5, 0.5)
	var other := PlayerBody.new()
	expect(other.load_section(body.save_section()), "the section loads")
	expect_eq(other.loaded_position, Vector3(4, 5, 6), "loaded position")
	expect_eq(other.loaded_yaw, 1.5, "loaded yaw")
	expect_eq(other.loaded_pitch, 0.5, "loaded pitch")
	expect(not other.load_section(var_to_bytes("not a player")), "a foreign section is refused")


func test_a_place_uses_a_body() -> void:
	var body := PlayerBody.new()
	var place := Place.new(Fakes.world(), Node3D.new(), null, null, body, null, SkydotClock.new(), SkydotStreamer.new())
	body.flying = true
	place.leave()
	expect(body.flying, "the player's flight stays")
