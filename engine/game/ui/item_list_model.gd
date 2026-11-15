# SPDX-License-Identifier: GPL-3.0-or-later
#
# The view model of the shared item list (ADR 0010): the rows a menu offers (from ItemListSources, or any
# array of row dictionaries), the tab being shown, the sort of each tab, the search text, the row under the
# cursor and the card of that row. It knows no game state and no node; ItemListView draws it and the menus
# (inventory, magic, container, favorites) feed it and act on what it reports. Tabs, columns and actions
# are data in ItemListRegistry.
#
# A row is a Dictionary with at least `form`, `name` and `tab` (a tab id) and usually `kind` ("item", "spell",
# "power", "shout", "scroll", "effect"), `extra`, `label` (the name with its count; defaults to the name) and
# the fields the columns show. The model adds `favorite` and `hotkey` from `favorites`.
class_name ItemListModel
extends RefCounted

signal changed

var menu: String
var favorites: Favorites
var search := "":
	set(value):
		search = value
		_dirty = true
		selected = 0
		changed.emit()
var tab := 0:
	set(value):
		tab = clampi(value, 0, maxi(tab_ids().size() - 1, 0))
		selected = 0
		_dirty = true
		changed.emit()
var selected := 0

var _all: Array = []
var _shown: Array = []
var _dirty := true
var _sort := {}  # tab id -> {column: id, ascending: bool}


func _init(menu_id: String, favs: Favorites = null) -> void:
	menu = menu_id
	favorites = favs if favs != null else Favorites.new()
	favorites.changed.connect(func() -> void:
		_dirty = true
		changed.emit())


## Replace the rows. The cursor stays on the same row (form and extra) if it is still listed, else on its index.
func set_rows(rows: Array) -> void:
	var before := current()
	var shown_tab := tab_id()
	var cursor := selected
	_all = rows
	_dirty = true
	var ids := tab_ids()
	tab = ids.find(shown_tab) if ids.has(shown_tab) else mini(tab, maxi(ids.size() - 1, 0))
	selected = cursor
	_dirty = true
	_refresh()
	if not before.is_empty():
		for i in _shown.size():
			if _same(_shown[i], before):
				selected = i
				break
	selected = clampi(selected, 0, maxi(_shown.size() - 1, 0))
	changed.emit()


# ---- tabs

## Ids of the tabs shown: those of the menu, minus the ones that hide while empty.
func tab_ids() -> Array[String]:
	var out: Array[String] = []
	for def: Dictionary in ItemListRegistry.tabs_of(menu):
		if def.get("hidden_empty", false) and _rows_of(def).is_empty():
			continue
		out.append(def["id"])
	return out


func tab_names() -> Array[String]:
	var out: Array[String] = []
	for id in tab_ids():
		out.append(_def(id)["name"])
	return out


func tab_id() -> String:
	var ids := tab_ids()
	return ids[tab] if tab < ids.size() else ""


func tab_name() -> String:
	return _def(tab_id()).get("name", "")


## Show the tab with this id or name; false if there is none.
func select_tab(id_or_name: String) -> bool:
	var ids := tab_ids()
	for i in ids.size():
		if ids[i] == id_or_name or _def(ids[i])["name"] == id_or_name:
			tab = i
			return true
	return false


## The next (+1) or previous (-1) tab, wrapping.
func step_tab(step: int) -> void:
	var count := tab_ids().size()
	if count > 0:
		tab = (tab + step + count) % count


# ---- rows

## The rows the current tab lists, sorted and filtered by the search text.
func rows() -> Array:
	_refresh()
	return _shown


## The rows of tab `id_or_name` whatever the search says, in that tab's sort order.
func rows_of_tab(id_or_name: String) -> Array:
	for def: Dictionary in ItemListRegistry.tabs_of(menu):
		if def["id"] == id_or_name or def["name"] == id_or_name:
			var rows := _rows_of(def)
			_sort_rows(rows, def["id"])
			return rows
	return []


## The row under the cursor, {} if the list is empty.
func current() -> Dictionary:
	_refresh()
	return _shown[selected] if selected < _shown.size() else {}


func move(step: int) -> void:
	_refresh()
	if not _shown.is_empty():
		selected = (selected + step + _shown.size()) % _shown.size()
		changed.emit()


func select(index: int) -> void:
	_refresh()
	selected = clampi(index, 0, maxi(_shown.size() - 1, 0))
	changed.emit()


## Every row of the menu, flagged by the favorites (for totals the header shows).
func all_rows() -> Array:
	_refresh()
	return _all


# ---- columns and sorting

func columns() -> Array[Dictionary]:
	return ItemListRegistry.columns_of(menu, tab_id())


## The sort of the current tab: {column, ascending}.
func sort_state() -> Dictionary:
	return _sort_of(tab_id())


## Sort the current tab by column `id`: the first click orders it (numbers high to low), another click on the
## same column reverses it. Columns marked not sortable are ignored.
func sort_by(column_id: String) -> void:
	var column: Dictionary = ItemListRegistry.columns.get(column_id, {})
	if column.is_empty() or not column.get("sortable", true):
		return
	var state := _sort_of(tab_id())
	if state["column"] == column_id:
		_sort[tab_id()] = {"column": column_id, "ascending": not state["ascending"]}
	else:
		_sort[tab_id()] = {"column": column_id, "ascending": not column.get("numeric", false)}
	_dirty = true
	selected = 0
	changed.emit()


# ---- card

## The card of the row under the cursor: [{label, text}], only the fields it has a value for.
func card() -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	var row := current()
	if row.is_empty():
		return out
	for field: Dictionary in ItemListRegistry.card_fields.get(row.get("kind", "item"), []):
		var text := ItemListRegistry.field_text(field["field"], row)
		if text != "":
			out.append({"label": field["label"], "text": text})
	return out


# ---- internals

func _def(id: String) -> Dictionary:
	for def: Dictionary in ItemListRegistry.tabs_of(menu):
		if def["id"] == id:
			return def
	return {}


func _sort_of(id: String) -> Dictionary:
	return _sort.get(id, {"column": "name", "ascending": true})


func _same(a: Dictionary, b: Dictionary) -> bool:
	return a.get("form") == b.get("form") and a.get("extra", 0) == b.get("extra", 0) and a.get("kind") == b.get("kind")


## The rows tab `def` lists (without search), favorites flagged.
func _rows_of(def: Dictionary) -> Array:
	_flag()
	var out: Array = []
	for row: Dictionary in _all:
		if def.get("all", false) or (def["id"] == ItemListRegistry.FAVORITES and row["favorite"]) or row.get("tab", "") == def["id"]:
			out.append(row)
	return out


var _flagged := false


func _flag() -> void:
	if _flagged and not _dirty:
		return
	for row: Dictionary in _all:
		row["favorite"] = favorites.has(int(row["form"]))
		row["hotkey"] = favorites.hotkey_of(int(row["form"]))
	_flagged = true


func _refresh() -> void:
	if not _dirty:
		return
	_dirty = false
	_flagged = false
	_flag()
	var def := _def(tab_id())
	_shown = _rows_of(def) if not def.is_empty() else []
	if search != "":
		var needle := search.to_lower()
		_shown = _shown.filter(func(row: Dictionary) -> bool: return String(row["name"]).to_lower().contains(needle))
	_sort_rows(_shown, def.get("id", ""))
	selected = clampi(selected, 0, maxi(_shown.size() - 1, 0))


func _sort_rows(rows: Array, tab_id_: String) -> void:
	var state := _sort_of(tab_id_)
	var key: String = ItemListRegistry.columns.get(state["column"], {}).get("key", state["column"])
	var ascending: bool = state["ascending"]
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var x: Variant = _key(a, key)
		var y: Variant = _key(b, key)
		if x == y:
			return _name_before(a, b)
		return (x < y) if ascending else (x > y))


func _key(row: Dictionary, key: String) -> Variant:
	var value: Variant = row.get(key, 0)
	if key == "name":
		return String(row["name"]).to_lower()
	if typeof(value) == TYPE_BOOL:
		return 1 if value else 0
	if typeof(value) == TYPE_STRING or typeof(value) == TYPE_STRING_NAME:
		return String(value).to_lower()
	return value


func _name_before(a: Dictionary, b: Dictionary) -> bool:
	var x := String(a["name"]).to_lower()
	var y := String(b["name"]).to_lower()
	if x != y:
		return x < y
	return int(a["form"]) < int(b["form"]) or (a["form"] == b["form"] and int(a.get("extra", 0)) < int(b.get("extra", 0)))
