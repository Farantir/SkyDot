# SPDX-License-Identifier: GPL-3.0-or-later
#
# The death screen (GAMEPLAY-PLAN.md, 6k): when GameSession says the player died the picture fades to black
# over FADE_SECONDS and, RELOAD_SECONDS after the death, the last save loads by itself (`load_chosen`: the
# shell calls GameSession.reload_or_restart), as the game does: there is no question. Enter or A skips the
# wait; Escape or B quits (`quit_chosen`). A mouse click skips the wait too. PLACEHOLDER: the 5 s is
# fPlayerDeathReloadTime's default, not yet measured in the game. This is the view, a Control that fills its
# host (the flat viewer's CanvasLayer, a VR shell's panel); the shell lowers the camera and stops the controls.
class_name DeathView
extends Control

## The player chose to load the last save (or, with none, to start again).
signal load_chosen
## The player chose to quit.
signal quit_chosen

## Seconds from the death to full black.
const FADE_SECONDS := 2.0
## Seconds from the death to the automatic reload (fPlayerDeathReloadTime).
const RELOAD_SECONDS := 5.0

## 0 clear, 1 black.
var fade := 0.0
var _time := -1.0
var _reloaded := false
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
	_reloaded = false
	visible = true
	queue_redraw()


## The player lives again (a load, a restart).
func end() -> void:
	_time = -1.0
	fade = 0.0
	visible = false


func is_showing() -> bool:
	return _time >= 0.0


## Whether the screen is black and the reload may be skipped with a key.
func is_asking() -> bool:
	return _time >= FADE_SECONDS


## Advance the fade by `delta` seconds (the view does it itself each frame while shown).
func step(delta: float) -> void:
	if _time < 0.0:
		return
	_time += delta
	fade = clampf(_time / FADE_SECONDS, 0.0, 1.0)
	queue_redraw()
	if enabled.call() and _time >= RELOAD_SECONDS:
		_reload()


func _process(delta: float) -> void:
	step(delta)


func _unhandled_input(event: InputEvent) -> void:
	handle_input(event)


## The keys: Enter or A skips the wait, Escape or B quits. A host whose view gets no keys (a VR panel's
## SubViewport) hands them in.
func handle_input(event: InputEvent) -> void:
	if not is_asking() or not bool(enabled.call()):
		return
	if event.is_action_pressed("ui_accept") or (event is InputEventMouseButton and event.pressed):
		get_viewport().set_input_as_handled()
		_reload()
	elif event.is_action_pressed("ui_cancel"):
		get_viewport().set_input_as_handled()
		quit_chosen.emit()


func _reload() -> void:
	if _reloaded:
		return
	_reloaded = true
	load_chosen.emit()


func _draw() -> void:
	if _time >= 0.0:
		draw_rect(Rect2(Vector2.ZERO, size), Color(0, 0, 0, fade))
