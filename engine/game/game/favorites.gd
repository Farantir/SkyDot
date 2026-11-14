# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the player has marked as a favorite, and the quick hotkeys 1-8: a per-form flag (an item, a spell,
# a shout) that the menus draw, the favorites popup lists and scripts read (Game.IsObjectFavorited, kept
# in SkydotPapyrus by GameSession). Saved as the FAVS section; a save without it has none.
class_name Favorites
extends RefCounted

signal changed

const SLOTS := 8

var _forms := {}  # form id -> true
var _hotkeys := {}  # slot (1..SLOTS) -> form id


func has(form: int) -> bool:
	return _forms.has(form)


## The forms marked, in no particular order.
func forms() -> Array:
	return _forms.keys()


## Mark or unmark; returns the new state. Unmarking also frees the form's hotkey.
func set_favorite(form: int, on: bool) -> bool:
	if on == _forms.has(form):
		return on
	if on:
		_forms[form] = true
	else:
		_forms.erase(form)
		for slot: int in _hotkeys.keys():
			if _hotkeys[slot] == form:
				_hotkeys.erase(slot)
	changed.emit()
	return on


func toggle(form: int) -> bool:
	return set_favorite(form, not _forms.has(form))


## The hotkey (1..SLOTS) of `form`, 0 if it has none.
func hotkey_of(form: int) -> int:
	for slot: int in _hotkeys:
		if _hotkeys[slot] == form:
			return slot
	return 0


## The form on hotkey `slot`, 0 if it is free.
func form_at(slot: int) -> int:
	return _hotkeys.get(slot, 0)


## Put `form` on hotkey `slot` (1..SLOTS), taking it from whatever held the slot and from its old slot; the
## form becomes a favorite. Slot 0 only frees the form's hotkey.
func assign(slot: int, form: int) -> void:
	for old: int in _hotkeys.keys():
		if _hotkeys[old] == form:
			_hotkeys.erase(old)
	if slot >= 1 and slot <= SLOTS:
		_forms[form] = true
		_hotkeys[slot] = form
	changed.emit()


func clear() -> void:
	if _forms.is_empty() and _hotkeys.is_empty():
		return
	_forms.clear()
	_hotkeys.clear()
	changed.emit()


func save_bytes() -> PackedByteArray:
	return var_to_bytes({"forms": _forms.keys(), "hotkeys": _hotkeys.duplicate()})


## The state `bytes` hold ({forms: Array, hotkeys: Dictionary}), or an empty Dictionary if they are not a
## favorites section (checks only; changes nothing).
static func decode(bytes: PackedByteArray) -> Dictionary:
	if bytes.size() < 4:  # not even a Variant header
		return {}
	var state = bytes_to_var(bytes)
	if typeof(state) != TYPE_DICTIONARY or typeof(state.get("forms")) != TYPE_ARRAY or typeof(state.get("hotkeys")) != TYPE_DICTIONARY:
		return {}
	for form in state["forms"]:
		if typeof(form) != TYPE_INT:
			return {}
	for slot in state["hotkeys"]:
		if typeof(slot) != TYPE_INT or typeof(state["hotkeys"][slot]) != TYPE_INT:
			return {}
	return state


## Replace everything with a decoded state ({} makes it empty).
func adopt(state: Dictionary) -> void:
	_forms.clear()
	_hotkeys.clear()
	for form: int in state.get("forms", []):
		_forms[form] = true
	for slot: int in state.get("hotkeys", {}):
		var form: int = state["hotkeys"][slot]
		if slot >= 1 and slot <= SLOTS and _forms.has(form):
			_hotkeys[slot] = form
	changed.emit()
