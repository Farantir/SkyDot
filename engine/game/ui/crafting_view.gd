# SPDX-License-Identifier: GPL-3.0-or-later
#
# A workbench's menu, one view with a mode by bench type (GameSession's
# `crafting_opened`): recipes for a create-object bench (type 1: forge,
# smelter, tanning rack, cooking pot), tempering for the grindstone and the
# armor workbench (2, 7), ingredients and a preview for the alchemy lab (5, 6),
# and a stub for the enchanting tables (3, 4). The list is on the left, the
# components or effects of the row under the cursor on the right. Up and down
# choose a row, accept crafts one / tempers it / marks an ingredient (at most
# three, two at least to mix), `menu_drop` (R, X) mixes, cancel, Tab or I
# closes. Rows that cannot be made are dimmed. A Control that fills its host
# (see InventoryView for the contract); the game logic stays in GameSession.
class_name CraftingView
extends Control

signal engaged(open: bool)

enum Mode { RECIPES, TEMPER, ALCHEMY, ENCHANT }

const NORMAL := Color(0.92, 0.92, 0.92)
const DIM := Color(0.6, 0.6, 0.6)
const SELECTED := Color(1.0, 0.9, 0.5)
const SHORT := Color(0.9, 0.5, 0.45)
const MARKED := Color(0.6, 0.9, 0.6)
const STICK_THRESHOLD := 0.6
const MAX_INGREDIENTS := 3

var _session: GameSession
var _open := false
var _bench := 0
var _bench_type := 0
var _title := ""
var _mode := Mode.RECIPES
var _selected := 0
var _rows: Array = []
var _marked: Array[int] = []  # alchemy: the forms chosen, in order
var _preview := {}
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


## The bench being used (0 when closed).
func bench() -> int:
	return _bench if _open else 0


func open(ref: int, bench_type: int) -> void:
	_bench = ref
	_bench_type = bench_type
	_title = _session.bench_name(ref)
	_mode = mode_of(bench_type)
	_selected = 0
	_marked.clear()
	var was_open := _open
	_open = true
	_refresh()
	print("crafting: %s opens (type %d, %d %s)" % [_title, bench_type, _rows.size(), _unit()])
	if not was_open:
		engaged.emit(true)


static func mode_of(bench_type: int) -> Mode:
	match bench_type:
		2, 7:
			return Mode.TEMPER
		5, 6:
			return Mode.ALCHEMY
		3, 4:
			return Mode.ENCHANT
	return Mode.RECIPES


func _unit() -> String:
	match _mode:
		Mode.RECIPES:
			return "recipes"
		Mode.TEMPER:
			return "items"
		Mode.ALCHEMY:
			return "ingredients"
	return "enchanted items"


func close() -> void:
	if not _open:
		return
	_open = false
	_bench = 0
	engaged.emit(false)
	queue_redraw()


func current() -> Dictionary:
	return _rows[_selected] if _selected < _rows.size() else {}


func marked() -> Array[int]:
	return _marked


func _refresh() -> void:
	if not _open:
		return
	match _mode:
		Mode.RECIPES:
			_rows = _session.recipes(_bench)
		Mode.TEMPER:
			_rows = _session.temper_rows(_bench)
		Mode.ALCHEMY:
			_rows = _session.alchemy_rows()
			var held := {}
			for row: Dictionary in _rows:
				held[int(row["form"])] = true
			_marked = _marked.filter(func(form: int) -> bool: return held.has(form))
		Mode.ENCHANT:
			_rows = _session.enchant_rows()
	_selected = clampi(_selected, 0, maxi(_rows.size() - 1, 0))
	_preview = _session.alchemy_preview(PackedInt64Array(_marked)) if _mode == Mode.ALCHEMY and _marked.size() >= 2 else {}
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
	elif event is InputEventJoypadMotion and event.axis == JOY_AXIS_LEFT_Y:
		_stick(event.axis_value)
	elif event.is_action_pressed(&"ui_accept"):
		accept()
	elif event.is_action_pressed(&"menu_drop"):
		mix()
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


## Craft one, temper, or mark / unmark an ingredient.
func accept() -> void:
	var row := current()
	if row.is_empty():
		return
	match _mode:
		Mode.RECIPES:
			if row["craftable"]:
				_session.craft(int(row["recipe"]), 1)
		Mode.TEMPER:
			if row["craftable"] and row["next_tier"] != "":
				_session.temper(int(row["recipe"]), int(row["form"]), int(row["extra"]))
		Mode.ALCHEMY:
			var form := int(row["form"])
			if _marked.has(form):
				_marked.erase(form)
			elif _marked.size() < MAX_INGREDIENTS:
				_marked.append(form)
			_refresh()


## Alchemy: mix the marked ingredients if that makes a potion.
func mix() -> void:
	if _mode != Mode.ALCHEMY or _marked.size() < 2:
		return
	var potion := _session.alchemy_mix(PackedInt64Array(_marked))
	if potion != 0:
		print("crafting: mixed %s" % _preview.get("name", ""))
		_marked.clear()
		_refresh()


func _row_text(row: Dictionary) -> String:
	match _mode:
		Mode.RECIPES:
			return row["name"] + (" x%d" % int(row["count"]) if int(row["count"]) > 1 else "")
		Mode.TEMPER:
			var text: String = row["name"]
			return text + (" -> " + row["next_tier"] if row["next_tier"] != "" else " (best)")
		Mode.ALCHEMY:
			var text: String = row["name"] + (" (%d)" % int(row["count"]) if int(row["count"]) > 1 else "")
			return ("[x] " if _marked.has(int(row["form"])) else "[ ] ") + text
	return row["name"]


func _row_ok(row: Dictionary) -> bool:
	match _mode:
		Mode.RECIPES:
			return row["craftable"]
		Mode.TEMPER:
			return row["craftable"] and row["next_tier"] != ""
	return true


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
	var split := panel.size.x * 0.52
	var left := panel.position.x + font_size
	var right := panel.position.x + split + font_size
	var top := panel.position.y + font_size * 2.0
	_text(font, _title if _title != "" else "Workbench", font_size, Vector2(left, top - font_size * 0.4), SELECTED, split - font_size * 2.0)
	draw_line(Vector2(panel.position.x + split, panel.position.y + 4.0), Vector2(panel.position.x + split, panel.end.y - 4.0), Color(0.5, 0.45, 0.35))
	if _mode == Mode.ENCHANT:
		_text(font, "Enchanting needs magic effects (step 7)", font_size, Vector2(right, top), DIM, panel.end.x - right - font_size)
	var rows_fit := int((panel.end.y - top - line * 3.0) / line)
	var first := clampi(_selected - rows_fit / 2, 0, maxi(_rows.size() - rows_fit, 0))
	for i in range(first, mini(first + rows_fit, _rows.size())):
		var row: Dictionary = _rows[i]
		var y := top + line * (i - first + 1)
		var chosen := i == _selected
		var colour := SELECTED if chosen else (NORMAL if _row_ok(row) else DIM)
		if _mode == Mode.ALCHEMY and _marked.has(int(row["form"])) and not chosen:
			colour = MARKED
		_text(font, ("> " if chosen else "  ") + _row_text(row), font_size, Vector2(left, y), colour, split - font_size * 2.0)
	if _rows.is_empty():
		_text(font, _empty_text(), font_size, Vector2(left, top + line), DIM, split - font_size * 2.0)
	_draw_details(font, font_size, line, Vector2(right, top), panel.end.x - right - font_size, panel)
	var hint := "%s %s   %s close" % [_key(&"ui_accept", "Enter"), _accept_text(), _key(&"inventory", "I")]
	if _mode == Mode.ALCHEMY:
		hint = "%s mark   %s mix   %s close" % [_key(&"ui_accept", "Enter"), _key(&"menu_drop", "R"), _key(&"inventory", "I")]
	_text(font, hint, font_size, Vector2(left, panel.end.y - font_size * 0.8), DIM, panel.size.x - font_size * 2.0)


func _empty_text() -> String:
	match _mode:
		Mode.RECIPES:
			return "Nothing to make here"
		Mode.TEMPER:
			return "Nothing to improve"
		Mode.ALCHEMY:
			return "No ingredients"
	return "No enchanted items"


func _accept_text() -> String:
	match _mode:
		Mode.RECIPES:
			return "craft"
		Mode.TEMPER:
			return "improve"
		Mode.ALCHEMY:
			return "mark"
	return "select"


func _draw_details(font: Font, font_size: int, line: float, at: Vector2, width: float, panel: Rect2) -> void:
	var row := current()
	var y := at.y
	match _mode:
		Mode.RECIPES, Mode.TEMPER:
			if row.is_empty():
				return
			_text(font, "Needs", font_size, Vector2(at.x, y), SELECTED, width)
			for c: Dictionary in row["components"]:
				y += line
				var have := int(c["have"])
				var need := int(c["need"])
				_text(font, "%s %d/%d" % [c["name"], have, need], font_size, Vector2(at.x, y), NORMAL if have >= need else SHORT, width)
		Mode.ALCHEMY:
			if not row.is_empty():
				_text(font, "Effects", font_size, Vector2(at.x, y), SELECTED, width)
				for effect: Dictionary in row["effects"]:
					y += line
					_text(font, effect["name"], font_size, Vector2(at.x, y), NORMAL if effect["known"] else DIM, width)
			y = maxf(y + line * 1.5, panel.position.y + panel.size.y * 0.5)
			if _marked.size() < 2:
				_text(font, "Mark two or three ingredients", font_size, Vector2(at.x, y), DIM, width)
			elif not _preview.get("mixable", false):
				_text(font, "These do not mix", font_size, Vector2(at.x, y), SHORT, width)
			else:
				_text(font, _preview["name"], font_size, Vector2(at.x, y), MARKED, width)
				for effect: Dictionary in _preview["effects"]:
					y += line
					_text(font, effect["description"], font_size, Vector2(at.x, y), NORMAL, width)
				y += line
				_text(font, "Value %d" % int(_preview["value"]), font_size, Vector2(at.x, y), DIM, width)


func _key(action: StringName, fallback: String) -> String:
	var label: String = key_label.call(action)
	return label if label != "" else fallback


## Text clipped (with "..") to `width` pixels.
func _text(font: Font, text: String, font_size: int, at: Vector2, colour: Color, width: float) -> void:
	while text.length() > 3 and font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x > width:
		text = text.substr(0, text.length() - 3) + ".."
	draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 3, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)
