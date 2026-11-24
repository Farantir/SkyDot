# SPDX-License-Identifier: GPL-3.0-or-later
#
# What every game menu (the item, magic, container, crafting, favorites, training, skills, Tween, journal,
# Sleep/Wait and map views) shares with its host (MenuHost): a Control that fills the host, says when it opens
# and closes (`engaged`), listens to input only while open and enabled, and draws on the game's 1280x720 stage.
# A view sets `_open` and emits `engaged(true)` when it opens; `close` ends it, with `_closed` for the view's own
# part. Input reaches `_menu_input` only while the menu is open and the host's input counts.
class_name GameMenu
extends Control

## True when the menu opens, false when it closes.
signal engaged(open: bool)

## The stage the menus' movies are drawn on.
const STAGE := Vector2(1280.0, 720.0)

## Whether the menu listens at all (false in runs that drive themselves).
var enabled: Callable = func() -> bool: return true
## The key or button shown for an action (the host's input device).
var key_label: Callable = func(_action: StringName) -> String: return ""

var _open := false


func is_open() -> bool:
	return _open


## Whether the world waits while this menu is open (the game pauses behind its menus).
func pauses() -> bool:
	return true


## Close the menu. `force` closes it also where the view would keep it open (a death, a load).
func close(force := false) -> void:
	if not _open or not _may_close(force):
		return
	_open = false
	_closed()
	engaged.emit(false)


## The stage drawn to fill the view: stage units to view pixels.
func fill() -> Transform2D:
	return UiDraw.stage_fill(size, STAGE)


## False keeps the menu open on a close that is not forced.
func _may_close(_force: bool) -> bool:
	return true


## The view's own part of closing (`_open` is already false; `engaged` follows).
func _closed() -> void:
	pass


func _input(event: InputEvent) -> void:
	if not _open or not enabled.call():
		return
	_menu_input(event)


## An input event while the menu is open and enabled.
func _menu_input(_event: InputEvent) -> void:
	pass


## End an event's handling: the menu keeps it when it used it, and also keeps keys, buttons and clicks it did
## not use from the game behind it (mouse motion passes). True when it was kept.
func _consume(event: InputEvent, used := false) -> bool:
	if used or event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton:
		get_viewport().set_input_as_handled()
		return true
	return false
