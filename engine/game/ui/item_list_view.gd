# SPDX-License-Identifier: GPL-3.0-or-later
#
# The one item list every item menu draws (ADR 0010): a titled panel with horizontal tabs, a search box, a
# table of sortable columns, the item card of the row under the cursor and the keys the row offers. It reads an
# ItemListModel and nothing else; what an action does is the host's business (InventoryView, MagicView,
# ContainerView, FavoritesView), told through `action_requested`. Colours, sizes, the font and the frames come
# from the game's own menus when the pack has them (UiTheme over UiAssets), else from ui/theme/menu_theme.tres.
#
# The host owns the keyboard contract (enabled, cancel, closing) and passes events to `handle_input`, which
# answers whether it used one: up and down (or the left stick) move the cursor, left and right and page up
# and down change the tab, `menu_search` starts typing in the search box (Enter or Escape ends it), and the
# keys of the row's actions report them. The mouse works too: hover chooses a row, a click chooses it and a
# second click on the chosen row does its first action, a click on a column title sorts, a click on a tab
# shows it, the wheel scrolls.
class_name ItemListView
extends Control

signal action_requested(action: StringName, row: Dictionary)
signal tab_changed

const THEME := preload("theme/menu_theme.tres")
const TYPE := &"ItemListView"
const STICK_THRESHOLD := 0.6
const WHEEL_ROWS := 3

var model: ItemListModel
## Where the panel is, as fractions of this control (the host's area).
var region := Rect2(0.1, 0.08, 0.8, 0.84)
var title := ""
## Right-aligned header texts, left to right.
var status: Array[String] = []
## The hint line shows this instead of the keys when it is not empty (a question, a refusal).
var notice := ""
## Lines under the card (a shout's words): {text, tone} with tone one of normal, dim, selected, equipped.
var detail_lines: Array[Dictionary] = []
## Whether left and right change the tab (a container uses them to change side).
var arrows_change_tab := true
## Whether the list takes the cursor keys (the side that is not chosen of a container does not).
var focused := true
## What the key labels say, by action (the viewer's gamepad or keyboard).
var key_label: Callable = func(_action: StringName) -> String: return ""

var _first := 0
var _last_selected := -1
var _stick_ready := true
var _searching := false
var _tab_first := 0  # the first tab the bar shows


func _init(list_model: ItemListModel) -> void:
	model = list_model
	theme = THEME
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	model.changed.connect(queue_redraw)
	resized.connect(_retheme)


func _retheme() -> void:
	theme = UiTheme.item_list(THEME, size.y)


## Whether the search box has the keyboard.
func is_searching() -> bool:
	return _searching


func stop_search() -> void:
	_searching = false
	queue_redraw()


# ---- input

func handle_input(event: InputEvent) -> bool:
	if _searching:
		return _search_input(event)
	if event is InputEventMouseButton or event is InputEventMouseMotion:
		return _mouse_input(event)
	if not focused:
		return false
	if event.is_action_pressed(&"ui_down"):
		model.move(1)
	elif event.is_action_pressed(&"ui_up"):
		model.move(-1)
	elif event.is_action_pressed(&"ui_page_down"):
		_change_tab(1)
	elif event.is_action_pressed(&"ui_page_up"):
		_change_tab(-1)
	elif arrows_change_tab and event.is_action_pressed(&"ui_right"):
		_change_tab(1)
	elif arrows_change_tab and event.is_action_pressed(&"ui_left"):
		_change_tab(-1)
	elif event is InputEventJoypadMotion and event.axis == JOY_AXIS_LEFT_Y:
		_stick(event.axis_value)
	elif event.is_action_pressed(&"menu_search"):
		_searching = true
		queue_redraw()
	else:
		return _action_input(event)
	return true


func _action_input(event: InputEvent) -> bool:
	var row := model.current()
	for action: Dictionary in ItemListRegistry.actions_for(model.menu, row):
		if event.is_action_pressed(action["input"]):
			action_requested.emit(StringName(action["id"]), row)
			return true
	return false


func _stick(value: float) -> void:
	if absf(value) < STICK_THRESHOLD:
		_stick_ready = true
	elif _stick_ready:
		_stick_ready = false
		model.move(1 if value > 0.0 else -1)


func _change_tab(step: int) -> void:
	model.step_tab(step)
	tab_changed.emit()


func _search_input(event: InputEvent) -> bool:
	if event is InputEventKey and event.pressed:
		if event.keycode == KEY_ENTER or event.keycode == KEY_KP_ENTER or event.keycode == KEY_ESCAPE:
			if event.keycode == KEY_ESCAPE and model.search != "":
				model.search = ""
			stop_search()
		elif event.keycode == KEY_BACKSPACE:
			model.search = model.search.left(-1)
		elif event.unicode >= 32 and event.unicode != 127:
			model.search += char(event.unicode)
		return true
	if event is InputEventMouseButton and event.pressed:
		_searching = false
		return _mouse_input(event)
	return event is InputEventKey or event is InputEventJoypadButton


func _mouse_input(event: InputEvent) -> bool:
	var at: Vector2 = make_input_local(event).position
	var layout := _layout()
	if event is InputEventMouseMotion:
		var index := _row_at(at, layout)
		if index >= 0 and index != model.selected:
			model.select(index)
			_last_selected = model.selected
		return false
	if not event.pressed or not layout["panel"].has_point(at):
		return false
	match event.button_index:
		MOUSE_BUTTON_WHEEL_UP:
			_first = maxi(_first - WHEEL_ROWS, 0)
			queue_redraw()
		MOUSE_BUTTON_WHEEL_DOWN:
			_first = clampi(_first + WHEEL_ROWS, 0, maxi(model.rows().size() - layout["fit"], 0))
			queue_redraw()
		MOUSE_BUTTON_LEFT:
			_click(at, layout, event.double_click)
		_:
			return false
	return true


func _click(at: Vector2, layout: Dictionary, double: bool) -> void:
	var tabs: Array = layout["tabs"]
	for i in tabs.size():
		if (tabs[i] as Rect2).has_point(at):
			model.tab = i
			tab_changed.emit()
			return
	if at.y > layout["search"].position.y - layout["line"] * 1.8 and at.y < layout["search"].position.y and (layout["more_left"] or layout["more_right"]):
		var left_side: bool = at.x < layout["left"] + layout["width"] / 2.0
		if left_side and layout["more_left"]:
			_tab_first = maxi(_tab_first - 1, 0)
			queue_redraw()
		elif not left_side and layout["more_right"]:
			_tab_first += 1
			queue_redraw()
		return
	if (layout["search"] as Rect2).has_point(at):
		_searching = true
		queue_redraw()
		return
	var columns: Array = layout["columns"]
	if at.y >= layout["header_y"] - layout["line"] and at.y < layout["header_y"] + layout["line"] * 0.3:
		for entry: Dictionary in columns:
			if at.x >= entry["x"] and at.x < entry["x"] + entry["w"]:
				model.sort_by(entry["column"]["id"])
				return
	var index := _row_at(at, layout)
	if index < 0:
		return
	var chosen := index == model.selected
	model.select(index)
	if chosen or double:
		var row := model.current()
		for action: Dictionary in ItemListRegistry.actions_for(model.menu, row):
			if action["input"] == &"ui_accept":
				action_requested.emit(StringName(action["id"]), row)
				return


## The width of tabs `from` to `to` inclusive.
func _span(widths: Array[float], from: int, to: int) -> float:
	var total := 0.0
	for i in range(from, to + 1):
		total += widths[i]
	return total


func _row_at(at: Vector2, layout: Dictionary) -> int:
	var rows_top: float = layout["rows_top"]
	var line: float = layout["line"]
	if not (layout["panel"] as Rect2).has_point(at) or at.y < rows_top - line * 0.8 or at.y >= rows_top - line * 0.8 + line * layout["fit"]:
		return -1
	var index := _first + int((at.y - (rows_top - line * 0.8)) / line)
	return index if index < model.rows().size() else -1


# ---- layout and drawing

func _font_size() -> int:
	var per_stage := size.y / 38.0
	if has_theme_constant(&"font_size_720", TYPE):
		per_stage = size.y * get_theme_constant(&"font_size_720", TYPE) / UiTheme.STAGE_HEIGHT
	return int(maxf(get_theme_constant(&"min_font_size", TYPE), per_stage))


## The row pitch: the game's entry spacing, scaled as the font is, else 1.45 lines.
func _line(fs: int) -> float:
	if has_theme_constant(&"row_pitch_720", TYPE) and has_theme_constant(&"font_size_720", TYPE):
		return fs * float(get_theme_constant(&"row_pitch_720", TYPE)) / get_theme_constant(&"font_size_720", TYPE)
	return fs * 1.45


## Where everything is: the panel, the tab rectangles, the search box, the columns (x and width), the rows.
func _layout() -> Dictionary:
	var fs := _font_size()
	var line := _line(fs)
	var panel := Rect2(size * region.position, size * region.size)
	var left := panel.position.x + fs
	var width := panel.size.x - fs * 2.0
	var title_y := panel.position.y + fs * 1.6
	var tabs_y := title_y + line
	var tab_rects: Array[Rect2] = []  # of the tabs shown, [tab index] -> rect; an unshown tab has an empty rect
	var font := get_theme_default_font()
	var names := model.tab_names()
	var widths: Array[float] = []
	for name in names:
		widths.append(font.get_string_size(name, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x + fs * 1.4)
	# The bar shows as many tabs as fit from `_tab_first`, moved so the chosen one is among them.
	var room := width - fs * 2.0  # for the "<" and ">" marks
	_tab_first = clampi(_tab_first, 0, maxi(names.size() - 1, 0))
	if model.tab < _tab_first:
		_tab_first = model.tab
	while _tab_first < model.tab and _span(widths, _tab_first, model.tab) > room:
		_tab_first += 1
	var x := left + fs
	var last := _tab_first
	for i in names.size():
		tab_rects.append(Rect2())
	for i in range(_tab_first, names.size()):
		if x + widths[i] > left + width - fs and i > _tab_first:
			break
		tab_rects[i] = Rect2(x, tabs_y - line * 0.8, widths[i], line)
		x += widths[i]
		last = i
	var more_left := _tab_first > 0
	var more_right := last < names.size() - 1
	var search_y := tabs_y + line
	var search := Rect2(left, search_y - line * 0.8, width, line)
	var header_y := search_y + line
	var rows_top := header_y + line
	var card_lines := 1 + detail_lines.size()
	var card_top := panel.end.y - fs * 0.8 - line * (card_lines + 0.6)
	var fit := maxi(int((card_top - rows_top) / line) + 1, 1)
	var columns: Array[Dictionary] = []
	var cols := model.columns()
	var widths_of: Array[float] = []
	var fixed := 0.0
	for column in cols:
		var w := 0.0
		if float(column.get("width", 0.0)) > 0.0:
			w = maxf(float(column["width"]) * width, font.get_string_size(String(column["title"]) + " ^", HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x + fs)
			w = maxf(w, fs * (2.5 if column["id"] == "mark" else 4.2))
		widths_of.append(w)
		fixed += w
	var cx := left
	for i in cols.size():
		var w := widths_of[i] if widths_of[i] > 0.0 else maxf(width - fixed, fs * 6.0)
		columns.append({"column": cols[i], "x": cx, "w": w})
		cx += w
	return {"panel": panel, "tabs": tab_rects, "search": search, "header_y": header_y, "rows_top": rows_top, "line": line, "fit": fit, "more_left": more_left, "more_right": more_right,
		"columns": columns, "left": left, "width": width, "title_y": title_y, "card_top": card_top, "fs": fs}


func _draw() -> void:
	var layout := _layout()
	var panel: Rect2 = layout["panel"]
	var fs: int = layout["fs"]
	var line: float = layout["line"]
	var font := get_theme_default_font()
	if has_theme_stylebox(&"panel", TYPE):
		draw_style_box(get_theme_stylebox(&"panel", TYPE), panel)
	else:
		draw_rect(panel, get_theme_color(&"panel", TYPE))
		draw_rect(panel, get_theme_color(&"border", TYPE), false, float(get_theme_constant(&"border_width", TYPE)))
	var left: float = layout["left"]
	# title and status
	_text(font, title, fs, Vector2(left, layout["title_y"]), &"selected" if focused else &"dim")
	var right := panel.end.x - fs
	for i in range(status.size() - 1, -1, -1):
		var w := font.get_string_size(status[i], HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
		_text(font, status[i], fs, Vector2(right - w, layout["title_y"]), &"normal")
		right -= w + fs * 1.5
	# tabs
	var names := model.tab_names()
	var tabs: Array = layout["tabs"]
	for i in names.size():
		var rect: Rect2 = tabs[i]
		if rect.size == Vector2.ZERO:
			continue
		var active: bool = i == model.tab
		if active:
			draw_rect(rect, get_theme_color(&"highlight", TYPE))
		_text(font, names[i], fs, Vector2(rect.position.x + fs * 0.7, rect.position.y + line * 0.8), &"selected" if active else &"dim")
	if layout["more_left"]:
		_text(font, "<", fs, Vector2(left, layout["title_y"] + line), &"selected")
	if layout["more_right"]:
		_text(font, ">", fs, Vector2(left + layout["width"] - fs * 0.7, layout["title_y"] + line), &"selected")
	# search
	var search: Rect2 = layout["search"]
	var search_text := "Search: " + model.search + ("_" if _searching else "")
	if model.search == "" and not _searching:
		search_text = "Search: (%s)" % _key(&"menu_search", "/")
	_text(font, search_text, fs, Vector2(search.position.x, search.position.y + line * 0.8), &"selected" if _searching else &"dim")
	# header
	var sort := model.sort_state()
	for entry: Dictionary in layout["columns"]:
		var column: Dictionary = entry["column"]
		var text: String = column["title"]
		if column["id"] == sort["column"] and column.get("sortable", true):
			text += " ^" if sort["ascending"] else " v"
		_cell(font, text, fs, entry, layout["header_y"], column.get("numeric", false), &"header")
	_rule(left, layout["header_y"] + line * 0.25, layout["width"])
	# rows
	var rows := model.rows()
	var fit: int = layout["fit"]
	if model.selected != _last_selected:
		_last_selected = model.selected
		if model.selected < _first:
			_first = model.selected
		elif model.selected >= _first + fit:
			_first = model.selected - fit + 1
	_first = clampi(_first, 0, maxi(rows.size() - fit, 0))
	for i in range(_first, mini(_first + fit, rows.size())):
		var row: Dictionary = rows[i]
		var y: float = layout["rows_top"] + line * (i - _first)
		var tone := &"equipped" if row.get("equipped", false) else &"normal"
		if not row.get("affordable", true):
			tone = &"dim"
		if i == model.selected:
			tone = &"selected"
			draw_rect(Rect2(left - fs * 0.5, y - line * 0.8, layout["width"] + fs, line), get_theme_color(&"highlight", TYPE))
		for entry: Dictionary in layout["columns"]:
			var column: Dictionary = entry["column"]
			_cell(font, ItemListRegistry.field_text(column["id"], row), fs, entry, y, column.get("numeric", false), tone)
	if rows.is_empty():
		_text(font, "Nothing here", fs, Vector2(left, layout["rows_top"]), &"dim")
	# card
	var card_top: float = layout["card_top"]
	_rule(left, card_top - line * 0.8, layout["width"])
	var x := left
	for field: Dictionary in model.card():
		var text := "%s %s" % [field["label"], field["text"]]
		var text_width := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
		if x + text_width > left + layout["width"]:
			break  # a narrow panel shows the first fields
		_text(font, text, fs, Vector2(x, card_top), &"normal")
		x += text_width + fs * 2.0
	for i in detail_lines.size():
		_text(font, detail_lines[i]["text"], fs, Vector2(left, card_top + line * (i + 1)), StringName(detail_lines[i].get("tone", "dim")))
	# hint
	_text(font, notice if notice != "" else _hint(), fs, Vector2(left, panel.end.y - fs * 0.8), &"dim")


## A horizontal rule: the game's line when the theme has it, else a thin border-coloured line.
func _rule(x: float, y: float, width: float) -> void:
	if has_theme_icon(&"divider", TYPE):
		var icon := get_theme_icon(&"divider", TYPE)
		draw_texture_rect(icon, Rect2(x, y - icon.get_height() * 0.5, width, icon.get_height()), false)
	else:
		draw_line(Vector2(x, y), Vector2(x + width, y), get_theme_color(&"border", TYPE), 1.0)


func _hint() -> String:
	var row := model.current()
	var parts: Array[String] = []
	for action: Dictionary in ItemListRegistry.actions_for(model.menu, row):
		parts.append("%s %s" % [_key(action["input"], _fallback_key(action["input"])), ItemListRegistry.action_label(action, row)])
	return "   ".join(parts)


func _key(action: StringName, fallback: String) -> String:
	var label: String = key_label.call(action)
	return label if label != "" else fallback


static func _fallback_key(action: StringName) -> String:
	match action:
		&"ui_accept":
			return "Enter"
		&"menu_drop":
			return "R"
		&"menu_equip_left":
			return "Q"
		&"menu_favorite":
			return "F"
	return String(action)


func _cell(font: Font, text: String, fs: int, entry: Dictionary, y: float, numeric: bool, tone: StringName) -> void:
	if text == "":
		return
	var colour := get_theme_color(tone, TYPE)
	var outline := get_theme_color(&"outline", TYPE)
	var at := Vector2(entry["x"], y)
	var align := HORIZONTAL_ALIGNMENT_RIGHT if numeric else HORIZONTAL_ALIGNMENT_LEFT
	var width: float = entry["w"] - fs * 0.5
	draw_string_outline(font, at, text, align, width, fs, get_theme_constant(&"outline_size", TYPE), outline)
	draw_string(font, at, text, align, width, fs, colour)


func _text(font: Font, text: String, fs: int, at: Vector2, tone: StringName) -> void:
	draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, get_theme_constant(&"outline_size", TYPE), get_theme_color(&"outline", TYPE))
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, get_theme_color(tone, TYPE))
