# SPDX-License-Identifier: GPL-3.0-or-later
#
# The death screen (GAMEPLAY-PLAN.md, 6k): when GameSession says the player died
# the picture fades to black over FADE_SECONDS and asks "Load the last save?".
# Enter, the gamepad's A (the `ui_accept` action) or a click chooses to load (`load_chosen`:
# the shell calls GameSession.reload_or_restart); Escape or B (`ui_cancel`) quits
# (`quit_chosen`). This is the view, a Control that fills its host (the flat viewer's
# CanvasLayer, a VR shell's panel); the shell lowers the camera and stops the controls.
class_name DeathView
extends Control

## The player chose to load the last save (or, with none, to start again).
signal load_chosen
## The player chose to quit.
signal quit_chosen

## Seconds from the death to full black.
const FADE_SECONDS := 2.0
## Seconds after the fade before the prompt shows (it appears as the black is whole).
const PROMPT_DELAY := 0.0
const TITLE := "You died."
const QUESTION := "Load the last save?"
const KEYS := "Enter / A: load      Esc / B: quit"

## 0 clear, 1 black.
var fade := 0.0
var _time := -1.0
## Whether the view listens at all (false in runs that drive themselves).
var enabled: Callable = func() -> bool: return true


func _init(session: GameSession = null) -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	visible = false
	if session != null:
		session.player_died.connect(func(_killer: int) -> void: begin())


## The death began: fade out from now.
func begin() -> void:
	_time = 0.0
	fade = 0.0
	visible = true
	queue_redraw()


## The player lives again (a load, a restart).
func end() -> void:
	_time = -1.0
	fade = 0.0
	visible = false


func is_showing() -> bool:
	return _time >= 0.0


## Whether the question is asked (the fade is whole).
func is_asking() -> bool:
	return _time >= FADE_SECONDS + PROMPT_DELAY


## Advance the fade by `delta` seconds (the view does it itself each frame while shown).
func step(delta: float) -> void:
	if _time < 0.0:
		return
	_time += delta
	fade = clampf(_time / FADE_SECONDS, 0.0, 1.0)
	queue_redraw()


func _process(delta: float) -> void:
	step(delta)


func _unhandled_input(event: InputEvent) -> void:
	if not is_asking() or not bool(enabled.call()):
		return
	if event.is_action_pressed("ui_accept"):
		get_viewport().set_input_as_handled()
		load_chosen.emit()
	elif event.is_action_pressed("ui_cancel"):
		get_viewport().set_input_as_handled()
		quit_chosen.emit()


func _draw() -> void:
	if _time < 0.0:
		return
	draw_rect(Rect2(Vector2.ZERO, size), Color(0, 0, 0, fade))
	if not is_asking():
		return
	var font := get_theme_default_font()
	var big := 44
	var small := 24
	var centre := size / 2.0
	for line: Array in [[TITLE, big, -40.0], [QUESTION, small + 6, 20.0], [KEYS, small - 4, 70.0]]:
		var text: String = line[0]
		var font_size: int = line[1]
		var width := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
		draw_string(font, centre + Vector2(-width / 2.0, line[2]), text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size,
			Color(0.9, 0.9, 0.9))
