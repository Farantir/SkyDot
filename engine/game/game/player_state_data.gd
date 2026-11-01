# SPDX-License-Identifier: GPL-3.0-or-later
#
# What PlayerState asks the data (ARCHITECTURE.md 8.2, ADR 0009): whether the player is dead
# (Health 0) and ending his invisibility, over SkydotPapyrus, and no rule of its own. A native
# object cannot be faked, so PlayerState's unit tests (tests/gd/test_player_state.gd) extend this
# class. A mod does not override this; it overrides PlayerState's rule methods.
class_name PlayerStateData
extends RefCounted

var _papyrus: SkydotPapyrus


func _init(papyrus: SkydotPapyrus = null) -> void:
	_papyrus = papyrus


## Whether the player is dead (Health 0).
func is_dead() -> bool:
	return _papyrus.is_player_dead()


## Ends the player's invisibility.
func break_invisibility() -> void:
	_papyrus.break_invisibility(SkydotPapyrus.PLAYER_REF)
