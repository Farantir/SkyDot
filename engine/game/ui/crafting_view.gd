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

const THEME := preload("theme/menu_theme.tres")
const TYPE := &"ItemListView"
const SHORT := Color(0.9, 0.5, 0.45)
const MARKED := Color(0.6, 0.9, 0.6)
const STICK_THRESHOLD := 0.6
const MAX_INGREDIENTS := 3
const ENCHANT_CATEGORIES: Array[String] = ["Disenchant", "Item", "Enchantment", "Soul gem"]
const ENCHANT_KEYS: Array[String] = ["disenchant", "items", "enchantments", "gems"]

# The text colours: the game's (UiTheme) when the pack has them, set by _draw.
var _normal := Color(0.92, 0.92, 0.92)
var _dim := Color(0.6, 0.6, 0.6)
var _head := Color(1.0, 0.9, 0.5)  # titles and headings
var _highlight := Color(1.0, 1.0, 1.0, 0.0)  # the chosen row's fill
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
var _model_view: ItemPreview  # the 3D preview of the item under the cursor
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
	_model_view = ItemPreview.new(session)
	_model_view.region = Rect2(0.34, 0.1, 0.3, 0.5)
	add_child(_model_view)


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
	_model_view.clear()
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
	_update_model()
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
	_update_model()
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


const SWF := "craftingmenu"
const STAGE := Vector2(1280.0, 720.0)
const LIST_FONT := "$EverywhereMediumFont"
# craftingmenu.swf's placements (stage pixels), the list's measured against the game (side_crafting_forge.png).
const HEADER_RIGHT := 1123.0  # the MenuDescription's right edge
const HEADER_Y := 41.0
const LIST_X := 33.0
const LIST_CENTRE_Y := 384.0  # the chosen row stands here: the list is centred on it (CenteredList)
const LIST_PITCH := 40.5
const LIST_WIDTH := 560.0
const BAR_TOP := 650.0  # BottomBarInfo's line (the movie places the bar at y 646.55)
const CARD_RECT := Rect2(636.0, 400.0, 560.0, 236.0)  # ItemInfoHolder at (635.5, 418.85): its card and requirements
const SKILLS := {Mode.RECIPES: "Smithing", Mode.TEMPER: "Smithing", Mode.ALCHEMY: "Alchemy", Mode.ENCHANT: "Enchanting"}


## The movie's description line: "<bench>: <the menu's GMST text>".
func _header() -> String:
	var gmst := "sConstructibleMenuDescription"
	var fallback := "Choose an item to create"
	match _mode:
		Mode.TEMPER:
			gmst = "sSmithingMenuDescription"
			fallback = "Choose an item to improve"
		Mode.ALCHEMY:
			gmst = "sAlchemyMenuDescription"
			fallback = "Combine ingredients to make potions"
		Mode.ENCHANT:
			gmst = "sEnchantDeconstructMenuDescription" if _category == 0 else "sEnchantMenuDescription"
			fallback = "Choose an item to destroy and learn its effects" if _category == 0 else "Choose an item to destroy and learn its Enchantment, or combine an Item, Enchantment, and Soul Gem to create magic items"
	var text := _session.world.get_gmst_string(gmst, fallback)
	return "%s: %s" % [_title, text] if _title != "" else text


## Text in stage pixels (the game stretches the stage to the window; glyphs keep the x scale).
func _stage_text(font: Font, text: String, at: Vector2, size_stage: float, colour: Color, align := HORIZONTAL_ALIGNMENT_LEFT, width := -1.0) -> void:
	var fill := UiDraw.stage_fill(size, STAGE)
	var px := maxi(int(roundf(size_stage * fill.x.x)), 6)
	var origin := fill * at
	var wide := width * fill.x.x if width > 0.0 else -1.0
	var shown := text
	while wide > 0.0 and shown.length() > 3 and font.get_string_size(shown, HORIZONTAL_ALIGNMENT_LEFT, -1, px).x > wide:
		shown = shown.substr(0, shown.length() - 3) + ".."
	if align == HORIZONTAL_ALIGNMENT_RIGHT:
		origin.x -= font.get_string_size(shown, HORIZONTAL_ALIGNMENT_LEFT, -1, px).x
	draw_string_outline(font, origin, shown, HORIZONTAL_ALIGNMENT_LEFT, -1, px, 3, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, origin, shown, HORIZONTAL_ALIGNMENT_LEFT, -1, px, colour)


func _update_model() -> void:
	if _model_view == null:
		return
	var row := current() if _open else {}
	var form := -1
	if not row.is_empty():
		match _mode:
			Mode.RECIPES:
				form = int(row.get("product", -1))
			Mode.TEMPER:
				form = int(row.get("form", -1))
			Mode.ENCHANT:
				form = int(row.get("form", -1)) if _category in [0, 1] else -1
	_model_view.show_item(form)


func _draw() -> void:
	if not _open:
		return
	var area := size
	var th := UiTheme.item_list(THEME, area.y)
	var game_look := UiAssets.available()
	var font: Font = th.default_font if th.default_font != null else ThemeDB.fallback_font
	if game_look:
		_normal = th.get_color(&"normal", TYPE)
		_dim = th.get_color(&"dim", TYPE)
		_head = _dim
	var fill := UiDraw.stage_fill(area, STAGE)
	var sx := fill.x.x
	var sy := fill.y.y
	# The movie's lines: above the bottom bar, and the list's column.
	var line_colour := Color(1.0, 1.0, 1.0, 0.55)
	draw_line(Vector2(0.0, BAR_TOP * sy), Vector2(area.x, BAR_TOP * sy), line_colour, 1.0)
	_stage_text(font, _header(), Vector2(HEADER_RIGHT, HEADER_Y), 26.0, Color.WHITE, HORIZONTAL_ALIGNMENT_RIGHT)
	if _mode == Mode.ENCHANT:
		var tabs := ""
		for i in ENCHANT_CATEGORIES.size():
			tabs += ("[%s] " if i == _category else " %s  ") % ENCHANT_CATEGORIES[i]
		_stage_text(font, tabs, Vector2(LIST_X, 74.0), 20.0, _normal, HORIZONTAL_ALIGNMENT_LEFT, LIST_WIDTH)
	# The list, centred on the chosen row as the game's category list is.
	for i in _rows.size():
		var y := LIST_CENTRE_Y + (i - _selected) * LIST_PITCH
		if y < 100.0 or y > BAR_TOP - 10.0:
			continue
		var row: Dictionary = _rows[i]
		var chosen := i == _selected
		var colour := _normal if chosen and _row_ok(row) else _dim
		if _mode == Mode.ALCHEMY and _marked.has(int(row["form"])):
			colour = MARKED
		_stage_text(font, _row_text(row), Vector2(LIST_X, y), 26.0 if chosen else 20.0, colour, HORIZONTAL_ALIGNMENT_LEFT, LIST_WIDTH)
	if _rows.is_empty():
		_stage_text(font, _empty_text(), Vector2(LIST_X, LIST_CENTRE_Y), 20.0, _dim, HORIZONTAL_ALIGNMENT_LEFT, LIST_WIDTH)
	# The card: requirements, effects, the preview of what is made.
	var frame := th.get_stylebox(&"panel", TYPE) if th.has_stylebox(&"panel", TYPE) else null
	var card := Rect2(fill * CARD_RECT.position, CARD_RECT.size * Vector2(sx, sy))
	if frame != null and not _rows.is_empty():
		draw_style_box(frame, card)
	if not _rows.is_empty() or _mode == Mode.ENCHANT:
		var px := int(roundf(20.0 * sx))
		_draw_details(font, px, 30.0 * sy, card.position + Vector2(px * 1.5, px * 2.2), card.size.x - px * 2.0, card)
	_draw_bottom_bar(font)
	_update_model_rect()


func _draw_bottom_bar(font: Font) -> void:
	var fill := UiDraw.stage_fill(size, STAGE)
	var exit_icon := UiAssets.exported(SWF, "Tab")
	if exit_icon >= 0:
		UiDraw.character(self, SWF, exit_icon, fill * Transform2D(Vector2(0.8, 0.0), Vector2(0.0, 0.8), Vector2(12.0, 662.0)))
	_stage_text(font, "Exit", Vector2(46.0, 684.0), 20.0, Color.WHITE)
	var hint := _accept_text() if _mode != Mode.ENCHANT else ("disenchant" if _category == 0 else "mark")
	if _mode == Mode.ALCHEMY:
		hint = "mark   %s mix" % _key(&"menu_drop", "R")
	elif _mode == Mode.ENCHANT:
		hint += "   %s enchant   %s name   left/right category" % [_key(&"menu_drop", "R"), _key(&"menu_equip_left", "Q")] if can_name else "   %s enchant" % _key(&"menu_drop", "R")
	_stage_text(font, "%s %s" % [_key(&"ui_accept", "Enter"), hint], Vector2(140.0, 684.0), 20.0, _dim, HORIZONTAL_ALIGNMENT_LEFT, 520.0)
	var skill: String = SKILLS.get(_mode, "Smithing")
	var level := int(_session.actor_value(0x14, skill))
	_stage_text(font, skill, Vector2(770.0, 680.0), 16.0, _dim)
	_stage_text(font, str(level), Vector2(833.0, 684.0), 20.0, Color.WHITE)
	_stage_text(font, str(mini(level + 1, 100)), Vector2(1015.0, 684.0), 20.0, Color.WHITE)
	var bar := Rect2(fill * Vector2(868.0, 671.0), Vector2(140.0, 14.0) * Vector2(fill.x.x, fill.y.y))
	draw_rect(bar, Color(0.0, 0.0, 0.0, 0.6))
	draw_rect(bar, Color(1.0, 1.0, 1.0, 0.55), false, 1.0)


func _update_model_rect() -> void:
	if _model_view != null:
		_model_view.region = Rect2(0.34, 0.1, 0.3, 0.5)


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
			_text(font, "Needs", font_size, Vector2(at.x, y), _head, width)
			for c: Dictionary in row["components"]:
				y += line
				var have := int(c["have"])
				var need := int(c["need"])
				_text(font, "%s %d/%d" % [c["name"], have, need], font_size, Vector2(at.x, y), _normal if have >= need else SHORT, width)
		Mode.ALCHEMY:
			if not row.is_empty():
				_text(font, "Effects", font_size, Vector2(at.x, y), _head, width)
				for effect: Dictionary in row["effects"]:
					y += line
					_text(font, effect["name"], font_size, Vector2(at.x, y), _normal if effect["known"] else _dim, width)
			y = maxf(y + line * 1.5, panel.position.y + panel.size.y * 0.5)
			if _marked.size() < 2:
				_text(font, "Mark two or three ingredients", font_size, Vector2(at.x, y), _dim, width)
			elif not _preview.get("mixable", false):
				_text(font, "These do not mix", font_size, Vector2(at.x, y), SHORT, width)
			else:
				_text(font, _preview["name"], font_size, Vector2(at.x, y), MARKED, width)
				for effect: Dictionary in _preview["effects"]:
					y += line
					_text(font, effect["description"], font_size, Vector2(at.x, y), _normal, width)
				y += line
				_text(font, "Value %d" % int(_preview["value"]), font_size, Vector2(at.x, y), _dim, width)
		Mode.ENCHANT:
			_draw_enchant_details(font, font_size, line, at, width, row)


func _draw_enchant_details(font: Font, font_size: int, line: float, at: Vector2, width: float, row: Dictionary) -> void:
	var y := at.y
	if _category == 0:
		if row.is_empty():
			return
		_text(font, "Destroys the item and teaches", font_size, Vector2(at.x, y), _head, width)
		_text(font, String(row["enchantment"]), font_size, Vector2(at.x, y + line), _normal, width)
		if not row["ok"]:
			_text(font, String(row["refusal"]), font_size, Vector2(at.x, y + line * 2.0), SHORT, width)
		return
	var labels := ["", "Item", "Enchantment", "Soul gem"]
	for category in [1, 2, 3]:
		var text := "-"
		if _marks.has(category):
			text = String(_marks[category]["name"])
		_text(font, "%s: %s" % [labels[category], text], font_size, Vector2(at.x, y), MARKED if _marks.has(category) else _dim, width)
		y += line
	if _category == 2 and not row.is_empty():
		for effect: String in row["effects"]:
			y += line
			_text(font, effect, font_size, Vector2(at.x, y), _normal, width)
		y += line
	y += line * 0.5
	if _preview.is_empty():
		_text(font, "Mark an item, an enchantment and a soul gem", font_size, Vector2(at.x, y), _dim, width)
		return
	_text(font, ("Name: " + (_name if _name != "" else "(default)")), font_size, Vector2(at.x, y), _dim, width)
	y += line
	if _preview["name"] != "":
		_text(font, String(_preview["name"]), font_size, Vector2(at.x, y), MARKED if _preview["ok"] else SHORT, width)
		y += line
	if not _preview["ok"]:
		_text(font, String(_preview["refusal"]), font_size, Vector2(at.x, y), SHORT, width)
		return
	for effect: String in _preview["effects"]:
		_text(font, effect, font_size, Vector2(at.x, y), _normal, width)
		y += line
	if _preview["constant"]:
		_text(font, "Worn: constant effect", font_size, Vector2(at.x, y), _dim, width)
	else:
		_text(font, "Charge %d, %d uses (%.0f each)" % [int(_preview["charge"]), int(_preview["uses"]), float(_preview["cost"])],
			font_size, Vector2(at.x, y), _dim, width)


func _key(action: StringName, fallback: String) -> String:
	var label: String = key_label.call(action)
	return label if label != "" else fallback


## Text clipped (with "..") to `width` pixels.
func _text(font: Font, text: String, font_size: int, at: Vector2, colour: Color, width: float) -> void:
	while text.length() > 3 and font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x > width:
		text = text.substr(0, text.length() - 3) + ".."
	draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 3, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)
