# SPDX-License-Identifier: GPL-3.0-or-later
#
# The rules of the player's state (ARCHITECTURE.md 8.2, ADR 0009): which state blocks which action
# and which actions break invisibility. `refusal(action)` answers "" when the player may, else
# "dead" or "paralyzed"; `acted(action)` ends his invisibility when the action breaks it. It
# emits nothing and touches no node. C++ keeps the state: death is the player's Health and
# invisibility an active effect, both asked through a PlayerStateData (the tests give it a fake).
# Menus and dialogue are gated by the shell (controls.blocked) and the door fade by the session,
# not here.
#
# Paralysis is the one flag held here, the mirror of the C++ "paralyzed" magic override;
# MagicWorld is its only writer. It is not in GameSession._lifecycle: MagicWorld re-syncs it
# every step and on load (MagicWorld.on_loaded), so there is nothing to reset.
#
# A script mod extends this class and overrides `refusal` or `breaks_invisibility`, or replaces
# it whole (GameSession.player_state_script).
class_name PlayerState
extends RefCounted

## Actions, the argument of `refusal`, `can`, `breaks_invisibility` and `acted`.
const MOVE := "move"
## Drawing or sheathing the weapon, readying the hands.
const READY := "ready"
const ATTACK := "attack"
const CAST := "cast"
const SHOUT := "shout"
const ACTIVATE := "activate"

var _data: PlayerStateData
var _paralyzed := false


func _init(data: PlayerStateData) -> void:
	_data = data


## Whether the player is dead (Health 0).
func is_dead() -> bool:
	return _data.is_dead()


## Whether the player is paralysed.
func is_paralyzed() -> bool:
	return _paralyzed


## Sets the paralysis flag (MagicWorld, from the C++ magic overrides).
func set_paralyzed(value: bool) -> void:
	_paralyzed = value


## Why the player cannot do `action`: "" if he can, else "dead" or "paralyzed". Every action,
## moving included, is refused by death first, then by paralysis (the death screen is the shell's).
func refusal(action: String) -> String:
	if is_dead():
		return "dead"
	if is_paralyzed():
		return "paralyzed"
	return ""


## Whether the player can do `action`.
func can(action: String) -> bool:
	return refusal(action) == ""


## Whether doing `action` ends the player's invisibility.
func breaks_invisibility(action: String) -> bool:
	return action == ATTACK or action == CAST or action == SHOUT or action == ACTIVATE


## The player did `action`: ends his invisibility if it breaks it.
func acted(action: String) -> void:
	if breaks_invisibility(action):
		_data.break_invisibility()
