# SPDX-License-Identifier: GPL-3.0-or-later
#
# A container's menu: what it holds on the left, what the player carries on
# the right. Up and down choose an item, left and right the column, accept
# moves the whole stack to the other side, `menu_drop` (R, X) takes everything
# the container holds, cancel, Tab or I closes. Items that are not playable
# are hidden in the player's column. A Control that fills its host (see
# InventoryView for the contract); opened by the shell when GameSession says
# `container_opened`.
class_name ContainerView
extends Control

signal engaged(open: bool)

const NORMAL := Color(0.92, 0.92, 0.92)
const DIM := Color(0.6, 0.6, 0.6)
const SELECTED := Color(1.0, 0.9, 0.5)
const STICK_THRESHOLD := 0.6

var _session: GameSession
var _open := false
var _container := 0
var _title := ""
var _column := 0  # 0 the container, 1 the player
var _selected := [0, 0]
var _rows: Array = [[], []]
var _stick_ready := true
var enabled: Callable = func() -> bool: return true
var key_label: Callable = func(_action: StringName) -> String: return ""


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	session.items_changed.connect(_refresh)


func is_open() -> bool:
	return _open


## The container reference being shown (0 when closed).
func container() -> int:
	return _container if _open else 0


func open(ref: int) -> void:
	_container = ref
	_title = _session.container_name(ref)
	_column = 0
	_selected = [0, 0]
	var was_open := _open
	_open = true
	if not was_open:
		engaged.emit(true)
	_refresh()


func close() -> void:
	if not _open:
		return
	_open = false
	_container = 0
	engaged.emit(false)
	queue_redraw()


func current() -> Dictionary:
	var rows: Array = _rows[_column]
	return rows[_selected[_column]] if _selected[_column] < rows.size() else {}


func _refresh() -> void:
	if not _open:
		return
	_rows[0] = ItemRows.visible(_session.inventory(_container))
	_rows[1] = ItemRows.visible(_session.inventory())
	for column in 2:
		_selected[column] = clampi(_selected[column], 0, maxi(_rows[column].size() - 1, 0))
	queue_redraw()


func _input(event: InputEvent) -> void:
	if not _open or not enabled.call():
		return
	var handled := true
	if event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"inventory"):
		close()
	elif event.is_action_pressed(&"ui_down"):
		_move(1)
	elif event.is_action_pressed(&"ui_up"):
		_move(-1)
	elif event.is_action_pressed(&"ui_right"):
		_switch(1)
	elif event.is_action_pressed(&"ui_left"):
		_switch(0)
	elif event is InputEventJoypadMotion and event.axis == JOY_AXIS_LEFT_Y:
		_stick(event.axis_value)
	elif event.is_action_pressed(&"ui_accept"):
		move_current()
	elif event.is_action_pressed(&"menu_drop"):
		take_all()
	else:
		handled = event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	if handled:
		get_viewport().set_input_as_handled()


func _stick(value: float) -> void:
	if absf(value) < STICK_THRESHOLD:
		_stick_ready = true
	elif _stick_ready:
		_stick_ready = false
		_move(1 if value > 0.0 else -1)


func _move(step: int) -> void:
	var rows: Array = _rows[_column]
	if rows.is_empty():
		return
	_selected[_column] = (_selected[_column] + step + rows.size()) % rows.size()
	queue_redraw()


func _switch(column: int) -> void:
	_column = column
	queue_redraw()


## The stack under the cursor goes to the other side.
func move_current() -> void:
	var row := current()
	if row.is_empty():
		return
	var other := SkydotPapyrus.PLAYER_REF if _column == 0 else _container
	var from := _container if _column == 0 else SkydotPapyrus.PLAYER_REF
	_session.transfer(from, other, int(row["form"]), int(row["count"]))
	_refresh()


## Everything the container holds goes to the player (non-playable items too), except
## non-playable armor: a dead actor's race skin, a dragon's underskin and skin hider are what it
## wears, not loot, and taking them would strip the corpse; nameless items (effect armors) are not loot either.
func take_all() -> void:
	for row: Dictionary in _session.papyrus.inventory(_container):
		if row["name"] == "" or (not row["playable"] and row["category"] in ["armor", "clothing"]):
			continue
		_session.transfer(_container, SkydotPapyrus.PLAYER_REF, int(row["form"]), int(row["count"]), int(row["extra"]))
	_refresh()


func _draw() -> void:
	if not _open:
		return
	var area := size
	var font := ThemeDB.fallback_font
	var font_size := int(maxf(14.0, area.y / 38.0))
	var line := font_size * 1.45
	var panel := Rect2(area * Vector2(0.1, 0.08), area * Vector2(0.8, 0.84))
	draw_rect(panel, Color(0.04, 0.04, 0.05, 0.88))
	draw_rect(panel, Color(0.5, 0.45, 0.35), false, 2.0)
	var half := panel.size.x / 2.0
	for column in 2:
		var x := panel.position.x + half * column + font_size
		var top := panel.position.y + font_size * 2.0
		var title := (_title if _title != "" else "Container") if column == 0 else "Inventory"
		_text(font, title, font_size, Vector2(x, top - font_size * 0.4), SELECTED if column == _column else NORMAL)
		var rows: Array = _rows[column]
		var rows_fit := int((panel.end.y - top - line * 3.0) / line)
		var first := clampi(_selected[column] - rows_fit / 2, 0, maxi(rows.size() - rows_fit, 0))
		for i in range(first, mini(first + rows_fit, rows.size())):
			var row: Dictionary = rows[i]
			var y := top + line * (i - first + 1)
			var chosen: bool = column == _column and i == _selected[column]
			var colour := SELECTED if chosen else NORMAL
			_text(font, ("> " if chosen else "  ") + ItemRows.label(row), font_size, Vector2(x, y), colour)
			var detail := "%s  %d" % [ItemRows.weight_text(row), int(row["value"])]
			var width := font.get_string_size(detail, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
			_text(font, detail, font_size, Vector2(x + half - font_size * 3.0 - width, y), DIM)
		if rows.is_empty():
			_text(font, "Empty", font_size, Vector2(x, top + line), DIM)
	draw_line(Vector2(panel.position.x + half, panel.position.y + 4.0), Vector2(panel.position.x + half, panel.end.y - 4.0), Color(0.5, 0.45, 0.35))
	var hint := "%s move stack   %s take all   %s close" % [_key(&"ui_accept", "Enter"), _key(&"menu_drop", "R"), _key(&"inventory", "I")]
	_text(font, hint, font_size, Vector2(panel.position.x + font_size, panel.end.y - font_size * 0.8), DIM)


func _key(action: StringName, fallback: String) -> String:
	var label: String = key_label.call(action)
	return label if label != "" else fallback


func _text(font: Font, text: String, font_size: int, at: Vector2, colour: Color) -> void:
	draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 3, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)
