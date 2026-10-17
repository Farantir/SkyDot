# SPDX-License-Identifier: GPL-3.0-or-later
#
# The flat viewer's host of the dialogue menu (ui/dialogue_view.gd): a
# CanvasLayer over the window. While the menu is open the player stands still
# and the other keys do nothing.
class_name DialogueMenu
extends CanvasLayer

var _view: DialogueView


func _init(session: GameSession, input: PlayerInput) -> void:
	layer = 11
	_view = DialogueView.new(session)
	_view.enabled = func() -> bool: return input.enabled
	_view.engaged.connect(func(open: bool) -> void: input.blocked = open)
	add_child(_view)


func is_open() -> bool:
	return _view.is_open()
