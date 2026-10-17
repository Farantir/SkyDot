# SPDX-License-Identifier: GPL-3.0-or-later
#
# The flat viewer's host of the heads-up display (ui/hud_view.gd): a
# CanvasLayer over the window.
class_name Hud
extends CanvasLayer


func _init(session: GameSession, input: PlayerInput) -> void:
	layer = 10
	add_child(HudView.new(session, input.key_label))
