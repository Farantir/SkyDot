# SPDX-License-Identifier: GPL-3.0-or-later
#
# PlayerState: which state of the player refuses which action, which actions break invisibility,
# over a fake PlayerStateData.
extends "gd_test.gd"

const ACTIONS: Array[String] = [PlayerState.MOVE, PlayerState.READY, PlayerState.ATTACK,
		PlayerState.CAST, PlayerState.SHOUT, PlayerState.ACTIVATE]


class FakeData extends PlayerStateData:
	var dead := false
	var broken := 0

	func is_dead() -> bool:
		return dead

	func break_invisibility() -> void:
		broken += 1


class Deaf extends PlayerState:
	func refusal(action: String) -> String:
		return "deaf" if action == PlayerState.SHOUT else super(action)


func _state(dead: bool, paralysed: bool) -> PlayerState:
	var data := FakeData.new()
	data.dead = dead
	var state := PlayerState.new(data)
	state.set_paralyzed(paralysed)
	return state


func test_refusal_table() -> void:
	for action in ACTIONS:
		var move: bool = action == PlayerState.MOVE
		expect_eq(_state(false, false).refusal(action), "", "%s alive" % action)
		expect_eq(_state(true, false).refusal(action), "dead", "%s dead" % action)
		expect_eq(_state(false, true).refusal(action), "paralyzed", "%s paralysed" % action)
		expect_eq(_state(true, true).refusal(action), "dead", "%s dead and paralysed" % action)


func test_can_is_an_empty_refusal() -> void:
	for action in ACTIONS:
		expect_eq(_state(false, false).can(action), true, "%s alive" % action)
		expect_eq(_state(false, true).can(action), false, "%s paralysed" % action)


func test_breaks_invisibility_per_action() -> void:
	var state := _state(false, false)
	for action in ACTIONS:
		var expected: bool = action in [PlayerState.ATTACK, PlayerState.CAST, PlayerState.SHOUT, PlayerState.ACTIVATE]
		expect_eq(state.breaks_invisibility(action), expected, action)


func test_acted_breaks_exactly_when_it_should() -> void:
	for action in ACTIONS:
		var data := FakeData.new()
		var state := PlayerState.new(data)
		state.acted(action)
		expect_eq(data.broken, 1 if state.breaks_invisibility(action) else 0, "%s breaks" % action)


func test_paralysis_flag() -> void:
	var state := _state(false, false)
	expect_eq(state.is_paralyzed(), false, "starts free")
	state.set_paralyzed(true)
	expect_eq(state.is_paralyzed(), true, "set")


func test_a_subclass_changes_can() -> void:
	var state := Deaf.new(FakeData.new())
	expect_eq(state.can(PlayerState.SHOUT), false, "shout")
	expect_eq(state.refusal(PlayerState.SHOUT), "deaf", "reason")
	expect_eq(state.can(PlayerState.CAST), true, "cast")
