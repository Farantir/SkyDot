# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game's menus (ui/: inventory, container, crafting, magic, favorites, training, skills, Tween, journal,
# Sleep/Wait, world map) and their rules, for both shells: which may open when, that one is open at a time,
# whether the world waits behind them (`pauses`), what the shell hides, and which keys and game signals open
# them. A shell only places the views: the viewer on a CanvasLayer, the VR shell on its panels (`place`).
class_name MenuHost
extends RefCounted

## A menu opened or closed: the shell shows or hides its panel.
signal engaged(view: GameMenu, open: bool)

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
var map_view: MapView
## Every view, in the order `describe` names them.
var views: Array[GameMenu] = []
## Whether a menu frees the mouse and closing it captures it again (a window with a mouse that looks).
var frees_mouse := true

var _session: GameSession
var _input: PlayerInput
var _names: Dictionary = {}  # view -> its name in `describe`


## `place` (Callable(view: GameMenu)) puts a view in the shell's scene; `key_label` (Callable(action) -> String)
## names keys, the input's own by default.
func _init(session: GameSession, input: PlayerInput, place: Callable, key_label := Callable()) -> void:
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
	journal_view = JournalView.new(session)
	sleep_wait_view = SleepWaitView.new(session)
	map_view = MapView.new(session)
	_names = {inventory_view: "inventory", container_view: "container", crafting_view: "crafting",
		magic_view: "magic", favorites_view: "favorites", training_view: "training", skills_view: "skills",
		tween_view: "tween", journal_view: "journal", sleep_wait_view: "sleep_wait", map_view: "map"}
	tween_view.chosen.connect(_on_tween_chosen)
	map_view.journal_requested.connect(func() -> void: journal_view.open(JournalView.QUESTS))
	journal_view.show_on_map.connect(func() -> void:
		journal_view.close()
		open_map())
	var label := key_label if key_label.is_valid() else input.key_label
	for view: GameMenu in _names:
		views.append(view)
		view.enabled = func() -> bool: return input.enabled
		view.key_label = label
		view.engaged.connect(_on_engaged.bind(view))
		place.call(view)
	session.container_opened.connect(open_container)
	session.crafting_opened.connect(open_crafting)
	session.training_opened.connect(training_view.open)
	session.sleep_requested.connect(open_sleep)


## The keys of the loop (ShellLoop) open the menus.
func attach(loop: ShellLoop) -> void:
	loop.inventory_requested.connect(open_inventory)
	loop.magic_requested.connect(open_magic)
	loop.favorites_requested.connect(open_favorites)
	loop.tween_requested.connect(open_tween)
	loop.journal_requested.connect(func(pause: bool) -> void:
		if not open_journal(pause) and frees_mouse and _input.enabled and DisplayServer.get_name() != "headless":
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE)  # no journal: Esc frees the mouse
	loop.wait_requested.connect(open_wait)
	loop.skills_requested.connect(toggle_skills)
	loop.map_requested.connect(open_map)


func is_open() -> bool:
	return open_view() != null


## The menu that is open, or null.
func open_view() -> GameMenu:
	for view in views:
		if view.is_open():
			return view
	return null


## Whether the world waits: a menu is open that pauses the game (all but training; the dialogue does not).
func pauses() -> bool:
	for view in views:
		if view.is_open() and view.pauses():
			return true
	return false


## The game hides its HUD while a menu is open.
func hides_hud() -> bool:
	return is_open()


## The arms and the weapon in hand stay below while the camera is up over the world map.
func hides_body() -> bool:
	return map_view.is_open()


## Close every menu; `force` also the ones that keep themselves open (the level-up choice): a load, a restart.
func close_all(force := false) -> void:
	for view in views:
		view.close(force)


## The open menu for a shot's "menu" block: {open, tab, selected, mode}; empty when none is open.
func describe() -> Dictionary:
	var view := open_view()
	if view == null:
		return {}
	var out := {"open": _names[view]}
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
	if view == map_view:
		out.merge(map_view.describe())
	if view == sleep_wait_view:
		out["mode"] = "sleep" if sleep_wait_view.sleeping else "wait"
		out["selected"] = sleep_wait_view.hours()
	if view == crafting_view:
		out["mode"] = CraftingView.Mode.keys()[crafting_view.get(&"_mode")]
	return out


## A menu the player opens himself: not over another menu, not during a dialogue.
func _may_open() -> bool:
	return not is_open() and not _session.is_dialogue_open()


## The Q key: the quick favorites menu.
func open_favorites() -> void:
	if _may_open():
		favorites_view.open()


## The `tween_menu` key (Tab): the Tween menu.
func open_tween() -> void:
	if _may_open():
		tween_view.open()


## The Journal key (J) opens the journal on Quests, Pause (Esc) on the tab it was closed on. False when the pack
## has no journal movie.
func open_journal(pause: bool) -> bool:
	if not JournalView.available():
		return false
	if _may_open():
		journal_view.open(-1 if pause else JournalView.QUESTS)
	return true


## The Wait key (T): the Sleep/Wait menu to wait. The game refuses with a message when the player may not wait
## (Rest.refusal).
func open_wait() -> void:
	if _may_open() and SleepWaitView.available():
		sleep_wait_view.open(false)


## The Quick Map key (M), the Tween menu's Map, the journal's Show on Map: the world map (nothing where the place
## has no world map).
func open_map() -> void:
	if _may_open() and MapView.available():
		map_view.open()


## A bed the player may sleep in was activated (GameSession.sleep_requested): the menu to sleep.
func open_sleep(bed: int) -> void:
	if not is_open() and SleepWaitView.available():
		sleep_wait_view.open(true, bed)


## The `inventory` key: the inventory.
func open_inventory() -> void:
	if _may_open():
		inventory_view.open()


## The `magic` key: the magic menu.
func open_magic() -> void:
	if _may_open():
		magic_view.open()


## The `skills` key: the skills screen; inside it, close it.
func toggle_skills() -> void:
	if skills_view.is_open():
		skills_view.close()
	elif _may_open():
		skills_view.open()


## A container was activated (GameSession.container_opened): its menu replaces the item menus.
func open_container(ref: int) -> void:
	if _session.is_dialogue_open():
		return
	inventory_view.close()
	magic_view.close()
	favorites_view.close()
	container_view.open(ref)


## A workbench was activated (GameSession.crafting_opened).
func open_crafting(ref: int, bench_type: int) -> void:
	if _session.is_dialogue_open():
		return
	inventory_view.close()
	container_view.close()
	magic_view.close()
	crafting_view.open(ref, bench_type)


## The menus the start options ask for (--open-inventory, --open-magic, ...; the map is the shell's). In a
## worldspace only the crafting category applies.
func open_start_menus(settings: ViewerSettings, in_world: bool) -> void:
	if settings.crafting_category != "":
		crafting_view.select_category(settings.crafting_category)
	if in_world:
		return
	if settings.open_inventory:
		open_inventory()
		if settings.inventory_tab != "":
			inventory_view.model.select_tab(settings.inventory_tab)
	if settings.open_favorites:
		open_favorites()
	if settings.open_magic:
		open_magic()
		magic_view.select_category(settings.magic_category)
	if settings.open_skills != "":
		if settings.grant_perks > 0:
			_session.papyrus.set_perk_points(settings.grant_perks)
		if settings.grant_xp > 0.0:
			_session.papyrus.set_player_xp(settings.grant_xp)
		skills_view.open()
		skills_view.select_skill(settings.open_skills)
	if settings.open_journal != "":
		journal_view.category = settings.journal_category
		journal_view.open(["quests", "stats", "system"].find(settings.open_journal))
		if settings.journal_system != "":
			journal_view.system.debug_state(settings.journal_system)
	if settings.open_sleep_wait != "":
		var parts := settings.open_sleep_wait.split(":")
		sleep_wait_view.open(parts[0] == "sleep")
		if parts.size() > 1:
			sleep_wait_view.set_value(float(parts[1]))
	if settings.open_training != 0:
		training_view.open(settings.open_training)


## A direction of the Tween menu was opened (the Tween menu has closed).
func _on_tween_chosen(direction: String) -> void:
	match direction:
		"inventory":
			inventory_view.open()
		"magic":
			magic_view.open()
		"skills":
			skills_view.open()
		"map":
			open_map()


## A menu opened or closed: the world behind an item menu blurs, the mouse is free to point at it.
func _on_engaged(open: bool, view: GameMenu) -> void:
	var any := is_open()
	_input.blocked = any
	# The world behind an open item menu, as the game shows it: the image space modifier of DOBJ IMID.
	var imid := _session.world.get_default_object("IMID") if _session.world != null else 0
	# Not behind the skills screen: StatsMenu asks for its own modifier (SkillMenuIdle), which Skyrim.esm lacks, so the
	# world shows sharp through the sky.
	var item_menu := any and not skills_view.is_open() and not map_view.is_open()
	ImageSpaceSync.modifier = _session.world.get_image_space_modifier(imid) if item_menu and imid != 0 else {}
	if frees_mouse and _input.enabled and DisplayServer.get_name() != "headless":
		if any:
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
		else:
			_input.capture_mouse()
	engaged.emit(view, open)
