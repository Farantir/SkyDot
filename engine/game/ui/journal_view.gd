# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game's Journal Menu (quest_journal.swf): three tabs, Quests, General Stats and System, over the world (the item
# menus' image space), with the bottom bar's buttons, level meter and date. Esc (Pause) opens it on the tab it was
# closed on, J (Journal) on Quests; Tab, Esc or J close it. Drawn from the movie through UiDraw and driven as its
# ActionScript does:
#   Quest_Journal.handleInput    the page first; then Tab (cancel) closes; the gamepad triggers (L2/R2) switch tabs;
#                                a click on a tab switches to it (there is no keyboard key for the tabs)
#   QuestsPage                   up/down choose a quest (CenteredScrollingList: the chosen one centred, larger), right
#                                moves to its objectives, left back; Enter toggles the quest active (not a completed
#                                one; MISCELLANEOUS toggles its objectives); M shows on map (no map: nothing)
#   StatsPage                    up/down choose a category, the list shows its stats (JournalLog.stat_rows)
#   SystemPage                   JournalSystem (ui/journal_system.gd)
# The lists are drawn from their Entry clips: positions as BSScrollingList / CenteredScrollingList.UpdateList stack
# them (each clip below the previous one by its height, up to the list's border height), the chosen entry of a
# centred list at 100% alpha and the others at 60% (the PC's rule in CenteredScrollingList.SetEntry).
class_name JournalView
extends GameMenu
## Show on Map (M): the journal closes for the world map.
signal show_on_map

const SWF := "quest_journal"
const MENU := "QuestJournalFader/Menu_mc"
const TABS := ["QuestsTab", "StatsTab", "SystemTab"]
const TAB_TEXT := ["$QUESTS", "$GENERAL STATS", "$SYSTEM"]
const FADERS := ["QuestsFader", "StatsFader", "SystemFader"]
const QUESTS := 0
const STATS := 1
const SYSTEM := 2
## QuestCenteredList.SetEntryText: a completed quest's title colour.
const COMPLETED_COLOUR := Color8(0x60, 0x60, 0x60)
## QuestsPage.SetDescriptionText: the objectives header 25 below the description, the list 10 below the header.
const HEADER_GAP := 25.0
const LIST_GAP := 10.0
## JournalBottomBar.PositionButtons: 20 between a button's text and the next button's art.
const BUTTON_GAP := 20.0

var tab := QUESTS
var system: JournalSystem
## The quest list (JournalLog.title_list) and the chosen entry; the objectives' focus and chosen one (-1 none).
var titles: Array = []
var title_index := 0
var objectives_focused := false
var objective_index := -1
## The stats page's category (0 GENERAL ... 5 CRIME).
var category := 0

var _session: GameSession
## The tab the menu was closed on: Esc opens it there (RememberCurrentTabIndex / RestoreSavedSettings).
var _saved_tab := QUESTS


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	visible = false
	system = JournalSystem.new(self, session)
	# The scripts change quests while the menu is open (a stage's fragment displays its objectives a frame later).
	session.quest_stage.connect(func(_q: int, _s: int, _t: String) -> void: _refresh())
	session.objective_changed.connect(func(_q: int, _i: int, _s: String, _t: String) -> void: _refresh())


func _refresh() -> void:
	if not _open:
		return
	if tab == QUESTS:
		var quest := int(titles[title_index]["quest"]) if not titles.is_empty() else -2
		titles = _session.journal.title_list() if _session.journal != null else []
		title_index = 0
		for i in titles.size():
			if int(titles[i]["quest"]) == quest:
				title_index = i
	queue_redraw()


## Whether the pack has the movie.
static func available() -> bool:
	return UiAssets.available() and not UiAssets.instance(SWF, MENU).is_empty()


## Open on `which` (QUESTS, STATS, SYSTEM), or on the tab it was last closed on (-1, the Pause key).
func open(which := -1) -> void:
	if _open:
		return
	_open = true
	tab = _saved_tab if which < 0 else which
	visible = true
	if _session.journal != null:
		_session.count_gold()
	system.on_open()
	_start_page()
	queue_redraw()
	engaged.emit(true)


func _closed() -> void:
	_saved_tab = tab
	visible = false


func switch_tab(to: int) -> void:
	to = posmod(to, TABS.size())
	if to == tab:
		return
	tab = to
	_start_page()
	queue_redraw()


## A page comes to the front: its data is read again (startPage).
func _start_page() -> void:
	match tab:
		QUESTS:
			var journal := _session.journal
			titles = journal.title_list() if journal != null else []
			title_index = clampi(title_index, 0, maxi(titles.size() - 1, 0))
			_skip_divider(1)
			objectives_focused = false
			objective_index = -1
		STATS:
			category = clampi(category, 0, JournalLog.CATEGORIES.size() - 1)
		SYSTEM:
			system.start_page()


func _process(delta: float) -> void:
	if _open and tab == SYSTEM:
		system.step(delta)


func _menu_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion:
		_hover(event.position)
		return
	var handled := _page_input(event)
	if not handled:
		handled = true
		if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
			var over := _tab_at(event.position)
			if over >= 0:
				switch_tab(over)
		elif event is InputEventJoypadButton and event.pressed and event.button_index == JOY_BUTTON_LEFT_SHOULDER:
			switch_tab(tab - 1)
		elif event is InputEventJoypadButton and event.pressed and event.button_index == JOY_BUTTON_RIGHT_SHOULDER:
			switch_tab(tab + 1)
		elif event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"journal"):
			close()
		else:
			handled = false
	if _consume(event, handled):
		queue_redraw()


## The page's own keys first (Quest_Journal passes input down the focus path before its own Tab handling).
func _page_input(event: InputEvent) -> bool:
	match tab:
		QUESTS:
			return _quests_input(event)
		STATS:
			if event.is_action_pressed(&"ui_up", true) or _wheel(event, MOUSE_BUTTON_WHEEL_UP):
				category = maxi(category - 1, 0)
				return true
			if event.is_action_pressed(&"ui_down", true) or _wheel(event, MOUSE_BUTTON_WHEEL_DOWN):
				category = mini(category + 1, JournalLog.CATEGORIES.size() - 1)
				return true
			if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
				var hit := _centered_hit("StatsFader/Page_mc/CategoryList_mc/List_mc", JournalLog.CATEGORIES.size(), category, event.position)
				if hit >= 0:
					category = hit
					return true
		SYSTEM:
			return system.input(event)
	return false


func _quests_input(event: InputEvent) -> bool:
	if titles.is_empty():
		return false
	var objectives := _current_objectives()
	if event.is_action_pressed(&"ui_up", true) or _wheel(event, MOUSE_BUTTON_WHEEL_UP):
		if objectives_focused:
			objective_index = maxi(objective_index - 1, 0)
		else:
			title_index = maxi(title_index - 1, 0)
			_skip_divider(-1)
		return true
	if event.is_action_pressed(&"ui_down", true) or _wheel(event, MOUSE_BUTTON_WHEEL_DOWN):
		if objectives_focused:
			objective_index = mini(objective_index + 1, objectives.size() - 1)
		else:
			title_index = mini(title_index + 1, titles.size() - 1)
			_skip_divider(1)
		return true
	if event.is_action_pressed(&"ui_left"):
		objectives_focused = false
		objective_index = -1
		return true
	if event.is_action_pressed(&"ui_right"):
		if not objectives.is_empty():
			objectives_focused = true
			objective_index = 0
		return true
	if event.is_action_pressed(&"ui_accept"):
		_toggle_active()
		return true
	if event is InputEventKey and event.pressed and event.physical_keycode == KEY_M:
		show_on_map.emit()  # the world map (its quest targets are not shown yet)
		return true
	if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		var hit := _centered_hit("QuestsFader/Page_mc/TitleList_mc/List_mc", titles.size(), title_index, event.position)
		if hit >= 0 and not titles[hit]["divider"]:
			if hit == title_index:
				_toggle_active()
			title_index = hit
			objectives_focused = false
			return true
	return false


## onTitleListSelect: a quest's Toggle Active (not a completed one); MISCELLANEOUS toggles its objectives.
func _toggle_active() -> void:
	if titles.is_empty() or _session.journal == null:
		return
	var entry: Dictionary = titles[title_index]
	if entry["completed"] or entry["divider"]:
		return
	entry["active"] = _session.journal.toggle_active(int(entry["quest"]))


func _skip_divider(direction: int) -> void:
	if titles.is_empty():
		return
	if titles[title_index]["divider"]:
		var next := title_index + direction
		title_index = next if next >= 0 and next < titles.size() else title_index - direction


func _objective_has_target(objective: Dictionary) -> bool:
	for o: Dictionary in _session.world.get_quest(int(objective["quest"])).get("objectives", []):
		if int(o["index"]) == int(objective["index"]):
			return not (o.get("targets", []) as Array).is_empty()
	return false


func _current_objectives() -> Array:
	if titles.is_empty() or _session.journal == null:
		return []
	var entry: Dictionary = titles[title_index]
	return [] if entry["divider"] else _session.journal.objectives(int(entry["quest"]))


static func _wheel(event: InputEvent, button: MouseButton) -> bool:
	return event is InputEventMouseButton and event.pressed and event.button_index == button


func _hover(at: Vector2) -> void:
	match tab:
		QUESTS:
			var hit := _centered_hit("QuestsFader/Page_mc/TitleList_mc/List_mc", titles.size(), title_index, at)
			if hit >= 0 and hit != title_index and not titles[hit]["divider"]:
				title_index = hit
				objectives_focused = false
				queue_redraw()
		STATS:
			var hit := _centered_hit("StatsFader/Page_mc/CategoryList_mc/List_mc", JournalLog.CATEGORIES.size(), category, at)
			if hit >= 0 and hit != category:
				category = hit
				queue_redraw()
		SYSTEM:
			if system.hover(at):
				queue_redraw()


# ---- drawing

## Movie pixels to this control: the stage stretched to the window, as the game shows its menus.
## An instance under Menu_mc by its path ("QuestsFader/Page_mc/..."): {id, xform} with xform to this control.
## BottomBar is locked to the bottom of the visible stage less the safe zone (Quest_Journal.InitExtensions:
## Lock("B"), _y = visibleRect.bottom - safeRect.y).
func at(path: String) -> Dictionary:
	var placed := UiAssets.instance(SWF, MENU + "/" + path)
	if placed.is_empty():
		return {}
	var xform: Transform2D = placed["xform"]
	if path.begins_with("BottomBar"):
		var bar := UiAssets.instance(SWF, MENU + "/BottomBar")
		var shift := STAGE.y - HudGame.SAFE_ZONE.y - (bar["xform"] as Transform2D).origin.y
		xform = Transform2D(0.0, Vector2(0.0, shift)) * xform
	return {"id": int(placed["id"]), "xform": fill() * xform}


func _draw() -> void:
	if not _open or not UiAssets.available():
		return
	var menu := UiAssets.instance(SWF, MENU)
	if menu.is_empty():
		return
	var look := UiDraw.text_look()
	for name: String in FADERS + TABS + ["BottomBar"]:
		look.hidden[name] = true
	UiDraw.character(self, SWF, int(menu["id"]), fill() * (menu["xform"] as Transform2D), Color.WHITE, look)
	_draw_tabs()
	match tab:
		QUESTS:
			_draw_quests()
		STATS:
			_draw_stats()
		SYSTEM:
			system.draw(self)
	_draw_bottom_bar()


func _draw_tabs() -> void:
	for i in TABS.size():
		var placed := at(TABS[i])
		if placed.is_empty():
			continue
		var id := int(placed["id"])
		var frame := UiAssets.label_frame(SWF, id, "selected_up" if i == tab else "up")
		var look := UiDraw.text_look()
		look.draw_texts = true
		look.texts["textField"] = UiDraw.translate(TAB_TEXT[i])
		UiDraw.character(self, SWF, id, placed["xform"], Color.WHITE, look, Rect2(), maxi(frame, 0))


## The tab whose button holds `point` (this control's pixels), else -1.
func _tab_at(point: Vector2) -> int:
	for i in TABS.size():
		var placed := at(TABS[i])
		if not placed.is_empty() and ((placed["xform"] as Transform2D) * UiAssets.bounds(SWF, int(placed["id"]))).has_point(point):
			return i
	return -1


func _draw_quests() -> void:
	var page := at("QuestsFader/Page_mc")
	if page.is_empty():
		return
	var page_xform: Transform2D = page["xform"]
	var look := UiDraw.text_look()
	look.draw_texts = true
	look.hidden = {"TitleList_mc": true, "objectiveList": true, "questTitleText": true, "questDescriptionText": true,
		"NoQuestsText": true, "questTitleEndpieces": true, "objectivesHeader": true}
	look.named_frames["Divider"] = UiAssets.label_frame(SWF, 533, "Left" if objectives_focused else "Right")
	UiDraw.character(self, SWF, int(page["id"]), page_xform, Color.WHITE, look)
	if titles.is_empty():
		var none := _child(int(page["id"]), "NoQuestsText")
		if not none.is_empty():
			UiDraw.field(self, SWF, int(none["character"]), page_xform * UiDraw.matrix(none["matrix"]), UiDraw.translate("$No Active Quests"), Color(0, 0, 0, 0), true)
		return
	# The title list.
	var rows := []
	for entry: Dictionary in titles:
		rows.append({"textField": entry["text"], "divider": entry["divider"],
			"colour": COMPLETED_COLOUR if entry["completed"] else Color(0, 0, 0, 0),
			"EquipIcon": "Equipped" if entry["active"] and not entry["completed"] else "None"})
	draw_centered_list("QuestsFader/Page_mc/TitleList_mc/List_mc", rows, title_index)
	var chosen: Dictionary = titles[title_index]
	if chosen["divider"]:
		return
	var quest := int(chosen["quest"])
	var journal := _session.journal
	# Title, endpieces, description.
	var title := _child(int(page["id"]), "questTitleText")
	if not title.is_empty():
		UiDraw.field(self, SWF, int(title["character"]), page_xform * UiDraw.matrix(title["matrix"]), String(chosen["text"]), Color(0, 0, 0, 0), true)
	var ends := _child(int(page["id"]), "questTitleEndpieces")
	if not ends.is_empty():
		var type := int(chosen["type"])
		var label: String = JournalLog.ENDPIECES[type] if type >= 0 and type < JournalLog.ENDPIECES.size() else "Misc"
		UiDraw.character(self, SWF, int(ends["character"]), page_xform * UiDraw.matrix(ends["matrix"]), Color.WHITE, null,
			Rect2(), maxi(UiAssets.label_frame(SWF, int(ends["character"]), label), 0))
	var misc := quest == 0
	var desc := _child(int(page["id"]), "questDescriptionText")
	var desc_bottom := 0.0
	var desc_y := 0.0
	if not desc.is_empty():
		var desc_xform := UiDraw.matrix(desc["matrix"])
		desc_y = desc_xform.origin.y
		var text := "" if misc else journal.description(quest)
		desc_bottom = text_block(int(desc["character"]), page_xform * desc_xform, text)
	# The objectives header and list follow the description (SetDescriptionText).
	var header := _child(int(page["id"]), "objectivesHeader")
	var list_y := desc_y
	if not header.is_empty() and not misc:
		var header_xform := UiDraw.matrix(header["matrix"])
		header_xform.origin.y = desc_y + desc_bottom + HEADER_GAP
		var hlook := UiDraw.text_look()
		hlook.draw_texts = true
		hlook.texts["#535"] = UiDraw.translate("$OBJECTIVES")
		UiDraw.character(self, SWF, int(header["character"]), page_xform * header_xform, Color.WHITE, hlook)
		list_y = header_xform.origin.y + UiAssets.bounds(SWF, int(header["character"])).end.y + LIST_GAP
	var objectives := journal.objectives(quest)
	var orow := []
	for o: Dictionary in objectives:
		var selected := objectives_focused and orow.size() == objective_index
		orow.append({"textField": o["text"], "frame": String(o["state"]) + ("Selected" if selected else "")})
	draw_list("QuestsFader/Page_mc/objectiveList", orow, -1, 0, list_y)


func _draw_stats() -> void:
	var page := at("StatsFader/Page_mc")
	if page.is_empty():
		return
	var look := UiDraw.text_look()
	look.hidden = {"CategoryList_mc": true, "StatsList_mc": true}
	UiDraw.character(self, SWF, int(page["id"]), page["xform"], Color.WHITE, look)
	var rows := []
	for name: String in JournalLog.CATEGORIES:
		rows.append({"textField": UiDraw.translate("$" + name)})
	draw_centered_list("StatsFader/Page_mc/CategoryList_mc/List_mc", rows, category)
	var stats := []
	if _session.journal != null:
		for row: Dictionary in _session.journal.stat_rows(category):
			stats.append({"textField": UiDraw.translate("$" + String(row["text"])), "valueText": str(row["value"])})
	draw_list("StatsFader/Page_mc/StatsList_mc", stats, -1, 0)


## JournalBottomBar.SetMode: Quests shows Toggle Active and Show on Map, General Stats and System the level meter;
## the date always.
func _draw_bottom_bar() -> void:
	var bar := at("BottomBar")
	if bar.is_empty():
		return
	var look := UiDraw.text_look()
	look.draw_texts = true
	look.hidden["ButtonRect"] = true
	if tab == QUESTS:
		look.hidden["LevelMeterRect"] = true
	var progression := _session.progression
	if progression != null:
		var progress := progression.data().progress()
		var level := int(progress["level"])
		look.texts["LevelNumberLabel"] = str(level)
		look.fills["LevelProgressBar"] = clampf(float(progress["xp"]) / maxf(progression.level_up_threshold(level), 1.0), 0.0, 1.0)
	look.texts["#684"] = UiDraw.translate("$LEVEL")
	look.texts["DateText"] = UiDraw.date_text(_session)
	UiDraw.character(self, SWF, int(bar["id"]), bar["xform"], Color.WHITE, look)
	var buttons := []
	if tab == QUESTS and not titles.is_empty():
		var chosen: Dictionary = titles[title_index]
		if not chosen["completed"]:
			buttons.append(["Enter", UiDraw.translate("$Toggle Active"), 1.0])
			# UpdateShowOnMapButtonAlpha: full when an objective has a quest target, else dimmed.
			var targets := false
			for o: Dictionary in _current_objectives():
				targets = targets or _objective_has_target(o)
			buttons.append(["M", UiDraw.translate("$Show on Map"), 1.0 if targets else 0.5])
	elif tab == SYSTEM:
		buttons = system.buttons()
	draw_buttons("BottomBar/ButtonRect", buttons)


## Bottom-bar style buttons (CrossPlatformButtons, MouseButton1-3): [art export, label, alpha]; each button's key art
## sits left of its text, the next button 20 px after the text (PositionButtons).
func draw_buttons(rect_path: String, buttons: Array, names := ["MouseButton1", "MouseButton2", "MouseButton3"]) -> void:
	var rect := at(rect_path)
	if rect.is_empty() or buttons.is_empty():
		return
	var x := 0.0
	for i in mini(buttons.size(), names.size()):
		var child := _child(int(rect["id"]), names[i])
		if child.is_empty():
			continue
		var art := UiAssets.exported(SWF, String(buttons[i][0]))
		var art_bounds := UiAssets.bounds(SWF, art) if art >= 0 else Rect2()
		var button_xform := UiDraw.matrix(child["matrix"])
		x += art_bounds.size.x
		button_xform.origin.x = x
		var xform: Transform2D = (rect["xform"] as Transform2D) * button_xform
		var modulate := Color(1, 1, 1, float(buttons[i][2]))
		var height := 27.0
		if art >= 0:
			UiDraw.character(self, SWF, art, xform * Transform2D(0.0, Vector2(-art_bounds.size.x - art_bounds.position.x,
				(height - art_bounds.size.y) * 0.5 - art_bounds.position.y)), modulate)
		var field := _child(int(child["character"]), "textField")
		var width := 0.0
		if not field.is_empty():
			var drawn := UiDraw.field(self, SWF, int(field["character"]), xform * UiDraw.matrix(field["matrix"]),
				String(buttons[i][1]), Color(String(UiAssets.character(SWF, int(field["character"])).get("color", "#ffffffff"))) * modulate, true)
			width = drawn.x / maxf((xform.x.length()), 0.001)
		x += width + BUTTON_GAP


## A list drawn as BSScrollingList.UpdateList stacks its Entry clips: from `scroll`, each below the previous by its
## height, while they fit the border's height. Rows: {field name: text, "frame": label, "colour": Color}. `selected`
## gets the "Selected" frame when the row names none. `y` (when not NAN) moves the list in its parent.
func draw_list(path: String, rows: Array, selected: int, scroll: int, y := NAN) -> void:
	var list := at(path)
	if list.is_empty():
		return
	var xform: Transform2D = list["xform"]
	if not is_nan(y):
		var parent := at(path.get_base_dir())
		var own := (parent["xform"] as Transform2D).affine_inverse() * xform
		own.origin.y = y
		xform = (parent["xform"] as Transform2D) * own
	var clips := _entries(int(list["id"]))
	if clips.is_empty():
		return
	var height := _border_height(int(list["id"]))
	var top := UiDraw.matrix(clips[0]["matrix"]).origin.y
	var used := 0.0
	var shown := 0
	for i in range(scroll, rows.size()):
		if shown >= clips.size():
			break
		var clip: Dictionary = clips[shown]
		var clip_h := _clip_height(clip)
		if used + clip_h > height + 0.5 and shown > 0:
			break
		var row: Dictionary = rows[i]
		var frame: String = row.get("frame", "Selected" if i == selected else "Normal")
		draw_entry(clip, xform, top + used, row, frame, 1.0)
		used += clip_h
		shown += 1
	# The list's scroll bar shows when it scrolls (SetScrollbarVisibility: iMaxScrollPosition > 0).
	var fit := 0
	var fit_used := 0.0
	while fit < clips.size() and fit < rows.size() and fit_used + _clip_height(clips[fit]) <= height + 0.5:
		fit_used += _clip_height(clips[fit])
		fit += 1
	if rows.size() > fit:
		draw_scrollbar(int(list["id"]), xform, scroll, rows.size() - fit, fit)


## A list's ScrollUp / ScrollDown arrows (BSScrollingList.UpdateList: up while scrolled, down while more follow).
func draw_scroll_arrows(list_path: String, position: int, max_position: int) -> void:
	var list := at(list_path)
	if list.is_empty():
		return
	for name: String in ["ScrollUp", "ScrollDown"]:
		var arrow := _child(int(list["id"]), name)
		if arrow.is_empty() or (name == "ScrollUp" and position <= 0) or (name == "ScrollDown" and position >= max_position):
			continue
		UiDraw.character(self, SWF, int(arrow["character"]), (list["xform"] as Transform2D) * UiDraw.matrix(arrow["matrix"]))


## A list's ScrollBar (gfx.controls.ScrollBar.draw): the thumb pageSize / (max + pageSize) of the track, placed at
## position / max of the track's free length; the arrows disabled at the ends.
func draw_scrollbar(list_id: int, list_xform: Transform2D, position: int, max_position: int, page: int) -> void:
	var bar := _child(list_id, "scrollbar")
	if bar.is_empty():
		return
	var bar_id := int(bar["character"])
	var track := _child(bar_id, "track")
	var thumb := _child(bar_id, "thumb")
	if track.is_empty() or thumb.is_empty():
		return
	var track_rect := UiDraw.matrix(track["matrix"]) * UiAssets.bounds(SWF, int(track["character"]))
	var thumb_bounds := UiAssets.bounds(SWF, int(thumb["character"]))
	var thumb_h := maxf(10.0, minf(track_rect.size.y, float(page) / float(max_position + page) * track_rect.size.y))
	var thumb_y := track_rect.position.y + float(position) / float(maxi(max_position, 1)) * (track_rect.size.y - thumb_h)
	var look := UiDraw.text_look()
	look.hidden["thumb"] = true
	look.named_frames["upArrow"] = UiAssets.label_frame(SWF, _child_id(bar_id, "upArrow"), "disabled" if position == 0 else "up")
	look.named_frames["downArrow"] = UiAssets.label_frame(SWF, _child_id(bar_id, "downArrow"), "disabled" if position >= max_position else "up")
	var bar_xform := list_xform * UiDraw.matrix(bar["matrix"])
	UiDraw.character(self, SWF, bar_id, bar_xform, Color.WHITE, look)
	var thumb_xform := UiDraw.matrix(thumb["matrix"])
	thumb_xform = Transform2D(thumb_xform.x, thumb_xform.y * (thumb_h / maxf(thumb_bounds.size.y, 0.001)),
		Vector2(thumb_xform.origin.x, thumb_y - thumb_bounds.position.y * thumb_h / maxf(thumb_bounds.size.y, 0.001)))
	UiDraw.character(self, SWF, int(thumb["character"]), bar_xform * thumb_xform)


## A centred list (CenteredScrollingList.UpdateList): the clips above the larger one hold the rows before `selected`,
## the larger one `selected`, the rest the rows after it; every clip below the previous by its height.
func draw_centered_list(path: String, rows: Array, selected: int) -> void:
	for slot: Dictionary in centered_slots(path, rows.size(), selected):
		var row: Dictionary = rows[slot["row"]]
		var frame := "Divider" if row.get("divider", false) else "Normal"
		draw_entry(slot["clip"], slot["xform"], slot["y"], row, frame, 1.0 if slot["row"] == selected else 0.6)


## Where a centred list puts its rows: [{row, clip, xform (the list's), y, height}].
func centered_slots(path: String, count: int, selected: int) -> Array:
	var list := at(path)
	if list.is_empty() or count == 0:
		return []
	var clips := _entries(int(list["id"]))
	var top_half := 0
	for i in clips.size():
		if int(clips[i]["character"]) != int(clips[0]["character"]):
			top_half = i
			break
	var height := _border_height(int(list["id"]))
	var y := UiDraw.matrix(clips[0]["matrix"]).origin.y
	var used := 0.0
	var out := []
	for i in top_half:
		var row := selected - top_half + i
		var clip_h := _clip_height(clips[i])
		if row >= 0:
			out.append({"row": row, "clip": clips[i], "xform": list["xform"], "y": y + used, "height": clip_h})
		used += clip_h
	var row := selected
	var slot := top_half
	while row < count and slot < clips.size():
		var clip_h := _clip_height(clips[slot])
		if used + clip_h > height + 0.5:
			break
		out.append({"row": row, "clip": clips[slot], "xform": list["xform"], "y": y + used, "height": clip_h})
		used += clip_h
		row += 1
		slot += 1
	return out


## The row of a centred list under `point`, else -1.
func _centered_hit(path: String, count: int, selected: int, point: Vector2) -> int:
	for slot: Dictionary in centered_slots(path, count, selected):
		var b := UiAssets.bounds(SWF, int(slot["clip"]["character"]))
		var local := UiDraw.matrix(slot["clip"]["matrix"])
		local.origin.y = slot["y"]
		if ((slot["xform"] as Transform2D) * local * b).has_point(point):
			return slot["row"]
	return -1


## One Entry clip at list-local `y`, at the frame labelled `frame` (0 when it has none), its fields' texts from `row`.
func draw_entry(clip: Dictionary, list_xform: Transform2D, y: float, row: Dictionary, frame: String, alpha: float) -> void:
	var id := int(clip["character"])
	var local := UiDraw.matrix(clip["matrix"])
	local.origin.y = y
	var look := UiDraw.text_look()
	look.draw_texts = true
	for key: String in row:
		if row[key] is String and key != "frame":
			look.texts[key] = row[key]
	if row.get("colour", Color(0, 0, 0, 0)) is Color and (row.get("colour", Color(0, 0, 0, 0)) as Color).a > 0.0:
		look.colours["textField"] = row["colour"]
	for key: String in row:
		if row[key] is String and UiAssets.label_frame(SWF, _child_id(id, key), row[key]) >= 0:
			look.named_frames[key] = UiAssets.label_frame(SWF, _child_id(id, key), row[key])
	for key: String in row.get("hide", []):
		look.hidden[key] = true
	for key: String in row.get("colours", {}):
		look.colours[key] = row["colours"][key]
	var frame_index := maxi(UiAssets.label_frame(SWF, id, frame), 0)
	# UiDraw tints shapes by the modulate, not texts: an entry's _alpha reaches its fields through their colours.
	if alpha < 1.0:
		for placed: Dictionary in UiAssets.display_list(SWF, id, frame_index):
			var child := UiAssets.character(SWF, int(placed["character"]))
			var name: String = placed.get("name", "")
			if child.get("type", "") == "edit_text" and name != "":
				var base: Color = look.colours.get(name, Color(String(child.get("color", "#ffffffff"))))
				look.colours[name] = Color(base, base.a * alpha)
	for key: String in row.get("fit", []):
		look.hidden[key] = true
	UiDraw.character(self, SWF, id, list_xform * local, Color(1, 1, 1, alpha), look, Rect2(), frame_index)
	# Fields that shrink their text to fit (textAutoSize "shrink").
	for key: String in row.get("fit", []):
		for placed: Dictionary in UiAssets.display_list(SWF, id, frame_index):
			if placed.get("name", "") == key:
				var colour := Color(String(UiAssets.character(SWF, int(placed["character"])).get("color", "#ffffffff")))
				fit_field(int(placed["character"]), list_xform * local * UiDraw.matrix(placed["matrix"]), String(row[key]),
					Color(colour, colour.a * alpha))


## A one-line field whose text shrinks to fit its box (textAutoSize "shrink"): the font size goes down until the text
## fits the box's width, the line stays centred in the box's height; glyphs stretched with the stage (UiDraw.field).
func fit_field(field_id: int, xform: Transform2D, text: String, colour: Color) -> void:
	var entry := UiAssets.character(SWF, field_id)
	var font := UiAssets.font(String(entry.get("font", "")))
	if font == null:
		font = ThemeDB.fallback_font
	var box := UiAssets.bounds(SWF, field_id)
	var sx := xform.get_scale().x
	var ratio := xform.get_scale().y / maxf(sx, 0.0001)
	var width := (box.size.x - 4.0) * sx
	var size_px := maxi(int(round(float(entry.get("height", 20.0)) * sx)), 6)
	while size_px > 6 and font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, size_px).x > width:
		size_px -= 1
	var align := HORIZONTAL_ALIGNMENT_LEFT
	match String(entry.get("align", "left")):
		"center":
			align = HORIZONTAL_ALIGNMENT_CENTER
		"right":
			align = HORIZONTAL_ALIGNMENT_RIGHT
	var left := xform * Vector2(box.position.x + 2.0, box.get_center().y)
	var base_y := left.y / ratio + (font.get_ascent(size_px) - font.get_descent(size_px)) * 0.5
	draw_set_transform(Vector2.ZERO, 0.0, Vector2(1.0, ratio))
	draw_string_outline(font, Vector2(left.x, base_y), text, align, width, size_px, 3, Color(0, 0, 0, 0.8))
	draw_string(font, Vector2(left.x, base_y), text, align, width, size_px, colour)
	draw_set_transform_matrix(Transform2D.IDENTITY)


## A multi-line text field (questDescriptionText): word-wrapped to the field's width with its font and size. Returns
## the text's height in movie pixels (getCharBoundaries of the last line's bottom).
func text_block(field_id: int, xform: Transform2D, text: String) -> float:
	var entry := UiAssets.character(SWF, field_id)
	var font := UiAssets.font(String(entry.get("font", "")))
	if font == null:
		font = ThemeDB.fallback_font
	var size_movie := float(entry.get("height", 20.0))
	var scale := xform.get_scale().x
	var size_px := maxi(int(round(size_movie * scale)), 6)
	var box := UiAssets.bounds(SWF, field_id)
	var width := (box.size.x - 4.0) * scale
	var lines := font.get_multiline_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, width, size_px).y if text != "" else 0.0
	var left := xform * Vector2(box.position.x + 2.0, box.position.y + 2.0)
	var colour := Color(String(entry.get("color", "#ffffffff")))
	var ratio := xform.get_scale().y / maxf(scale, 0.0001)  # glyphs stretched with the stage (UiDraw.field)
	var align := HORIZONTAL_ALIGNMENT_CENTER if String(entry.get("align", "left")) == "center" else HORIZONTAL_ALIGNMENT_LEFT
	if text != "":
		var base := Vector2(left.x, left.y / ratio + font.get_ascent(size_px))
		draw_set_transform(Vector2.ZERO, 0.0, Vector2(1.0, ratio))
		draw_multiline_string_outline(font, base, text, align, width, size_px, -1, 3, Color(0, 0, 0, 0.8))
		draw_multiline_string(font, base, text, align, width, size_px, -1, colour)
		draw_set_transform_matrix(Transform2D.IDENTITY)
	return lines * ratio / maxf(xform.get_scale().y, 0.001) + 2.0


## A named child of a sprite (its display entry), empty when missing.
static func _child(id: int, name: String) -> Dictionary:
	for placed: Dictionary in UiAssets.character(SWF, id).get("display", []):
		if placed.get("name", "") == name:
			return placed
	return {}


static func _child_id(id: int, name: String) -> int:
	var child := _child(id, name)
	return int(child["character"]) if not child.is_empty() else -1


## The Entry0..N clips of a list sprite, in number order.
static func _entries(id: int) -> Array:
	var out := []
	for placed: Dictionary in UiAssets.character(SWF, id).get("display", []):
		var name: String = placed.get("name", "")
		if name.begins_with("Entry") and name.substr(5).is_valid_int():
			out.append(placed)
	out.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return String(a["name"]).substr(5).to_int() < String(b["name"]).substr(5).to_int())
	return out


## The list's border height (fListHeight), in the list's pixels.
static func _border_height(id: int) -> float:
	var border := _child(id, "border")
	if border.is_empty():
		return INF
	return (UiDraw.matrix(border["matrix"]) * UiAssets.bounds(SWF, int(border["character"]))).size.y


static func _clip_height(clip: Dictionary) -> float:
	return (UiDraw.matrix(clip["matrix"]) * UiAssets.bounds(SWF, int(clip["character"]))).size.y
