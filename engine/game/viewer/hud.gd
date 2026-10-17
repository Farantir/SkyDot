# SPDX-License-Identifier: GPL-3.0-or-later
#
# The heads-up display: Magicka (bottom left), Health (bottom centre) and
# Stamina (bottom right) bars, a crosshair and the activation prompt under it.
# A bar shows while its value is below the maximum or changed in the last two
# seconds, then fades out over half a second. It reads only GameSession's
# getters (actor_value, actor_value_max, activation_target), every frame, and
# draws; it changes nothing.
class_name Hud
extends CanvasLayer

## Seconds a bar stays after its value changed.
const LINGER := 2.0
## Seconds it then takes to fade.
const FADE := 0.5
## Bar width as a fraction of the viewport's width, and its height in pixels
## per 1080 pixels of viewport height.
const BAR_WIDTH := 0.2
const BAR_HEIGHT := 9.0
const MARGIN := 0.05  # of the viewport, from its edges
const FRAME := Color(0.0, 0.0, 0.0, 0.7)
const BARS := [
	{"av": "Magicka", "colour": Color(0.2, 0.45, 0.9), "anchor": 0.0},
	{"av": "Health", "colour": Color(0.78, 0.12, 0.1), "anchor": 0.5},
	{"av": "Stamina", "colour": Color(0.25, 0.7, 0.2), "anchor": 1.0},
]

var _session: GameSession
var _input: PlayerInput
var _canvas: Control
var _last_value := {}  # actor value name -> value seen last frame
var _last_change := {}  # actor value name -> msec of its last change
var _prompt := {}


func _init(session: GameSession, input: PlayerInput) -> void:
	_session = session
	_input = input
	layer = 10
	_canvas = Control.new()
	_canvas.set_anchors_preset(Control.PRESET_FULL_RECT)
	_canvas.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_canvas.draw.connect(_draw_hud)
	add_child(_canvas)


func _process(_delta: float) -> void:
	_prompt = _session.activation_target()
	_canvas.queue_redraw()


func _draw_hud() -> void:
	var size := _canvas.size
	var now := Time.get_ticks_msec()
	for bar: Dictionary in BARS:
		_draw_bar(bar, size, now)
	_draw_crosshair(size)
	_draw_prompt(size)


func _alpha(name: String, value: float, maximum: float, now: int) -> float:
	if not _last_value.has(name) or not is_equal_approx(_last_value[name], value):
		_last_change[name] = now
	_last_value[name] = value
	if value < maximum:
		return 1.0
	var since: float = (now - int(_last_change.get(name, -1000000))) / 1000.0
	return clampf(1.0 - (since - LINGER) / FADE, 0.0, 1.0)


func _draw_bar(bar: Dictionary, size: Vector2, now: int) -> void:
	var name: String = bar["av"]
	var maximum := _session.actor_value_max(SkydotPapyrus.PLAYER_REF, name)
	var value := _session.actor_value(SkydotPapyrus.PLAYER_REF, name)
	var alpha := _alpha(name, value, maximum, now)
	if alpha <= 0.0 or maximum <= 0.0:
		return
	var width := size.x * BAR_WIDTH
	var height := maxf(3.0, BAR_HEIGHT * size.y / 1080.0)
	var margin := size.x * MARGIN
	var x := lerpf(margin, size.x - margin - width, bar["anchor"])
	var y := size.y - size.y * MARGIN - height
	var frame := Rect2(x, y, width, height)
	_canvas.draw_rect(frame.grow(2.0), Color(FRAME, FRAME.a * alpha))
	_canvas.draw_rect(frame, Color(0.0, 0.0, 0.0, 0.5 * alpha))
	var filled := frame
	filled.size.x = width * clampf(value / maximum, 0.0, 1.0)
	var colour: Color = bar["colour"]
	_canvas.draw_rect(filled, Color(colour, alpha))


func _draw_crosshair(size: Vector2) -> void:
	var centre := size * 0.5
	var radius := maxf(2.0, size.y / 540.0)
	_canvas.draw_circle(centre, radius + 1.0, Color(0.0, 0.0, 0.0, 0.6))
	_canvas.draw_circle(centre, radius, Color(1.0, 1.0, 1.0, 0.85))


func _draw_prompt(size: Vector2) -> void:
	if _prompt.is_empty():
		return
	var font := ThemeDB.fallback_font
	var font_size := int(maxf(14.0, size.y / 40.0))
	var key := _input.key_label(&"activate")
	var text := "%s  %s  %s" % [key, _prompt["verb"], _prompt["name"]] if not key.is_empty() \
		else "%s  %s" % [_prompt["verb"], _prompt["name"]]
	var y := size.y * 0.5 + size.y * 0.06
	_draw_centred(font, text, font_size, size.x * 0.5, y, Color.WHITE)
	var lock := ActivationPrompt.lock_text(_prompt["lock_level"])
	if not lock.is_empty():
		_draw_centred(font, lock, int(font_size * 0.8), size.x * 0.5, y + font_size * 1.3,
			Color(0.85, 0.8, 0.6))


func _draw_centred(font: Font, text: String, font_size: int, centre_x: float, y: float, colour: Color) -> void:
	var width := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
	var at := Vector2(centre_x - width * 0.5, y)
	_canvas.draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 4, Color(0.0, 0.0, 0.0, 0.8))
	_canvas.draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)
