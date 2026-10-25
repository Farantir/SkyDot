# SPDX-License-Identifier: GPL-3.0-or-later
#
# A workbench's menu, one view with a mode by bench type (GameSession's
# `crafting_opened`): recipes for a create-object bench (type 1: forge,
# smelter, tanning rack, cooking pot), tempering for the grindstone and the
# armor workbench (2, 7), ingredients and a preview for the alchemy lab (5, 6),
# and enchanting for the tables (3, 4). The list is on the left, the
# components or effects of the row under the cursor on the right. Up and down
# choose a row, accept crafts one / tempers it / marks an ingredient (at most
# three, two at least to mix), `menu_drop` (R, X) mixes, cancel, Tab or I
# closes. Rows that cannot be made are dimmed.
#
# The enchanting table (3, 4) has four categories, switched with left and right:
# Disenchant (accept destroys the item and teaches its enchantment), then Item,
# Enchantment and Soul gem, where accept marks one of each (an enchantment that
# does not fit the marked item is dimmed); the right side previews the result and
# `menu_drop` enchants. `menu_equip_left` (Q, gamepad Y) types a name for it on a
# keyboard; empty keeps "<item> of <enchantment>". A Control that fills its host
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
const ENCHANT_CATEGORIES: Array[String] = ["Disenchant", "Item", "Enchantment", "Soul gem"]
const ENCHANT_KEYS: Array[String] = ["disenchant", "items", "enchantments", "gems"]

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
var _enchant := {}  # the table's lists (GameSession.enchant_rows)
var _category := 0
var _marks := {}  # enchanting: category (1 item, 2 enchantment, 3 soul gem) -> the row marked
var _name := ""  # the name typed for the enchanted item
var _naming := false
var _name_edit: LineEdit
## Whether a name can be typed (a keyboard host; a VR shell keeps the default name).
var can_name := true
var enabled: Callable = func() -> bool: return true
var key_label: Callable = func(_action: StringName) -> String: return ""


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	session.items_changed.connect(_refresh)
	_name_edit = LineEdit.new()
	_name_edit.placeholder_text = "Name (empty: default)"
	_name_edit.visible = false
	_name_edit.text_submitted.connect(_name_done)
	add_child(_name_edit)


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
	_marks.clear()
	_category = 0
	_name = ""
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
	return "items"


func close() -> void:
	if not _open:
		return
	_open = false
	_bench = 0
	_naming = false
	_name_edit.visible = false
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
			_enchant = _session.enchant_rows()
			_rows = _enchant[ENCHANT_KEYS[_category]]
			_prune_marks()
	_selected = clampi(_selected, 0, maxi(_rows.size() - 1, 0))
	_preview = _session.alchemy_preview(PackedInt64Array(_marked)) if _mode == Mode.ALCHEMY and _marked.size() >= 2 else {}
	if _mode == Mode.ENCHANT:
		_preview = _enchant_preview()
	queue_redraw()


## The marks whose item, enchantment or gem is gone (used up, destroyed) are dropped.
func _prune_marks() -> void:
	for category: int in _marks.keys():
		var found := false
		for row: Dictionary in _enchant[ENCHANT_KEYS[category]]:
			if _same_row(category, row, _marks[category]):
				_marks[category] = row  # the counts may have changed
				found = true
				break
		if not found:
			_marks.erase(category)


func _same_row(category: int, a: Dictionary, b: Dictionary) -> bool:
	if category == 2:
		return int(a["id"]) == int(b["id"])
	return int(a["form"]) == int(b["form"]) and int(a["extra"]) == int(b["extra"])


## What enchanting the three marks would make ({} until all three are marked).
func _enchant_preview() -> Dictionary:
	if not (_marks.has(1) and _marks.has(2) and _marks.has(3)):
		return {}
	var item: Dictionary = _marks[1]
	var gem: Dictionary = _marks[3]
	return _session.enchant_preview(int(item["form"]), int(item["extra"]), int(_marks[2]["id"]),
		int(gem["form"]), int(gem["extra"]), _name)


func _input(event: InputEvent) -> void:
	if not _open or not enabled.call():
		return
	if _naming:
		if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
			_name_done(_name)  # keeps the name it had
			get_viewport().set_input_as_handled()
		return  # the LineEdit takes the keys
	var handled := true
	if _mode == Mode.ENCHANT and event.is_action_pressed(&"ui_right"):
		_change_category(1)
	elif _mode == Mode.ENCHANT and event.is_action_pressed(&"ui_left"):
		_change_category(-1)
	elif _mode == Mode.ENCHANT and can_name and event.is_action_pressed(&"menu_equip_left"):
		_start_naming()
	elif event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"inventory"):
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


## The enchanting table's category by name ("Soul gem") or number; no-op elsewhere.
func select_category(category: String) -> void:
	if _mode != Mode.ENCHANT:
		return
	var index := -1
	for i in ENCHANT_CATEGORIES.size():
		if ENCHANT_CATEGORIES[i].to_lower() == category.to_lower():
			index = i
	if index < 0:
		index = category.to_int()
	_category = clampi(index, 0, ENCHANT_CATEGORIES.size() - 1)
	_selected = 0
	_refresh()


func _change_category(step: int) -> void:
	_category = (_category + step + ENCHANT_CATEGORIES.size()) % ENCHANT_CATEGORIES.size()
	_selected = 0
	_refresh()


## Flat host: a LineEdit for the item's name; VR keeps the default (no key reaches it).
func _start_naming() -> void:
	_naming = true
	_name_edit.text = _name
	_name_edit.position = size * Vector2(0.1, 0.08) + Vector2(16.0, size.y * 0.84 - 56.0)
	_name_edit.size = Vector2(size.x * 0.8 - 32.0, 36.0)
	_name_edit.visible = true
	_name_edit.grab_focus()
	_name_edit.caret_column = _name_edit.text.length()


func _name_done(text: String) -> void:
	_name = text.strip_edges()
	_naming = false
	_name_edit.release_focus()
	_name_edit.visible = false
	_refresh()


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
		Mode.ENCHANT:
			_accept_enchant(row)


func _accept_enchant(row: Dictionary) -> void:
	if _category == 0:
		if not row["ok"]:
			_session.message.emit(String(row["refusal"]), false)
			return
		var result := _session.disenchant(int(row["form"]), int(row["extra"]))
		print("crafting: disenchanted %s -> %s" % [row["name"], result.get("learned", "")])
		return
	if _marks.has(_category) and _same_row(_category, _marks[_category], row):
		_marks.erase(_category)
	elif _category == 2 and _marks.has(1) and not _session.enchantment_fits(int(_marks[1]["form"]), int(row["id"])):
		_session.message.emit("This enchantment does not fit the item", false)
		return
	else:
		_marks[_category] = row
	_refresh()


## Enchanting: make the item from the three marks.
func enchant_marked() -> void:
	if _preview.is_empty():
		return
	if not _preview["ok"]:
		_session.message.emit(String(_preview["refusal"]), false)
		return
	var item: Dictionary = _marks[1]
	var gem: Dictionary = _marks[3]
	var result := _session.enchant(int(item["form"]), int(item["extra"]), int(_marks[2]["id"]),
		int(gem["form"]), int(gem["extra"]), _name)
	if result["ok"]:
		_marks.clear()
		_name = ""
		_refresh()


## Alchemy: mix the marked ingredients if that makes a potion.
func mix() -> void:
	if _mode == Mode.ENCHANT:
		enchant_marked()
		return
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
		Mode.ENCHANT:
			return _enchant_row_text(row)
	return row["name"]


func _enchant_row_text(row: Dictionary) -> String:
	var count := (" (%d)" % int(row["count"]) if row.get("count", 1) > 1 else "")
	match _category:
		0:
			return "%s - %s%s" % [row["name"], row["enchantment"], count]
		1:
			return _mark_text(row) + row["name"] + count
		2:
			return _mark_text(row) + "%s (%s)" % [row["name"], row["kind"]]
	return _mark_text(row) + "%s%s (%s, %d)" % [row["name"], count, row["soul_name"], int(row["charge"])]


func _mark_text(row: Dictionary) -> String:
	return "[x] " if _marks.has(_category) and _same_row(_category, _marks[_category], row) else "[ ] "


## Enchanting: whether the row at `row` can be used as it stands.
func _enchant_row_ok(row: Dictionary) -> bool:
	match _category:
		0:
			return row["ok"]
		2:
			return not _marks.has(1) or _session.enchantment_fits(int(_marks[1]["form"]), int(row["id"]))
	return true


func _row_ok(row: Dictionary) -> bool:
	match _mode:
		Mode.RECIPES:
			return row["craftable"]
		Mode.TEMPER:
			return row["craftable"] and row["next_tier"] != ""
		Mode.ENCHANT:
			return _enchant_row_ok(row)
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
	var offset := 1.0
	if _mode == Mode.ENCHANT:
		offset = 2.0
		var tabs := ""
		for i in ENCHANT_CATEGORIES.size():
			tabs += ("[%s] " if i == _category else " %s  ") % ENCHANT_CATEGORIES[i]
		# All four names stay readable: a smaller font when the bar is wider than the left side.
		var tab_size := font_size
		while tab_size > 8 and font.get_string_size(tabs, HORIZONTAL_ALIGNMENT_LEFT, -1, tab_size).x > split - font_size * 2.0:
			tab_size -= 1
		_text(font, tabs, tab_size, Vector2(left, top + line), SELECTED, split - font_size * 2.0)
	var rows_fit := int((panel.end.y - top - line * (offset + 2.0)) / line)
	var first := clampi(_selected - rows_fit / 2, 0, maxi(_rows.size() - rows_fit, 0))
	for i in range(first, mini(first + rows_fit, _rows.size())):
		var row: Dictionary = _rows[i]
		var y := top + line * (i - first + offset)
		var chosen := i == _selected
		var colour := SELECTED if chosen else (NORMAL if _row_ok(row) else DIM)
		if _mode == Mode.ALCHEMY and _marked.has(int(row["form"])) and not chosen:
			colour = MARKED
		_text(font, ("> " if chosen else "  ") + _row_text(row), font_size, Vector2(left, y), colour, split - font_size * 2.0)
	if _rows.is_empty():
		_text(font, _empty_text(), font_size, Vector2(left, top + line * offset), DIM, split - font_size * 2.0)
	_draw_details(font, font_size, line, Vector2(right, top), panel.end.x - right - font_size, panel)
	var hint := "%s %s   %s close" % [_key(&"ui_accept", "Enter"), _accept_text(), _key(&"inventory", "I")]
	if _mode == Mode.ALCHEMY:
		hint = "%s mark   %s mix   %s close" % [_key(&"ui_accept", "Enter"), _key(&"menu_drop", "R"), _key(&"inventory", "I")]
	elif _mode == Mode.ENCHANT:
		hint = "%s %s   %s enchant   %s%s   left/right category   %s close" % [_key(&"ui_accept", "Enter"),
			"disenchant" if _category == 0 else "mark", _key(&"menu_drop", "R"), _key(&"menu_equip_left", "Q") + " name" if can_name else "", "", _key(&"inventory", "I")]
	_text(font, hint, font_size, Vector2(left, panel.end.y - font_size * 0.8), DIM, panel.size.x - font_size * 2.0)


func _empty_text() -> String:
	match _mode:
		Mode.RECIPES:
			return "Nothing to make here"
		Mode.TEMPER:
			return "Nothing to improve"
		Mode.ALCHEMY:
			return "No ingredients"
	return ["Nothing enchanted to disenchant", "No item to enchant", "No enchantment learned (disenchant one)",
		"No filled soul gem"][_category]


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
		Mode.ENCHANT:
			_draw_enchant_details(font, font_size, line, at, width, row)


func _draw_enchant_details(font: Font, font_size: int, line: float, at: Vector2, width: float, row: Dictionary) -> void:
	var y := at.y
	if _category == 0:
		if row.is_empty():
			return
		_text(font, "Destroys the item and teaches", font_size, Vector2(at.x, y), SELECTED, width)
		_text(font, String(row["enchantment"]), font_size, Vector2(at.x, y + line), NORMAL, width)
		if not row["ok"]:
			_text(font, String(row["refusal"]), font_size, Vector2(at.x, y + line * 2.0), SHORT, width)
		return
	var labels := ["", "Item", "Enchantment", "Soul gem"]
	for category in [1, 2, 3]:
		var text := "-"
		if _marks.has(category):
			text = String(_marks[category]["name"])
		_text(font, "%s: %s" % [labels[category], text], font_size, Vector2(at.x, y), MARKED if _marks.has(category) else DIM, width)
		y += line
	if _category == 2 and not row.is_empty():
		for effect: String in row["effects"]:
			y += line
			_text(font, effect, font_size, Vector2(at.x, y), NORMAL, width)
		y += line
	y += line * 0.5
	if _preview.is_empty():
		_text(font, "Mark an item, an enchantment and a soul gem", font_size, Vector2(at.x, y), DIM, width)
		return
	_text(font, ("Name: " + (_name if _name != "" else "(default)")), font_size, Vector2(at.x, y), DIM, width)
	y += line
	if _preview["name"] != "":
		_text(font, String(_preview["name"]), font_size, Vector2(at.x, y), MARKED if _preview["ok"] else SHORT, width)
		y += line
	if not _preview["ok"]:
		_text(font, String(_preview["refusal"]), font_size, Vector2(at.x, y), SHORT, width)
		return
	for effect: String in _preview["effects"]:
		_text(font, effect, font_size, Vector2(at.x, y), NORMAL, width)
		y += line
	if _preview["constant"]:
		_text(font, "Worn: constant effect", font_size, Vector2(at.x, y), DIM, width)
	else:
		_text(font, "Charge %d, %d uses (%.0f each)" % [int(_preview["charge"]), int(_preview["uses"]), float(_preview["cost"])],
			font_size, Vector2(at.x, y), DIM, width)


func _key(action: StringName, fallback: String) -> String:
	var label: String = key_label.call(action)
	return label if label != "" else fallback


## Text clipped (with "..") to `width` pixels.
func _text(font: Font, text: String, font_size: int, at: Vector2, colour: Color, width: float) -> void:
	while text.length() > 3 and font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x > width:
		text = text.substr(0, text.length() - 3) + ".."
	draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 3, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)
