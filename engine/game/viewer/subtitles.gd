# SPDX-License-Identifier: GPL-3.0-or-later
#
# The flat viewer's host of the scene subtitles (ui/subtitles_view.gd): a
# CanvasLayer over the window.
class_name Subtitles
extends CanvasLayer


func _init(session: GameSession) -> void:
	layer = 9
	add_child(SubtitlesView.new(session))
