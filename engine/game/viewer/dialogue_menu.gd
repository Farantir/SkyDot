# SPDX-License-Identifier: GPL-3.0-or-later
#
# The flat viewer's host of the dialogue menu (ui/dialogue_view.gd): a
# CanvasLayer over the window. While the dialogue is open the player stands
# still and the other keys do nothing (ShellLoop.update_blocked).
class_name DialogueMenu
extends CanvasLayer

var _view: DialogueView


func _init(session: GameSession, input: PlayerInput) -> void:
	layer = 11
	_view = DialogueView.new(session)
	_view.enabled = func() -> bool: return input.enabled
	add_child(_view)


## () -> bool: whether a service menu is on top of the dialogue (DialogueView.service_open).
func set_service_open(service_open: Callable) -> void:
	_view.service_open = service_open


func is_open() -> bool:
	return _view.is_open()
