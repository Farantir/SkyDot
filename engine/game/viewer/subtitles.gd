# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scene subtitles (Skyrim's general subtitles): the lines actors speak in
# scenes, "Name: text" under the crosshair, for speakers within 20 m of the
# player, as long as the line lasts. It listens to GameSession's
# `subtitle_line` and asks `distance_to_player`; it changes nothing.
class_name Subtitles
extends CanvasLayer

## Speakers further than this (metres) are not subtitled.
const RANGE := 20.0

var _session: GameSession
var _canvas: Control
var _shown: Array = []  # {text, left}


func _init(session: GameSession) -> void:
	_session = session
	layer = 9
	_canvas = Control.new()
	_canvas.set_anchors_preset(Control.PRESET_FULL_RECT)
	_canvas.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_canvas.draw.connect(_draw_lines)
	add_child(_canvas)
	session.subtitle_line.connect(_on_line)


func _on_line(speaker: int, text: String, seconds: float) -> void:
	var distance := _session.distance_to_player(speaker)
	if distance > RANGE:
		return
	var name := _session.ref_name(speaker)
	_shown.append({"text": "%s: %s" % [name, text] if not name.is_empty() else text, "left": seconds})
	print("subtitle: %s (%.1f m, %.2f s)" % [_shown.back()["text"], distance, seconds])
	_canvas.queue_redraw()


func _process(delta: float) -> void:
	if _shown.is_empty():
		return
	for line: Dictionary in _shown:
		line["left"] -= delta
	_shown = _shown.filter(func(line: Dictionary) -> bool: return line["left"] > 0.0)
	_canvas.queue_redraw()


func _draw_lines() -> void:
	var size := _canvas.size
	var font := ThemeDB.fallback_font
	var font_size := int(maxf(14.0, size.y / 40.0))
	var y := size.y * 0.58
	for line: Dictionary in _shown:
		var text: String = line["text"]
		var width := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
		var at := Vector2(size.x * 0.5 - width * 0.5, y)
		_canvas.draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 4, Color(0.0, 0.0, 0.0, 0.8))
		_canvas.draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color.WHITE)
		y += font_size * 1.4
