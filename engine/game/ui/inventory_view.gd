# SPDX-License-Identifier: GPL-3.0-or-later
#
# The inventory menu: categories on the left, the items of the chosen one on
# the right with count, weight, value and a mark on what is equipped. Up and
# down choose an item, left and right a category, accept equips or unequips
# it (a potion or food is used up, a book read; neither does anything yet),
# `menu_drop` (R, X) drops the stack, `menu_equip_left` (Q, gamepad Y) recharges an
# enchanted weapon below its maximum from a filled soul gem (the smallest one
# that fills it, else the largest), cancel, Tab or I closes. Items that are
# not playable are hidden. This is the view, a Control that fills its host
# (the viewer's CanvasLayer, a VR shell's panel); it takes its input from the
# InputMap's ui_* actions and `inventory` and `menu_drop`, so any shell can
# feed them, and it knows the game only through GameSession's item methods.
# While it is open it emits `engaged(true)` so the shell keeps the player still.
class_name InventoryView
extends Control

signal engaged(open: bool)

const NORMAL := Color(0.92, 0.92, 0.92)
const DIM := Color(0.6, 0.6, 0.6)
const SELECTED := Color(1.0, 0.9, 0.5)
const EQUIPPED := Color(0.6, 0.9, 0.6)
const STICK_THRESHOLD := 0.6

var _session: GameSession
var _open := false
var _category := 0
var _selected := 0
var _rows: Array = []
var _all: Array = []
var _stick_ready := true
## Whether the menu listens at all (false in runs that drive themselves).
var enabled: Callable = func() -> bool: return true
## What the key labels say, by action (the viewer's gamepad or keyboard).
var key_label: Callable = func(_action: StringName) -> String: return ""


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	session.items_changed.connect(_refresh)


func is_open() -> bool:
	return _open


func open() -> void:
	if _open:
		return
	_open = true
	engaged.emit(true)
	_refresh()


func close() -> void:
	if not _open:
		return
	_open = false
	engaged.emit(false)
	queue_redraw()


## The item under the cursor, {} if the list is empty.
func current() -> Dictionary:
	return _rows[_selected] if _selected < _rows.size() else {}


func _refresh() -> void:
	if not _open:
		return
	_all = _session.inventory()
	_rows = ItemRows.visible(_all, _category)
	_selected = clampi(_selected, 0, maxi(_rows.size() - 1, 0))
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
		_change_category(1)
	elif event.is_action_pressed(&"ui_left"):
		_change_category(-1)
	elif event is InputEventJoypadMotion and event.axis == JOY_AXIS_LEFT_Y:
		_stick(event.axis_value)
	elif event.is_action_pressed(&"ui_accept"):
		use_current()
	elif event.is_action_pressed(&"menu_drop"):
		drop_current()
	elif event.is_action_pressed(&"menu_equip_left"):
		recharge_current()
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
	if _rows.is_empty():
		return
	_selected = (_selected + step + _rows.size()) % _rows.size()
	queue_redraw()


func _change_category(step: int) -> void:
	_category = (_category + step + ItemRows.CATEGORIES.size()) % ItemRows.CATEGORIES.size()
	_selected = 0
	_refresh()


## Equip the item under the cursor, or take it off if it is worn; a potion, food or ingredient is
## used up and takes effect (GameSession.use_item).
func use_current() -> void:
	var row := current()
	if row.is_empty():
		return
	if row["equipped"]:
		_session.unequip(int(row["form"]), int(row["extra"]))
	elif row["category"] in GameSession.USABLE:
		_session.use_item(int(row["form"]), int(row["extra"]))  # drunk, eaten: takes effect
	else:
		_session.equip(int(row["form"]), int(row["extra"]))
	_refresh()


## Whether the row is an enchanted weapon that can take more charge.
static func rechargeable(row: Dictionary) -> bool:
	return row.has("charge_max") and float(row["charge"]) < float(row["charge_max"])


## Recharge the weapon under the cursor from the filled soul gem that fills it with the least waste
## (the smallest that reaches the maximum), else from the largest soul there is.
func recharge_current() -> void:
	var row := current()
	if row.is_empty() or not rechargeable(row):
		return
	var options := _session.recharge_options(int(row["form"]), int(row["extra"]))
	if options.is_empty():
		_session.message.emit("No filled soul gem", false)
		return
	var best: Dictionary = {}
	for option: Dictionary in options:
		if best.is_empty():
			best = option
		elif option["fills"] != best["fills"]:
			best = option if option["fills"] else best
		elif option["fills"] == (float(option["charge"]) < float(best["charge"])):
			best = option  # both fill: the smaller; neither fills: the larger
	_session.recharge(int(row["form"]), int(row["extra"]), int(best["form"]), int(best["extra"]))


## Drop the stack under the cursor.
func drop_current() -> void:
	var row := current()
	if row.is_empty():
		return
	_session.drop(int(row["form"]), int(row["count"]), int(row["extra"]))
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
	var left := panel.position.x + font_size
	var top := panel.position.y + font_size * 2.0
	_text(font, "Inventory", font_size, Vector2(left, top - font_size * 0.4), SELECTED)
	var gold := 0
	for row: Dictionary in _all:
		if int(row["form"]) == 0xF:
			gold = int(row["count"])
	var header := "Gold %d    Weight %.1f" % [gold, ItemRows.total_weight(_all)]
	_text(font, header, font_size, Vector2(panel.end.x - font.get_string_size(header, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x - font_size, top - font_size * 0.4), NORMAL)
	var list_top := top + line
	for i in ItemRows.CATEGORIES.size():
		var current_category := i == _category
		_text(font, ("> " if current_category else "  ") + String(ItemRows.CATEGORIES[i]["name"]), font_size,
			Vector2(left, list_top + line * i), SELECTED if current_category else NORMAL)
	var columns_x := panel.position.x + panel.size.x * 0.24
	var weight_x := panel.end.x - panel.size.x * 0.24
	var value_x := panel.end.x - panel.size.x * 0.10
	_text(font, "Weight", font_size, Vector2(weight_x, list_top - line * 0.1), DIM)
	_text(font, "Value", font_size, Vector2(value_x, list_top - line * 0.1), DIM)
	var rows_fit := int((panel.end.y - list_top - line * 3.0) / line)
	var first := clampi(_selected - rows_fit / 2, 0, maxi(_rows.size() - rows_fit, 0))
	for i in range(first, mini(first + rows_fit, _rows.size())):
		var row: Dictionary = _rows[i]
		var y := list_top + line * (i - first + 1)
		var colour := EQUIPPED if row["equipped"] else NORMAL
		if i == _selected:
			colour = SELECTED
		_text(font, ("E " if row["equipped"] else "  ") + ItemRows.label(row), font_size, Vector2(columns_x, y), colour)
		_text(font, ItemRows.weight_text(row), font_size, Vector2(weight_x, y), colour)
		_text(font, str(int(row["value"])), font_size, Vector2(value_x, y), colour)
	if _rows.is_empty():
		_text(font, "Nothing here", font_size, Vector2(columns_x, list_top + line), DIM)
	var hint := "%s equip/unequip   %s drop   %s close" % [_key(&"ui_accept", "Enter"), _key(&"menu_drop", "R"), _key(&"inventory", "I")]
	if rechargeable(current()):
		hint = "%s equip/unequip   %s drop   %s recharge   %s close" % [_key(&"ui_accept", "Enter"), _key(&"menu_drop", "R"),
			_key(&"menu_equip_left", "Q"), _key(&"inventory", "I")]
	_text(font, hint, font_size, Vector2(left, panel.end.y - font_size * 0.8), DIM)


func _key(action: StringName, fallback: String) -> String:
	var label: String = key_label.call(action)
	return label if label != "" else fallback


func _text(font: Font, text: String, font_size: int, at: Vector2, colour: Color) -> void:
	draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 3, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)
