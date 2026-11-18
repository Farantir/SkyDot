# SPDX-License-Identifier: GPL-3.0-or-later
#
# The flat viewer's host of the inventory, container and crafting menus
# (ui/inventory_view.gd, ui/container_view.gd, ui/crafting_view.gd, ui/magic_view.gd): a CanvasLayer over the window.
# While either is open the player stands still and the other keys do nothing.
# The `inventory` action (I) opens the inventory, `tween_menu` (Tab, the gamepad's B) the Tween menu, whose
# directions open the skills, magic and item menus; a menu's cancel (Tab, Esc) closes it. A container opens when GameSession says
# `container_opened`, a workbench's menu on `crafting_opened`; the `magic` action (P, gamepad R3) opens the magic menu.
class_name ItemMenu
extends CanvasLayer

var inventory_view: InventoryView
var container_view: ContainerView
var crafting_view: CraftingView
var magic_view: MagicView
var favorites_view: FavoritesView
var training_view: TrainingView
var skills_view: SkillsView
var tween_view: TweenView
var journal_view: JournalView
var sleep_wait_view: SleepWaitView
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
	favorites_view = FavoritesView.new(session)
	training_view = TrainingView.new(session)
	skills_view = SkillsView.new(session)
	tween_view = TweenView.new(session)
	tween_view.chosen.connect(_on_tween_chosen)
	journal_view = JournalView.new(session)
	sleep_wait_view = SleepWaitView.new(session)
	for view: Control in [inventory_view, container_view, crafting_view, magic_view, favorites_view, training_view, skills_view, tween_view, journal_view, sleep_wait_view]:
		view.enabled = func() -> bool: return input.enabled
		view.key_label = input.key_label
		view.engaged.connect(func(_open: bool) -> void: _on_engaged())
		add_child(view)
	session.container_opened.connect(open_container)
	session.crafting_opened.connect(open_crafting)
	session.training_opened.connect(training_view.open)
	session.sleep_requested.connect(open_sleep)


func is_open() -> bool:
	return inventory_view.is_open() or container_view.is_open() or crafting_view.is_open() or magic_view.is_open() or favorites_view.is_open() or training_view.is_open() or skills_view.is_open() or tween_view.is_open() or journal_view.is_open() or sleep_wait_view.is_open()


## The open menu for a shot's "menu" block: {open, tab, selected, mode}; empty when none is open.
func describe() -> Dictionary:
	var names := {"inventory": inventory_view, "container": container_view, "crafting": crafting_view,
		"magic": magic_view, "favorites": favorites_view, "training": training_view, "skills": skills_view,
		"tween": tween_view, "journal": journal_view, "sleep_wait": sleep_wait_view}
	for name: String in names:
		var view: Control = names[name]
		if not view.is_open():
			continue
		var out := {"open": name}
		var model: Variant = view.get(&"model")
		if model is ItemListModel:
			out["tab"] = model.tab
			out["selected"] = model.selected
		if view == tween_view:
			out["selected"] = TweenView.LABELS[tween_view.selection]
		if view == journal_view:
			out["tab"] = journal_view.tab
			out["selected"] = journal_view.title_index if journal_view.tab == JournalView.QUESTS else journal_view.category
			if journal_view.tab == JournalView.SYSTEM:
				out["mode"] = JournalSystem.State.keys()[journal_view.system.state]
		if view == sleep_wait_view:
			out["mode"] = "sleep" if sleep_wait_view.sleeping else "wait"
			out["selected"] = sleep_wait_view.hours()
		var mode: Variant = view.get(&"_mode")
		if mode != null and view == crafting_view:
			out["mode"] = CraftingView.Mode.keys()[mode]
		return out
	return {}


## A menu opened or closed: the player stands still while one is open, and the mouse is free to point at it.
func _on_engaged() -> void:
	var open := is_open()
	_input.blocked = open
	# The world behind an open item menu, as the game shows it: the image space modifier of DOBJ IMID.
	var imid := _session.world.get_default_object("IMID") if _session.world != null else 0
	# Not behind the skills screen: StatsMenu asks for its own modifier (SkillMenuIdle), which Skyrim.esm lacks, so the
	# world shows sharp through the sky.
	var item_menu := open and not skills_view.is_open()
	ImageSpaceSync.modifier = _session.world.get_image_space_modifier(imid) if item_menu and imid != 0 else {}
	if _input.enabled and DisplayServer.get_name() != "headless":
		if open:
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
		else:
			_input.capture_mouse()


## The Q key outside a menu: the quick favorites menu (not during a dialogue).
func open_favorites() -> void:
	if is_open() or _session.is_dialogue_open():
		return
	favorites_view.open()


## The `tween_menu` key (Tab) outside a menu: the Tween menu (not during a dialogue).
func open_tween() -> void:
	if is_open() or _session.is_dialogue_open():
		return
	tween_view.open()


## The Journal key (J) outside a menu opens the journal on Quests, Pause (Esc) on the tab it was closed on; not during
## a dialogue. False when the pack has no journal movie (the shell frees the mouse instead, as before).
func open_journal(pause: bool) -> bool:
	if not JournalView.available():
		return false
	if is_open() or _session.is_dialogue_open():
		return true
	journal_view.open(-1 if pause else JournalView.QUESTS)
	return true


## The Wait key (T) outside a menu: the Sleep/Wait menu to wait (not during a dialogue). The game refuses with a
## message when the player may not wait (Rest.refusal).
func open_wait() -> void:
	if is_open() or _session.is_dialogue_open() or not SleepWaitView.available():
		return
	sleep_wait_view.open(false)


## A bed the player may sleep in was activated (GameSession.sleep_requested): the menu to sleep.
func open_sleep(bed: int) -> void:
	if is_open() or not SleepWaitView.available():
		return
	sleep_wait_view.open(true, bed)


## A direction of the Tween menu was opened (the Tween menu has closed).
func _on_tween_chosen(direction: String) -> void:
	match direction:
		"inventory":
			inventory_view.open()
		"magic":
			magic_view.open()
		"skills":
			skills_view.open()


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
	favorites_view.close()
	container_view.open(ref)


func open_crafting(ref: int, bench_type: int) -> void:
	if _session.is_dialogue_open():
		return
	inventory_view.close()
	container_view.close()
	magic_view.close()
	crafting_view.open(ref, bench_type)


## The `skills` key outside a menu: show the skills screen (not during a dialogue); inside it, close it.
func toggle_skills() -> void:
	if skills_view.is_open():
		skills_view.close()
	elif not is_open() and not _session.is_dialogue_open():
		skills_view.open()
