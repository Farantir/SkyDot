# SPDX-License-Identifier: GPL-3.0-or-later
#
# The dialogue menu: the speaker's name and subtitle at the bottom centre, the
# topics to pick from centred right (topics already said are greyed), and a
# Goodbye entry of its own at the end. Text only. Up/down (arrows, d-pad, left
# stick) move, Enter or A choose, Escape, Tab or B say goodbye; the activate
# key (F, or A while a line plays) skips the line being spoken. It listens to
# GameSession's dialogue signals and calls its dialogue_* methods, and knows
# nothing else of the game. This is the view, a Control that fills its host
# (the viewer's CanvasLayer, a VR shell's panel); it takes its input from the
# InputMap's ui_* actions and `activate`, so any shell can feed them. While it
# is open it emits `engaged(true)` so the shell keeps the player still.
class_name DialogueView
extends Control

## True while the menu is open: the shell keeps the player still.
signal engaged(open: bool)

const GOODBYE := "Goodbye"
const SAID := Color(0.55, 0.55, 0.55)
const NORMAL := Color(0.95, 0.95, 0.95)
const SELECTED := Color(1.0, 0.9, 0.5)
## Seconds the stick must rest before it repeats a move.
const STICK_THRESHOLD := 0.6

var _session: GameSession
var _speaker := ""
var _line := ""
var _topics: Array = []
var _selected := 0
var _open := false
var _stick_ready := true
## Whether the menu listens at all (false in runs that drive themselves).
var enabled: Callable = func() -> bool: return true


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	session.dialogue_line.connect(_on_line)
	session.dialogue_topics.connect(_on_topics)
	session.dialogue_closed.connect(_on_closed)


func is_open() -> bool:
	return _open


func _on_line(speaker: int, text: String, _seconds: float) -> void:
	_open = true
	engaged.emit(true)
	_speaker = _session.ref_name(speaker)
	_line = text
	_topics = []
	queue_redraw()


func _on_topics(list: Array) -> void:
	_open = true
	engaged.emit(true)
	_topics = list
	_selected = 0
	queue_redraw()


func _on_closed() -> void:
	_open = false
	engaged.emit(false)
	_line = ""
	_topics = []
	queue_redraw()


func _entries() -> Array:
	var out: Array = []
	for topic: Dictionary in _topics:
		out.append({"text": topic["text"], "said": topic["said"]})
	out.append({"text": GOODBYE, "said": false})
	return out


func _input(event: InputEvent) -> void:
	if not _session.is_dialogue_open() or not enabled.call():
		return
	var handled := true
	if event.is_action_pressed(&"ui_cancel") or (event is InputEventKey and event.pressed
			and not event.echo and event.keycode == KEY_TAB):
		_session.dialogue_close()
	elif event.is_action_pressed(&"ui_down"):
		_move(1)
	elif event.is_action_pressed(&"ui_up"):
		_move(-1)
	elif event is InputEventJoypadMotion and event.axis == JOY_AXIS_LEFT_Y:
		_stick(event.axis_value)
	elif event.is_action_pressed(&"ui_accept") or event.is_action_pressed(&"activate"):
		if _topics.is_empty() and not _line.is_empty() and _is_talking():
			_session.dialogue_skip()
		else:
			_choose()
	else:
		handled = event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	if handled:
		get_viewport().set_input_as_handled()


## A line is playing while there is no list to pick from.
func _is_talking() -> bool:
	return _topics.is_empty()


func _stick(value: float) -> void:
	if absf(value) < STICK_THRESHOLD:
		_stick_ready = true
	elif _stick_ready:
		_stick_ready = false
		_move(1 if value > 0.0 else -1)


func _move(step: int) -> void:
	if _topics.is_empty():
		return
	var count := _topics.size() + 1
	_selected = (_selected + step + count) % count
	queue_redraw()


func _choose() -> void:
	if _topics.is_empty():
		return
	if _selected >= _topics.size():
		_session.dialogue_close()
	else:
		_session.dialogue_choose(_selected)


func _draw() -> void:
	if not _open:
		return
	var area := size
	var font := ThemeDB.fallback_font
	var font_size := int(maxf(14.0, area.y / 36.0))
	if not _line.is_empty():
		var y := area.y * 0.88
		var lines := _wrap(font, _line, font_size, area.x * 0.7)
		var line_height := font_size * 1.3
		if not _speaker.is_empty():
			_draw_centred(font, _speaker, int(font_size * 0.85), area.x * 0.5,
				y - line_height * lines.size() - font_size * 0.2, Color(0.8, 0.75, 0.55))
		for i in lines.size():
			_draw_centred(font, lines[i], font_size, area.x * 0.5, y - line_height * (lines.size() - 1 - i), NORMAL)
	if _topics.is_empty():
		return
	var entries := _entries()
	var line_height := font_size * 1.5
	var x := area.x * 0.62
	var top := area.y * 0.45 - line_height * entries.size() * 0.5
	for i in entries.size():
		var entry: Dictionary = entries[i]
		var colour := SAID if entry["said"] else NORMAL
		var text: String = entry["text"]
		if i == _selected:
			colour = SELECTED
			text = "> " + text
		else:
			text = "  " + text
		_draw_text(font, text, font_size, Vector2(x, top + line_height * i), colour)


func _draw_text(font: Font, text: String, font_size: int, at: Vector2, colour: Color) -> void:
	draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 4, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)


func _draw_centred(font: Font, text: String, font_size: int, centre_x: float, y: float, colour: Color) -> void:
	var width := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
	_draw_text(font, text, font_size, Vector2(centre_x - width * 0.5, y), colour)


## `text` broken into lines of at most `width` pixels.
func _wrap(font: Font, text: String, font_size: int, width: float) -> Array[String]:
	var lines: Array[String] = []
	var current := ""
	for word in text.split(" "):
		var trial := word if current.is_empty() else current + " " + word
		if font.get_string_size(trial, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x > width and not current.is_empty():
			lines.append(current)
			current = word
		else:
			current = trial
	lines.append(current)
	return lines
