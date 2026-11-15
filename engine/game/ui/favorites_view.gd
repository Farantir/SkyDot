# SPDX-License-Identifier: GPL-3.0-or-later
#
# The quick favorites menu (Q): every favorite item, spell, scroll and shout in one item list
# (ItemListView). Up and down choose, accept uses the entry as the game does (equips or takes off an item,
# uses a potion, puts a spell in the right hand, a shout in the voice slot) and closes the menu,
# `menu_equip_left` (the right mouse button, the left trigger) puts a spell or scroll in the left hand, the keys 1-8 put the entry on
# that hotkey (outside the menu the same keys use it), `menu_favorite` (F) removes the entry from the
# favorites, cancel or Tab closes. A Control that fills its host (see InventoryView for the contract).
class_name FavoritesView
extends Control

signal engaged(open: bool)

var model: ItemListModel
var _session: GameSession
var _open := false
var _list: ItemListView
var enabled: Callable = func() -> bool: return true
var key_label: Callable = func(_action: StringName) -> String: return ""


func _init(session: GameSession) -> void:
	_session = session
	model = ItemListModel.new("favorites", session.favorites)
	_list = ItemListView.new(model)
	_list.title = "Favorites"
	_list.region = Rect2(0.3, 0.1, 0.4, 0.74)
	_list.status = ["Keys 1-8 set a hotkey"]
	_list.visible = false
	_list.key_label = func(action: StringName) -> String: return key_label.call(action)
	_list.action_requested.connect(_on_action)
	add_child(_list)
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	session.items_changed.connect(_refresh)
	session.favorites.changed.connect(_refresh)


func is_open() -> bool:
	return _open


func open() -> void:
	if _open:
		return
	_open = true
	_list.visible = true
	engaged.emit(true)
	_refresh()


func close() -> void:
	if not _open:
		return
	_open = false
	_list.visible = false
	_list.stop_search()
	engaged.emit(false)


func current() -> Dictionary:
	return model.current()


func _refresh() -> void:
	if _open:
		model.set_rows(ItemListSources.favorites(_session))


func _input(event: InputEvent) -> void:
	if not _open or not enabled.call():
		return
	var handled := true
	if _list.is_searching():
		_list.handle_input(event)
	elif event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"favorites"):
		close()  # the control map's Favorites menu Cancel: the Favorites key, Tab or Esc
	elif _hotkey_pressed(event):
		pass
	elif not _list.handle_input(event):
		handled = event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	if handled:
		get_viewport().set_input_as_handled()


func _hotkey_pressed(event: InputEvent) -> bool:
	for slot in Favorites.SLOTS:
		if event.is_action_pressed(StringName("hotkey_%d" % (slot + 1))):
			assign_hotkey(slot + 1)
			return true
	return false


## Put the entry under the cursor on hotkey `slot` (1-8; pressing the key it already has frees it).
func assign_hotkey(slot: int) -> void:
	var row := current()
	if row.is_empty():
		return
	var form := int(row["form"])
	_session.favorites.assign(0 if _session.favorites.hotkey_of(form) == slot else slot, form)
	_session.favorites.set_favorite(form, true)


func _on_action(action: StringName, row: Dictionary) -> void:
	match action:
		&"use_favorite":
			_session.use_favorite(int(row["form"]))
			close()
		&"equip_left_favorite":
			_session.use_favorite(int(row["form"]), Magic.LEFT)
			close()
		&"unfavorite":
			_session.favorites.set_favorite(int(row["form"]), false)
