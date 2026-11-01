# SPDX-License-Identifier: GPL-3.0-or-later
#
# The flat viewer's drawing of the door fade: a black rectangle over everything (a layer above
# the notes) that follows PlaceTransition.fade_changed. The VR shell has its own: a CanvasLayer
# is not drawn into a headset's views.
class_name FadeLayer
extends CanvasLayer

var _rect: ColorRect


func _init(transition: PlaceTransition) -> void:
	layer = 100  # over the notes
	_rect = ColorRect.new()
	_rect.color = Color(0, 0, 0, 0)
	_rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_rect.visible = false
	add_child(_rect)
	transition.fade_changed.connect(_on_fade_changed)
	_on_fade_changed(transition.fade)


func _on_fade_changed(alpha: float) -> void:
	_rect.color.a = alpha
	_rect.visible = alpha > 0.0
