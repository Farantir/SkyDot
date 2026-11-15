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
var _cats: Array[String] = []  # smithing: the categories in play (see MATERIALS), grouped lists as the game shows them
var _cat := 0
var _in_items := false  # the focus is on the item column
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
	_model_view.region = Rect2(0.6, 0.22, 0.2, 0.28)
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
	_cat = 0
	_in_items = _mode == Mode.ALCHEMY
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
	_group()
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
	if event is InputEventMouseButton or event is InputEventMouseMotion:
		if _mouse_input(event):
			get_viewport().set_input_as_handled()
		return
	if _columns_input(event):
		get_viewport().set_input_as_handled()
		return
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
	elif _mode == Mode.ALCHEMY and event.is_action_pressed(&"menu_favorite"):
		_marked.clear()
		_refresh()
	elif event.is_action_pressed(&"menu_drop"):
		mix()
	else:
		handled = event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	if handled:
		get_viewport().set_input_as_handled()


## The enchanting table's category by name ("Soul gem") or number; no-op elsewhere.
func select_category(category: String) -> void:
	if _grouped():  # the forge and tempering: a category by name, its items focused (for shots)
		var found := _cats.find(category.to_upper())
		if found >= 0:
			_cat = found
			_selected = _first_of_cat()
			_in_items = true
			_update_model()
			queue_redraw()
		return
	if _mode != Mode.ENCHANT:
		return
	var index := -1
	for i in ENCHANT_CATEGORIES.size():
		if ENCHANT_CATEGORIES[i].to_lower() == category.to_lower():
			index = i
	if index < 0:
		index = category.to_int()
	_category = clampi(index, 0, ENCHANT_CATEGORIES.size() - 1)
	_in_items = true
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


## The categories the game names with GMSTs (sHide, sIron, ...), in the order the forge shows the ones seen in the game
## (Hide, Iron, Studded, Leather, Jewelry, Building materials, Misc); the other materials follow Leather in the game's
## material order (unchecked in the game: only these were on screen).
const MATERIALS: Array[String] = ["Hide", "Iron", "Studded", "Leather", "Steel", "Dwarven", "Elven", "Orcish", "Glass", "Ebony",
	"Daedric", "Dragon", "Imperial", "Falmer"]
const MATERIAL_SUFFIXES: Array[String] = ["", "Banded", "Plate", "Gilded", "Scale", "Scaled", "Light", "Heavy"]
const JEWELRY_KEYWORDS: Array[String] = ["JewelryExpensive", "ClothingNecklace", "ClothingRing", "ArmorJewelry", "ClothingCirclet"]
static var _keyword_category := {}  # keyword form -> "Hide" ...; filled once from the pack's keyword table
static var _jewelry := {}


## Whether the list is grouped in categories with a column of its own (the forge and the tempering benches).
func _grouped() -> bool:
	return _mode == Mode.RECIPES or _mode == Mode.TEMPER


func _load_keywords() -> void:
	if not _keyword_category.is_empty():
		return
	for material in MATERIALS:
		for prefix in ["ArmorMaterial", "WeapMaterial"]:
			for suffix in MATERIAL_SUFFIXES:
				var id := _session.world.find_keyword(prefix + material + suffix)
				if id != 0:
					_keyword_category[id] = material
	for name in JEWELRY_KEYWORDS:
		var id := _session.world.find_keyword(name)
		if id != 0:
			_jewelry[id] = true
	_keyword_category[0] = ""  # never empty again


## The game's category of a recipe's product: by its material keyword (ArmorMaterialHide -> the sHide text), jewelry by
## its keywords, other misc items as building materials. Tempering lists the sWeapons / sArmor groups.
## Which keyword the game ranks first when an item has several is unknown (the first listed wins).
func _category_of(row: Dictionary) -> String:
	var form := int(row["product"]) if row.has("product") else int(row["form"])
	var info := _session.item_info(form)
	var kind := String(info.get("category", ""))
	if _mode == Mode.TEMPER:
		return _session.world.get_gmst_string("sWeapons", "Weapons").to_upper() if kind == "weapon" else _session.world.get_gmst_string("sArmor", "Armor").to_upper()
	_load_keywords()
	var keywords := _session.world.get_base_keywords(form)
	for keyword in keywords:
		if _jewelry.has(int(keyword)):
			return _session.world.get_gmst_string("sJewelry", "Jewelry").to_upper()
	for keyword in keywords:
		if _keyword_category.has(int(keyword)):
			var material: String = _keyword_category[int(keyword)]
			return _session.world.get_gmst_string("s" + material, material).to_upper()
	if kind == "misc":
		return "BUILDING MATERIALS"
	return _session.world.get_gmst_string("sMisc", "Misc").to_upper()


func _group() -> void:
	if not _grouped():
		_cats.clear()
		return
	var found := {}
	for row: Dictionary in _rows:
		found[_category_of(row)] = true
	_cats.clear()
	var order: Array[String] = []
	if _mode == Mode.RECIPES:
		for material in MATERIALS.slice(0, 4):
			order.append(_session.world.get_gmst_string("s" + material, material).to_upper())
		order.append(_session.world.get_gmst_string("sJewelry", "Jewelry").to_upper())
		for material in MATERIALS.slice(4):
			order.append(_session.world.get_gmst_string("s" + material, material).to_upper())
		order.append("BUILDING MATERIALS")
		order.append(_session.world.get_gmst_string("sMisc", "Misc").to_upper())
	else:
		order = [_session.world.get_gmst_string("sWeapons", "Weapons").to_upper(), _session.world.get_gmst_string("sArmor", "Armor").to_upper()]
	for c in order:
		if found.has(c):
			_cats.append(c)
	_cat = clampi(_cat, 0, maxi(_cats.size() - 1, 0))
	if _selected < _rows.size() and not _cats.is_empty() and _category_of(_rows[_selected]) != _cats[_cat]:
		_selected = _first_of_cat()


func _cat_rows() -> Array[int]:
	var out: Array[int] = []
	if _cats.is_empty():
		return out
	for i in _rows.size():
		if _category_of(_rows[i]) == _cats[_cat]:
			out.append(i)
	return out


func _first_of_cat() -> int:
	var rows := _cat_rows()
	return rows[0] if not rows.is_empty() else 0


## Keys of the column layout: the grouped lists (forge, tempering) have their own; at the enchanting table up/down
## choose Disenchant / Item / Enchantment / Soul Gem until accept or right enters the list, left leaves it. Cancel
## (Tab, Esc) closes the menu from either column, as the game's forge does (SkyrimRemote: Tab in the item column exits).
## The alchemy lab has the one category, so its list has the focus from the start.
func _columns_input(event: InputEvent) -> bool:
	if _grouped():
		return _grouped_input(event)
	if _mode != Mode.ENCHANT or _naming:
		return false
	if event.is_action_pressed(&"ui_down") or event.is_action_pressed(&"ui_up"):
		_move(1 if event.is_action_pressed(&"ui_down") else -1)
	elif event is InputEventJoypadMotion and event.axis == JOY_AXIS_LEFT_Y:
		_stick(event.axis_value)
	elif event.is_action_pressed(&"ui_right") or (event.is_action_pressed(&"ui_accept") and not _in_items):
		if not _in_items and not _rows.is_empty():
			_in_items = true
			_selected = 0
			_update_model()
			queue_redraw()
	elif event.is_action_pressed(&"ui_left"):
		_in_items = false
		queue_redraw()
	else:
		return false
	return true


## Keys of the grouped lists: up/down move in the focused column, accept / right enter the items (accept then crafts),
## left leaves them; cancel closes the menu (_input). Returns whether the event was used.
func _grouped_input(event: InputEvent) -> bool:
	if event.is_action_pressed(&"ui_down") or event.is_action_pressed(&"ui_up"):
		_move(1 if event.is_action_pressed(&"ui_down") else -1)
	elif event is InputEventJoypadMotion and event.axis == JOY_AXIS_LEFT_Y:
		_stick(event.axis_value)
	elif event.is_action_pressed(&"ui_right") or (event.is_action_pressed(&"ui_accept") and not _in_items):
		if not _in_items and not _cat_rows().is_empty():
			_in_items = true
			_selected = _first_of_cat()
			_update_model()
			queue_redraw()
	elif event.is_action_pressed(&"ui_left"):
		_in_items = false
		queue_redraw()
	elif event.is_action_pressed(&"ui_accept"):
		accept()
	else:
		return false
	return true


## The mouse as craftingmenu.swf takes it: the wheel over the category column scrolls the categories
## (CenteredScrollingList.onMouseWheel), over the item column or the item's 3D view it moves the item selection
## (CraftingMenu.onMouseWheel: up is moveSelectionUp) once the items have the focus; a click picks a category or an
## item, a click on the chosen item accepts it. Hovering does not select (the centred lists move only by keys,
## wheel and clicks). True if the event was used.
func _mouse_input(event: InputEvent) -> bool:
	if not event is InputEventMouseButton or not event.pressed:
		return false
	var at: Vector2 = UiDraw.stage_fill(size, STAGE).affine_inverse() * make_input_local(event).position
	if at.y >= BAR_TOP:
		return false
	var in_categories := at.x < CATEGORY_COLUMN_RIGHT
	var step := 0
	match event.button_index:
		MOUSE_BUTTON_WHEEL_UP:
			step = -1
		MOUSE_BUTTON_WHEEL_DOWN:
			step = 1
		MOUSE_BUTTON_LEFT:
			_click(at, in_categories)
			return true
		_:
			return false
	if in_categories:
		_in_items = false
		_move(step)
	elif _in_items:
		_move(step)
	return true


## A left click at stage point `at`: the category or item row there, by the rows' centred places (_draw_columns).
func _click(at: Vector2, in_categories: bool) -> void:
	if in_categories:
		var names := _category_names()
		var offset := int(round((at.y - LIST_CENTRE_Y) / LIST_PITCH))
		var target := _category_index() + offset
		if target < 0 or target >= names.size() or offset == 0 and not _in_items:
			return
		_in_items = false
		_move(offset)
		return
	if at.x > ITEM_COLUMN_RIGHT:
		return
	var rows := _column_rows()
	if rows.is_empty():
		return
	var at_row := maxi(rows.find(_selected), 0)
	var target := at_row + int(round((at.y - ITEM_ROW_Y) / LIST_PITCH))
	if target < 0 or target >= rows.size():
		return
	if _in_items and rows[target] == _selected:
		accept()
		return
	_in_items = true
	_selected = rows[target]
	_update_model()
	queue_redraw()


func _stick(value: float) -> void:
	if absf(value) < STICK_THRESHOLD:
		_stick_ready = true
	elif _stick_ready:
		_stick_ready = false
		_move(1 if value > 0.0 else -1)


func _move(step: int) -> void:
	if _mode == Mode.ENCHANT and not _in_items:
		_change_category(step)
		return
	if _rows.is_empty():
		return
	if _grouped() and not _cats.is_empty():
		if _in_items:
			var rows := _cat_rows()
			_selected = rows[(rows.find(_selected) + step + rows.size()) % rows.size()]
		else:
			_cat = (_cat + step + _cats.size()) % _cats.size()
			_selected = _first_of_cat()
		_update_model()
		queue_redraw()
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
			return String(row["name"])
		Mode.ALCHEMY:
			var text: String = row["name"] + (" (%d)" % int(row["count"]) if int(row["count"]) > 1 else "")
			return text
		Mode.ENCHANT:
			return _enchant_row_text(row)
	return row["name"]


func _enchant_row_text(row: Dictionary) -> String:
	var count := (" (%d)" % int(row["count"]) if row.get("count", 1) > 1 else "")
	return String(row["name"]) + (count if _category == 1 else "")


func _mark_text(row: Dictionary) -> String:
	return ""  # a marked row is shown white (see _draw_columns), as the game shows the marked parts under "Requires:"


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
const LIST_CENTRE_Y := 392.0  # the chosen row stands here: the list is centred on it (CenteredList)
const LIST_PITCH := 40.5
const CATEGORY_COLUMN_RIGHT := 203.6  # the line between the category and the item columns (_draw_columns)
const ITEM_COLUMN_RIGHT := 473.7
const ITEM_ROW_Y := 386.0  # the chosen item's row
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
			gmst = "sEnchantMenuDescription"  # the game keeps it for every part (Disenchant too)
			fallback = "Choose an item to destroy and learn its effects" if _category == 0 else "Choose an item to destroy and learn its Enchantment, or combine an Item, Enchantment, and Soul Gem to create magic items"
	var text := _session.world.get_gmst_string(gmst, fallback)
	var name := _title
	match _mode:  # the game names these menus by their skill, not by the furniture
		Mode.TEMPER:
			name = _session.world.get_gmst_string("sWeaponSmithing" if _bench_type == 2 else "sArmorSmithing", "Weapon Smithing" if _bench_type == 2 else "Armor Smithing")
		Mode.ALCHEMY:
			name = _session.world.get_gmst_string("sAlchemy", "Alchemy")
		Mode.ENCHANT:
			name = _session.world.get_gmst_string("sEnchanting", "Enchanting")
	return "%s: %s" % [name, text] if name != "" else text


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
				form = int(row.get("form", -1)) if _category in [0, 1, 3] else -1
			Mode.ALCHEMY:
				form = int(row.get("form", -1))
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
	var header_lines := _wrap(font, _header(), 26.0, HEADER_WIDTH)
	for i in header_lines.size():
		_stage_centred(font, header_lines[i], Vector2(HEADER_CENTRE, HEADER_Y + i * 28.0), 26.0, Color.WHITE)
	_draw_columns(font, th, fill)
	_draw_bottom_bar(font)
	_update_model_rect()
	return


const HEADER_CENTRE := 950.0  # the description's centre (measured: 816 px of 1100)
const HEADER_WIDTH := 560.0


## The words of `text` in lines no wider than `width` stage pixels at `size_stage`.
func _wrap(font: Font, text: String, size_stage: float, width: float) -> Array[String]:
	var px := maxi(int(roundf(size_stage * UiDraw.stage_fill(size, STAGE).x.x)), 6)
	var lines: Array[String] = []
	var current := ""
	for word in text.split(" "):
		var trial := word if current == "" else current + " " + word
		if current != "" and font.get_string_size(trial, HORIZONTAL_ALIGNMENT_LEFT, -1, px).x / UiDraw.stage_fill(size, STAGE).x.x > width:
			lines.append(current)
			current = word
		else:
			current = trial
	if current != "":
		lines.append(current)
	return lines


## The category column of the mode: names, the chosen one, and whether names are upper case (the forge's are).
func _category_names() -> Array[String]:
	if _grouped():
		return _cats
	if _mode == Mode.ENCHANT:
		var names: Array[String] = []
		for name in ENCHANT_CATEGORIES:
			names.append(name)
		return names
	return ["INGREDIENTS"]


func _category_index() -> int:
	if _grouped():
		return _cat
	return _category if _mode == Mode.ENCHANT else 0


## The rows of the item column (indices into _rows).
func _column_rows() -> Array[int]:
	if _grouped():
		return _cat_rows()
	var out: Array[int] = []
	for i in _rows.size():
		out.append(i)
	return out


## The game's three columns (categories, items, the item's card), measured in side_crafting_forge.png and the
## alchemy / enchanting shots: lines at 175, 197 and 407 px of 1100, the chosen category big and white, the item
## column dim until it has the focus.
func _draw_columns(font: Font, th: Theme, fill: Transform2D) -> void:
	var line_colour := Color(1.0, 1.0, 1.0, 0.5)
	for x in [203.6, 229.3, 473.7]:
		draw_line(fill * Vector2(x, 0.0), fill * Vector2(x, BAR_TOP), line_colour, 1.0)
	var names := _category_names()
	var at_cat := _category_index()
	for i in names.size():
		var y := LIST_CENTRE_Y + (i - at_cat) * LIST_PITCH
		if _mode == Mode.ENCHANT:
			y += _enchant_gap(i, at_cat)
		if y > 40.0 and y < BAR_TOP - 10.0:
			_stage_text(font, names[i], Vector2(LIST_X, y), 26.0 if i == at_cat else 20.0, Color.WHITE if i == at_cat else _dim)
	if _mode == Mode.ENCHANT:
		var rule_y := LIST_CENTRE_Y + (0 - at_cat) * LIST_PITCH + _enchant_gap(0, at_cat) + 40.0
		draw_line(fill * Vector2(LIST_X, rule_y), fill * Vector2(LIST_X + 110.0, rule_y), line_colour, 1.0)
	var rows := _column_rows()
	var at := maxi(rows.find(_selected), 0)
	for n in rows.size():
		var y := ITEM_ROW_Y + (n - at) * LIST_PITCH
		if y < 40.0 or y > BAR_TOP - 10.0:
			continue
		var row: Dictionary = _rows[rows[n]]
		var chosen := n == at
		var colour := Color.WHITE if chosen and _in_items and _row_ok(row) else _dim
		if _mode == Mode.ALCHEMY and _marked.has(int(row["form"])):
			colour = MARKED
		elif _mode == Mode.ENCHANT and _category > 0 and _marks.has(_category) and _same_row(_category, _marks[_category], row):
			colour = MARKED
		_stage_text(font, _row_text(row), Vector2(252.0, y), 20.0 if chosen else 18.0, colour, HORIZONTAL_ALIGNMENT_LEFT, 215.0)
		if colour == MARKED:
			_draw_mark(fill, Vector2(236.0, y - 6.0))
	var row := current()
	_draw_card(font, th, fill, row)
	_draw_requires(font)


## Enchanting's category column (measured against the game with Item chosen: Disenchant 101 stage px above it, then
## 60.5 and 40.5 px): the chosen row has 20 px of room either side, and the rule under Disenchant opens 40 px.
func _enchant_gap(i: int, chosen: int) -> float:
	var rule := 40.0 if i > 0 else 0.0
	if chosen > 0:
		rule -= 40.0
	return rule + (20.0 if i > chosen else -20.0 if i < chosen else 0.0)


## The little arrow the game puts before a marked row (an outlined triangle pointing right).
func _draw_mark(fill: Transform2D, at: Vector2) -> void:
	var points := PackedVector2Array([fill * at + Vector2(0.0, -6.0), fill * at + Vector2(8.0, 0.0), fill * at + Vector2(0.0, 6.0), fill * at + Vector2(0.0, -6.0)])
	draw_polyline(points, Color.WHITE, 1.5)


## The card at the bottom centre: the name, the stats of an item (weapon, armor), or the effects of an ingredient or
## enchantment, or what the marked parts would make.
func _draw_card(font: Font, th: Theme, fill: Transform2D, row: Dictionary) -> void:
	if row.is_empty() and _preview.is_empty():
		return
	var frame := th.get_stylebox(&"panel", TYPE) if th.has_stylebox(&"panel", TYPE) else null
	var card := Rect2(fill * Vector2(640.0, 415.0), Vector2(419.0, 143.0) * Vector2(fill.x.x, fill.y.y))
	if frame != null:
		draw_style_box(frame, card)
	var middle := 850.0
	var made: bool = (_mode == Mode.ALCHEMY and bool(_preview.get("mixable", false))) or (_mode == Mode.ENCHANT and not _preview.is_empty() and bool(_preview.get("ok", false)))
	if made:
		_stage_centred(font, String(_preview["name"]).to_upper(), Vector2(middle, 465.0), 26.0, Color.WHITE)
		var y := 492.0
		var lines: Array = []
		if _mode == Mode.ALCHEMY:
			for effect: Dictionary in _preview["effects"]:
				lines.append(String(effect["description"]))
		else:
			lines.append_array(_preview["effects"])
			lines.append("Worn: constant effect" if _preview["constant"] else "Charge %d, %d uses" % [int(_preview["charge"]), int(_preview["uses"])])
		for text: String in lines.slice(0, 3):
			_stage_centred(font, text, Vector2(middle, y), 18.0, _normal)
			y += 22.0
		return
	if row.is_empty():
		return
	var title := _row_text(row) if _mode == Mode.ALCHEMY else String(row["name"])
	_stage_centred(font, title.to_upper(), Vector2(middle, 477.0), 30.0, Color.WHITE)
	var form := int(row["product"]) if row.has("product") else int(row.get("form", 0))
	var info := _session.item_info(form) if form != 0 else {}
	var kind := String(info.get("category", ""))
	if kind == "armor":
		_stage_text(font, ["Light Armor", "Heavy Armor", "Clothing"][clampi(int(info.get("armor_type", 0)), 0, 2)], Vector2(667.0, 436.0), 14.0, _dim)
	if bool(info.get("found", false)) and not (_mode == Mode.ENCHANT and _category == 2):
		var stats: Array = []
		if kind == "armor":
			stats.append(["ARMOR", "%d" % _shown_armor(info)])
		elif kind == "weapon":
			stats.append(["DAMAGE", "%d" % int(info.get("damage", 0))])
		stats.append(["WEIGHT", ("%.1f" % float(info.get("weight", 0))).trim_suffix(".0")])
		stats.append(["VALUE", "%d" % int(info.get("value", 0))])
		var x := 679.0 if stats.size() == 3 else 725.0
		for pair: Array in stats:
			_stage_text(font, pair[0], Vector2(x, 509.0), 14.0, _dim)
			var label_w := font.get_string_size(pair[0], HORIZONTAL_ALIGNMENT_LEFT, -1, int(roundf(14.0 * fill.x.x))).x / fill.x.x
			_stage_text(font, pair[1], Vector2(x + label_w + 6.0, 512.0), 30.0, Color.WHITE)
			x += 125.0
	# Below the stats: an ingredient's four effect slots, an enchanted item's enchantment, an enchantment's effects,
	# a soul gem's soul.
	var lines: Array[String] = []
	if _mode == Mode.ALCHEMY:
		var slots: Array = row.get("effects", [])
		for i in 4:
			var known: bool = i < slots.size() and bool(slots[i]["known"])
			var text := String(slots[i]["name"]) if known else "UNKNOWN"
			_stage_centred(font, text, Vector2(745.0 if i % 2 == 0 else 955.0, 545.0 + (i / 2) * 20.0), 14.0, _normal if known else _dim)
		return
	if _mode == Mode.ENCHANT:
		match _category:
			0:
				lines.append(String(row.get("enchantment", "")))
			2:
				for effect: String in row.get("effects", []):
					lines.append(effect)
			3:
				lines.append("Filled with %s" % row.get("soul_name", ""))
	var y := 538.0 if _category != 2 or _mode != Mode.ENCHANT else 505.0
	for text: String in lines.slice(0, 3):
		_stage_centred(font, text, Vector2(middle, y), 16.0, _normal)
		y += 22.0


## "Requires: ..." under the card: what the recipe needs, or the slots of the mode with the marked parts in white.
func _draw_requires(font: Font) -> void:
	var segments: Array = []  # [text, bright]
	match _mode:
		Mode.RECIPES, Mode.TEMPER:
			var row := current()
			var parts: Array[String] = []
			for c: Dictionary in row.get("components", []):
				parts.append(("%d %s" % [int(c["need"]), c["name"]]) if int(c["need"]) > 1 else String(c["name"]))
			if parts.is_empty():
				return
			segments = [[", ".join(parts), false]]
		Mode.ALCHEMY:
			var names: Array[String] = []
			for form in _marked:
				for row: Dictionary in _rows:
					if int(row["form"]) == form:
						names.append(String(row["name"]))
			for i in 3:
				segments.append([names[i] if i < names.size() else ("Ingredient" if i < 2 else "Optional"), i < names.size()])
		Mode.ENCHANT:
			for i in 3:
				var labels := ["Item", "Enchantment", "Soul Gem"]
				segments.append([String(_marks[i + 1]["name"]) if _marks.has(i + 1) else labels[i], _marks.has(i + 1)])
	var px := int(roundf(22.0 * UiDraw.stage_fill(size, STAGE).x.x))
	var fill_x := UiDraw.stage_fill(size, STAGE).x.x
	var texts: Array[String] = ["Requires: "]
	for i in segments.size():
		texts.append(String(segments[i][0]) + (", " if i < segments.size() - 1 and segments.size() > 1 else ""))
	var total := 0.0
	for text in texts:
		total += font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, px).x / fill_x
	var x := 850.0 - total * 0.5
	_stage_text(font, texts[0], Vector2(x, 630.0), 22.0, Color.WHITE)
	x += font.get_string_size(texts[0], HORIZONTAL_ALIGNMENT_LEFT, -1, px).x / fill_x
	for i in segments.size():
		_stage_text(font, texts[i + 1], Vector2(x, 630.0), 22.0, Color.WHITE if segments[i][1] else _dim)
		x += font.get_string_size(texts[i + 1], HORIZONTAL_ALIGNMENT_LEFT, -1, px).x / fill_x


## The armor rating the item card shows: the base rating scaled by the player's skill in its weight class, rounded up
## (lerp(fArmorRatingBase, fArmorRatingPCMax, skill / 100), as in AttackRules.armor_percent); clothing as is.
## Tempering is not applied (the card is of an item not yet made).
func _shown_armor(info: Dictionary) -> int:
	var base := float(info.get("armor_rating", 0.0))
	var type := int(info.get("armor_type", 0))
	if type > 1:
		return roundi(base)
	var skill := float(_session.actor_value(0x14, "HeavyArmor" if type == 1 else "LightArmor"))
	var factor := lerpf(AttackRules.SETTINGS["fArmorRatingBase"], AttackRules.SETTINGS["fArmorRatingPCMax"], skill / 100.0)
	return int(ceilf(base * factor))


func _stage_centred(font: Font, text: String, at: Vector2, size_stage: float, colour: Color) -> void:
	var fill := UiDraw.stage_fill(size, STAGE)
	var px := maxi(int(roundf(size_stage * fill.x.x)), 6)
	var wide := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, px).x
	var origin := fill * at - Vector2(wide * 0.5, 0.0)
	draw_string_outline(font, origin, text, HORIZONTAL_ALIGNMENT_LEFT, -1, px, 3, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, origin, text, HORIZONTAL_ALIGNMENT_LEFT, -1, px, colour)


func _draw_bottom_bar(font: Font) -> void:
	var fill := UiDraw.stage_fill(size, STAGE)
	var buttons: Array = []  # [icon export, label]
	var can_select := not _rows.is_empty() and (_in_items or _mode != Mode.ENCHANT)
	var row := current()
	match _mode:
		Mode.RECIPES:
			buttons = [["E", "Create"], ["Tab", "Exit"]] if can_select else [["Tab", "Exit"]]
		Mode.TEMPER:
			buttons = [["E", "Select"], ["Tab", "Exit"]] if can_select else [["Tab", "Exit"]]
		Mode.ALCHEMY:
			if can_select:
				buttons.append(["E", "Remove" if not row.is_empty() and _marked.has(int(row["form"])) else "Add"])
			buttons.append(["Tab", "Exit"])
			if not _marked.is_empty():
				buttons.append(["F", "Clear Selections"])
			if _marked.size() >= 2:
				buttons.append(["R", "Craft"])
		Mode.ENCHANT:
			var learn := _in_items and _category == 0
			if _in_items:
				buttons.append(["E", "Learn" if learn else "Select"])
			buttons.append(["Tab", "Exit"])
			if not _marks.is_empty():
				buttons.append(["R", "Craft"])
	var x := 12.0
	var px := int(roundf(20.0 * fill.x.x))
	for button: Array in buttons:
		var icon := UiAssets.exported(SWF, String(button[0]))
		if icon >= 0:
			UiDraw.character(self, SWF, icon, fill * Transform2D(Vector2(1.1, 0.0), Vector2(0.0, 1.1), Vector2(x, 661.0)))
		_stage_text(font, String(button[1]), Vector2(x + 32.0, 684.0), 20.0, Color.WHITE)
		x += 32.0 + font.get_string_size(String(button[1]), HORIZONTAL_ALIGNMENT_LEFT, -1, px).x / fill.x.x + 16.0
	_draw_skill(font, fill)


func _draw_skill(font: Font, fill: Transform2D) -> void:
	var skill: String = SKILLS.get(_mode, "Smithing")
	var level := int(_session.actor_value(0x14, skill))
	_stage_text(font, skill, Vector2(770.0, 680.0), 16.0, _dim)
	_stage_text(font, str(level), Vector2(821.0, 684.0), 20.0, Color.WHITE)
	_stage_text(font, str(mini(level + 1, 100)), Vector2(1005.0, 684.0), 20.0, Color.WHITE)
	_draw_skill_meter(fill, _skill_fraction(skill))


## The skill meter of the bottom bar's "Crafting" frame (bottombar.swf, PlayerInfoCard at (503, 16) in the bar at
## y 646.55): the frame art (the unnamed shape) and LevelMeterInstance at the fraction, as BottomBar draws its meters.
func _draw_skill_meter(fill: Transform2D, fraction: float) -> void:
	var swf := BottomBar.SWF
	var frame := UiAssets.label_frame(swf, BottomBar.CARD_SPRITE, "Crafting")
	if frame < 0:
		return
	var card := fill * Transform2D(0.0, Vector2(503.0 + BottomBar.GROUP_SHIFT, 646.55 + 16.0))
	for entry: Dictionary in UiAssets.display_list(swf, BottomBar.CARD_SPRITE, frame):
		var id := int(entry["character"])
		if entry.get("name", "") == "" and UiAssets.character(swf, id).get("type", "") == "shape":
			UiDraw.character(self, swf, id, card * UiDraw.matrix(entry["matrix"]))
		elif entry.get("name", "") == "LevelMeterInstance":
			var look := UiDraw.Look.new()
			look.fills["Meter_mc"] = fraction
			var xform := card * UiDraw.matrix(entry["matrix"])
			for bar: Dictionary in UiAssets.character(swf, id).get("display", []):
				var m: Array = bar["matrix"]
				var tall := Transform2D(Vector2(m[0], 0.0), Vector2(0.0, BottomBar.METER_HEIGHT), Vector2(m[4], BottomBar.METER_ORIGIN_Y))
				UiDraw.character(self, swf, int(bar["character"]), xform * tall, Color.WHITE, look, Rect2(), 0, "Meter_mc")


## How far the skill is to its next level (the xp over Progression.skill_threshold).
func _skill_fraction(skill: String) -> float:
	var av: int = {"Smithing": Progression.SMITHING, "Alchemy": Progression.ALCHEMY, "Enchanting": Progression.ENCHANTING}[skill]
	var level := int(_session.actor_value(0x14, skill))
	var need := _session.progression.skill_threshold(av, level)
	if need <= 0.0:
		return 0.0
	var xp := float(_session.progression.data().progress()["skill_xp"][av - ProgressionData.FIRST_SKILL])
	return clampf(xp / need, 0.0, 1.0)


func _update_model_rect() -> void:
	if _model_view != null:
		_model_view.region = Rect2(0.48, 0.08, 0.3, 0.64)  # the game's forge centres the item at about x 0.63, y 0.4


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
