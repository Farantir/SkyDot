# SPDX-License-Identifier: GPL-3.0-or-later
#
# The inventory menu: the shared item list (ItemListView) with the player's items in the Favorites, Weapons,
# Apparel, ... Keys tabs. Up and down choose an item, left and right a tab (or the mouse), accept equips or
# unequips it (a potion or food is used up, a book read; neither does anything yet), `menu_drop` (R, gamepad X) drops the
# stack, `menu_favorite` (F, gamepad Y) marks it a favorite, `menu_charge` (T) recharges an
# enchanted weapon below its maximum from a filled soul gem (the smallest one that fills it, else the largest),
# `menu_search` types in the search box, cancel, Tab or I closes. Items that are not playable are hidden.
# This is the view's host, a Control that fills its host (the viewer's CanvasLayer, a VR shell's panel); it
# takes its input from the InputMap, so any shell can feed them, and it knows the game only through
# GameSession's item methods (via ItemListSources and the model). While it is open it emits `engaged(true)`
# so the shell keeps the player still.
class_name InventoryView
extends Control

signal engaged(open: bool)

var model: ItemListModel
var _session: GameSession
var _open := false
var _list: ItemListView
var _preview: ItemPreview
var _bar: BottomBar
## Whether the menu listens at all (false in runs that drive themselves).
var enabled: Callable = func() -> bool: return true
## What the key labels say, by action (the viewer's gamepad or keyboard).
var key_label: Callable = func(_action: StringName) -> String: return ""


func _init(session: GameSession) -> void:
	_session = session
	model = ItemListModel.new("inventory", session.favorites)
	_list = ItemListView.new(model)
	_list.title = "Inventory"
	_list.key_label = func(action: StringName) -> String: return key_label.call(action)
	_list.region = Rect2(0.06, 0.06, 0.56, 0.82)
	_list.visible = false
	_list.action_requested.connect(_on_action)
	add_child(_list)
	_preview = ItemPreview.new(session)
	add_child(_preview)
	_bar = BottomBar.new("inventory")
	_bar.visible = false
	add_child(_bar)
	model.changed.connect(_update_preview)
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	session.items_changed.connect(_refresh)


func is_open() -> bool:
	return _open


func open() -> void:
	if _open:
		return
	_open = true
	_list.visible = true
	_bar.visible = true
	engaged.emit(true)
	_refresh()


func close() -> void:
	if not _open:
		return
	_open = false
	_list.visible = false
	_bar.visible = false
	_preview.clear()
	_list.stop_search()
	engaged.emit(false)


## The item under the cursor, {} if the list is empty.
func current() -> Dictionary:
	return model.current()


func _refresh() -> void:
	if not _open:
		return
	var raw := _session.inventory()
	model.set_rows(ItemListSources.from_items(raw, _session))
	var gold := 0
	for row: Dictionary in raw:
		if int(row["form"]) == 0xF:
			gold = int(row["count"])
	_list.status = []
	_bar.gold = gold
	_bar.carry_weight = ItemRows.total_weight(raw)
	_bar.carry_capacity = _session.actor_value(SkydotPapyrus.PLAYER_REF, "CarryWeight")
	_bar.health = Vector2(_session.actor_value(SkydotPapyrus.PLAYER_REF, "Health"), _session.actor_value_max(SkydotPapyrus.PLAYER_REF, "Health"))
	_bar.queue_redraw()


## The preview shows the item under the cursor.
func _update_preview() -> void:
	if not _open:
		return
	var row := model.current()
	_preview.show_item(int(row["form"]) if not row.is_empty() else -1)


## The 3D preview, for a VR host to put into the scene.
func preview() -> ItemPreview:
	return _preview


func _input(event: InputEvent) -> void:
	if not _open or not enabled.call():
		return
	var handled := true
	if _list.is_searching():
		_list.handle_input(event)
	elif _preview.handle_input(event):
		pass
	elif event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"inventory"):
		close()
	elif not _list.handle_input(event):
		handled = event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	if handled:
		get_viewport().set_input_as_handled()


func _on_action(action: StringName, _row: Dictionary) -> void:
	match action:
		&"use":
			use_current()
		&"drop":
			drop_current()
		&"recharge":
			recharge_current()
		&"favorite":
			favorite_current()


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


## Mark the item under the cursor a favorite, or unmark it.
func favorite_current() -> void:
	var row := current()
	if not row.is_empty():
		_session.toggle_favorite(int(row["form"]))


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
