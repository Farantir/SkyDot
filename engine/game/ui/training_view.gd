# SPDX-License-Identifier: GPL-3.0-or-later
#
# A trainer's menu (GameSession's `training_opened`, after the "Train" topic): the skill the trainer teaches, its
# level and the trainer's limit, the cost of a session, the player's gold and the sessions left at this level.
# Accept buys a session (Progression.train: the skill goes up by one), cancel, Tab or I closes; a refusal shows its
# vanilla text. The dialogue stays open underneath and its keys are quiet while this is open (`service_open`).
# A Control that fills its host (see InventoryView for the contract); the numbers are TrainingModel's, the rules
# Progression's.
class_name TrainingView
extends Control

signal engaged(open: bool)

const NORMAL := Color(0.92, 0.92, 0.92)
const SELECTED := Color(1.0, 0.9, 0.5)
const SHORT := Color(0.9, 0.5, 0.45)

var _session: GameSession
var _open := false
var _trainer := 0
var _model: TrainingModel
var enabled: Callable = func() -> bool: return true
var key_label: Callable = func(_action: StringName) -> String: return ""


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	session.items_changed.connect(_refresh)


func is_open() -> bool:
	return _open


## The trainer being talked to (0 when closed).
func trainer() -> int:
	return _trainer if _open else 0


func model() -> TrainingModel:
	return _model


func open(trainer_ref: int) -> void:
	_trainer = trainer_ref
	var was_open := _open
	_open = true
	_session.service_open = true
	_refresh()
	print("training: %s offers %s (%d gold)" % [_model.trainer_name, _model.skill_name, _model.cost])
	if not was_open:
		engaged.emit(true)


func close() -> void:
	if not _open:
		return
	_open = false
	_trainer = 0
	_session.service_open = false
	engaged.emit(false)
	queue_redraw()


## Buy one session.
func train() -> Dictionary:
	if not _open:
		return {"ok": false}
	var result := _session.train(_trainer)
	_refresh()
	return result


func _refresh() -> void:
	if _open:
		_model = TrainingModel.of(_session, _trainer)
		queue_redraw()


func _input(event: InputEvent) -> void:
	if not _open or not enabled.call():
		return
	var handled := true
	if event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"inventory"):
		close()
	elif event.is_action_pressed(&"ui_accept"):
		train()
	else:
		handled = event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	if handled:
		get_viewport().set_input_as_handled()


func _draw() -> void:
	if not _open or _model == null:
		return
	var area := size
	var font := ThemeDB.fallback_font
	var font_size := int(maxf(14.0, area.y / 38.0))
	var line := font_size * 1.45
	var panel := Rect2(area * Vector2(0.25, 0.25), area * Vector2(0.5, 0.4))
	draw_rect(panel, Color(0.04, 0.04, 0.05, 0.9))
	draw_rect(panel, Color(0.5, 0.45, 0.35), false, 2.0)
	var y := panel.position.y + font_size * 2.0
	var lines := _model.lines()
	for i in lines.size():
		var colour := SELECTED if i == 0 else (SHORT if i == 4 else NORMAL)
		_text(font, lines[i], font_size, Vector2(panel.position.x + font_size, y), colour, panel.size.x - font_size * 2.0)
		y += line
	var hint := "%s train   %s close" % [_key(&"ui_accept", "Enter"), _key(&"inventory", "I")]
	_text(font, hint, font_size, Vector2(panel.position.x + font_size, panel.end.y - font_size * 0.6), NORMAL, panel.size.x - font_size * 2.0)


func _key(action: StringName, fallback: String) -> String:
	var label: String = key_label.call(action)
	return label if label != "" else fallback


func _text(font: Font, text: String, font_size: int, at: Vector2, colour: Color, width: float) -> void:
	while text.length() > 3 and font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x > width:
		text = text.substr(0, text.length() - 3) + ".."
	draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 3, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)
