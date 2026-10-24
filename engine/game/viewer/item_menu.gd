# SPDX-License-Identifier: GPL-3.0-or-later
#
# The flat viewer's host of the inventory, container and crafting menus
# (ui/inventory_view.gd, ui/container_view.gd, ui/crafting_view.gd, ui/magic_view.gd): a CanvasLayer over the window.
# While either is open the player stands still and the other keys do nothing.
# The `inventory` action (I, Tab, the gamepad's Back) opens the inventory or
# closes whichever menu is open; a container opens when GameSession says
# `container_opened`, a workbench's menu on `crafting_opened`; the `magic` action (P, gamepad R3) opens the magic menu.
class_name ItemMenu
extends CanvasLayer

var inventory_view: InventoryView
var container_view: ContainerView
var crafting_view: CraftingView
var magic_view: MagicView
var _session: GameSession
var _input: PlayerInput


func _init(session: GameSession, input: PlayerInput) -> void:
	layer = 12
	_session = session
	_input = input
	inventory_view = InventoryView.new(session)
	container_view = ContainerView.new(session)
	crafting_view = CraftingView.new(session)
	magic_view = MagicView.new(session)
	for view: Control in [inventory_view, container_view, crafting_view, magic_view]:
		view.enabled = func() -> bool: return input.enabled
		view.key_label = input.key_label
		view.engaged.connect(func(_open: bool) -> void: input.blocked = is_open())
		add_child(view)
	session.container_opened.connect(open_container)
	session.crafting_opened.connect(open_crafting)


func is_open() -> bool:
	return inventory_view.is_open() or container_view.is_open() or crafting_view.is_open() or magic_view.is_open()


## The `inventory` key outside a menu: show the inventory (not during a dialogue).
func open_inventory() -> void:
	if is_open() or _session.is_dialogue_open():
		return
	inventory_view.open()


## The `magic` key outside a menu: show the magic menu (not during a dialogue).
func open_magic() -> void:
	if is_open() or _session.is_dialogue_open():
		return
	magic_view.open()


func open_container(ref: int) -> void:
	if _session.is_dialogue_open():
		return
	inventory_view.close()
	magic_view.close()
	container_view.open(ref)


func open_crafting(ref: int, bench_type: int) -> void:
	if _session.is_dialogue_open():
		return
	inventory_view.close()
	container_view.close()
	magic_view.close()
	crafting_view.open(ref, bench_type)
