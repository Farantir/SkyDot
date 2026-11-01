# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

const Fakes := preload("fakes.gd")

func _place(player: PlayerBody) -> Place:
	return Place.new(Fakes.world(), Node3D.new(), null, null, player, null, SkydotClock.new(), SkydotStreamer.new())

func test_flight_the_place_started_ends_with_the_place() -> void:
	var player := PlayerBody.new()
	var place := _place(player)
	player.flying = true
	place._flying_by_place = true
	place.leave()
	expect(not player.flying, "flying from a door-less cell stops when leaving")

func test_flight_the_player_chose_stays() -> void:
	var player := PlayerBody.new()
	var place := _place(player)
	player.flying = true
	place.leave()
	expect(player.flying, "a player who pressed V keeps flying")
