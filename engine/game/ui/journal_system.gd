# SPDX-License-Identifier: GPL-3.0-or-later
#
# The journal's System tab (SystemPage of quest_journal.swf): the category list on the left and the panel of the
# state it is in, driven as SystemPage's ActionScript does (handleInput, onAcceptPress, onCancelPress, StartState):
#   MAIN          QUICKSAVE, SAVE, LOAD, INSTALLED CONTENT, CREATIONS, SETTINGS, CONTROLS, HELP, QUIT; up/down
#                 choose, Enter or right opens one, Tab/Esc close the journal
#   SAVE_LIST     "[NEW SAVE]" and this character's saves; Enter on [NEW SAVE] saves at once, on a save asks "Save over
#                 this game?"; X asks "Delete this save?"
#   CHARACTERS    LOAD first lists the characters ("Show All Saves" and the player's name); Enter lists the saves
#   LOAD_LIST     every save, newest first, with its number (QUICK, AUTO, or the save's number) and place, and the
#                 chosen one's picture, "name, race, Level n", play time and date; Enter asks "Load this game? All
#                 unsaved progress will be lost."
#   SETTINGS      Gameplay, Display, Audio; Enter shows the options (sliders, steppers, check boxes); left/right change
#                 the chosen one (only those SkyDot has do something: Invert Y)
#   CONTROLS      the Main Gameplay events of the control map with their keys (no remapping)
#   QUIT_LIST     Main Menu, Desktop; Enter asks "Quit to desktop?  Any unsaved progress will be lost." (no main
#                 menu exists: Main Menu's question leads nowhere)
#   CONFIRM       ConfirmPanel: Enter (Yes) does it, Tab/Esc (No) goes back
# Cancel (Tab, Esc, left) in a panel goes back to MAIN (EndState). INSTALLED CONTENT, CREATIONS and HELP have nothing
# in SkyDot and stay closed, as the game does for an empty list.
class_name JournalSystem
extends RefCounted

enum State { MAIN, SAVE_LIST, CHARACTERS, LOAD_LIST, SETTINGS, OPTIONS, CONTROLS, QUIT_LIST, CONFIRM }

const PAGE := "SystemFader/Page_mc"
const CATEGORIES := ["$QUICKSAVE", "$SAVE", "$LOAD", "$INSTALLED CONTENT", "$CREATIONS", "$SETTINGS", "$CONTROLS",
	"$HELP", "$QUIT"]
const PANELS := ["ConfirmPanel", "SettingsPanel", "ControlsPanel", "OptionsListsPanel", "InputMappingPanel",
	"PCQuitPanel", "SaveLoadPanel", "HelpListPanel", "HelpTextPanel", "CreationListPanel", "CreationTextPanel",
	"ErrorText", "CategoryList_mc", "VersionText"]
const SETTINGS_LIST := ["$Gameplay", "$Display", "$Audio"]
## The options SystemPage.onSettingsCategoryPress lists and the PC keeps (the exe drops those without an ID: on PC
## Survival Mode, Kinect, the shadow/light/specularity/tree fades, as the game captures show): [key, movieType
## (0 slider, 1 stepper, 2 check box), default, options].
const OPTIONS := {
	0: [["$Invert Y", 2, 0], ["$Look Sensitivity", 0, 0.5], ["$Vibration", 2, 1], ["$360 Controller", 2, 1],
		["$SaveGameMissingCreationsCheck", 2, 1],
		["$Difficulty", 1, 2, ["$Very Easy", "$Easy", "$Normal", "$Hard", "$Very Hard", "$Legendary"]],
		["$Show Floating Markers", 2, 1], ["$Save on Rest", 2, 1], ["$Save on Wait", 2, 1], ["$Save on Travel", 2, 1],
		["$Save on Pause", 1, 6, ["$5 Mins", "$10 Mins", "$15 Mins", "$30 Mins", "$45 Mins", "$60 Mins", "$Disabled"]]],
	1: [["$Brightness", 0, 0.5], ["$HUD Opacity", 0, 1.0], ["$Actor Fade", 0, 1.0], ["$Item Fade", 0, 1.0],
		["$Object Fade", 0, 1.0], ["$Grass Fade", 0, 1.0], ["$Crosshair", 2, 1], ["$Dialogue Subtitles", 2, 0],
		["$General Subtitles", 2, 0]],
	2: [["$Master", 0, 1.0], ["$Effects", 0, 1.0], ["$Footsteps", 0, 0.8], ["$Voice", 0, 0.8], ["$Music", 0, 0.5]],
}
## SettingsOptionItem and InputMappingList.SetEntry: a row not chosen at 30% alpha.
const UNSELECTED_ALPHA := 0.3
## The game's difficulty names (the stepper shows them as the game words them in English).
const DIFFICULTY_TEXT := {"$Very Easy": "Novice", "$Easy": "Apprentice", "$Normal": "Adept", "$Hard": "Expert",
	"$Very Hard": "Master", "$Legendary": "Legendary"}

var state := State.MAIN
var main_index := 0
var list_index := 0
var list_scroll := 0
var settings_index := 0
var option_index := 0
var mapping_index := 0
var quit_index := 0
## The confirmation shown: {text, action (Callable), back (State)}.
var confirm := {}
## Option values by settings category: [value...] (slider 0-1, stepper index, check box 0/1).
var values := {}
## The world as it looked when the journal opened: the save's picture.
var shot: Image
var _view: JournalView
var _session: GameSession
var _saves: Array = []
var _textures := {}


func _init(view: JournalView, session: GameSession) -> void:
	_view = view
	_session = session
	for category: int in OPTIONS:
		var row := []
		for option: Array in OPTIONS[category]:
			row.append(option[2])
		values[category] = row


## The journal opens: what the screen shows now is the save's picture (taken before the menu is drawn).
func on_open() -> void:
	shot = null
	if DisplayServer.get_name() != "headless" and _view.get_viewport() != null:
		var image := _view.get_viewport().get_texture().get_image()
		if image != null and not image.is_empty():
			image.resize(320, roundi(320.0 * image.get_height() / image.get_width()), Image.INTERPOLATE_BILINEAR)
			shot = image


## The page comes to the front: the main list (SystemPage.startPage keeps its state otherwise).
func start_page() -> void:
	state = State.MAIN
	confirm = {}


func step(_delta: float) -> void:
	pass


## --journal-system: open a panel for a shot or a test ("save", "save-new" (saves to a new numbered save), "load",
## "characters", "settings", "settings:N", "controls", "quit", "confirm-load", "confirm-quit"), as the keys would.
func debug_state(name: String) -> void:
	var parts := name.split(":")
	match parts[0]:
		"save", "save-new":
			main_index = 1
			_open_category(1)
			if parts[0] == "save-new":
				list_index = 0
				_accept_list()  # [NEW SAVE]: saves at once and closes the menu
		"load", "confirm-load":
			main_index = 2
			_open_category(2)
			if parts[0] != "characters":
				_accept_list()
			if parts[0] == "confirm-load":
				_accept_list()
		"characters":
			main_index = 2
			_open_category(2)
		"settings":
			main_index = 5
			_open_category(5)
			if parts.size() > 1:
				settings_index = clampi(parts[1].to_int(), 0, SETTINGS_LIST.size() - 1)
				state = State.OPTIONS
		"controls":
			main_index = 6
			_open_category(6)
		"quit", "confirm-quit":
			main_index = 8
			_open_category(8)
			if parts[0] == "confirm-quit":
				quit_index = 1
				_ask("$Quit to desktop?  Any unsaved progress will be lost.", _quit, State.QUIT_LIST)
	_view.queue_redraw()


# ---- input

func input(event: InputEvent) -> bool:
	if event is InputEventMouseButton and event.pressed:
		return _click(event)
	match state:
		State.CONFIRM:
			if event.is_action_pressed(&"ui_accept"):
				_accept_confirm()
				return true
			if event.is_action_pressed(&"ui_cancel"):
				state = confirm.get("back", State.MAIN)
				confirm = {}
				return true
			return event is InputEventKey
		State.MAIN:
			if event.is_action_pressed(&"ui_up", true):
				main_index = maxi(main_index - 1, 0)
				return true
			if event.is_action_pressed(&"ui_down", true):
				main_index = mini(main_index + 1, CATEGORIES.size() - 1)
				return true
			if event.is_action_pressed(&"ui_accept") or event.is_action_pressed(&"ui_right"):
				_open_category(main_index)
				return true
			return false
	# A panel: up/down move in its list, cancel or left go back to MAIN.
	if event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"ui_left") and state != State.OPTIONS:
		if state == State.OPTIONS:
			state = State.SETTINGS  # the options go back to their categories (the game's capture)
		else:
			_end_state()
		return true
	var delta := 0
	if event.is_action_pressed(&"ui_up", true):
		delta = -1
	elif event.is_action_pressed(&"ui_down", true):
		delta = 1
	match state:
		State.SAVE_LIST, State.LOAD_LIST, State.CHARACTERS:
			var count := _list_rows().size()
			if delta != 0:
				list_index = clampi(list_index + delta, 0, maxi(count - 1, 0))
				_scroll_to(list_index)
				return true
			if event.is_action_pressed(&"ui_accept"):
				_accept_list()
				return true
			if event is InputEventKey and event.pressed and event.physical_keycode == KEY_X and state != State.CHARACTERS:
				_ask_delete()
				return true
			if event is InputEventKey and event.pressed and event.physical_keycode == KEY_T and state == State.LOAD_LIST:
				list_index = 0
				state = State.CHARACTERS  # Character Selection (CHARACTER_LOAD_STATE)
				return true
		State.SETTINGS:
			if delta != 0:
				settings_index = clampi(settings_index + delta, 0, SETTINGS_LIST.size() - 1)
				return true
			if event.is_action_pressed(&"ui_accept"):
				option_index = 0
				state = State.OPTIONS
				return true
		State.OPTIONS:
			var options: Array = OPTIONS[settings_index]
			if event is InputEventKey and event.pressed and event.physical_keycode == KEY_T:
				_ask("$Reset settings to default values?", func() -> void:
					for i in options.size():
						values[settings_index][i] = options[i][2]
					state = State.OPTIONS, State.OPTIONS)
				return true
			if delta != 0:
				option_index = clampi(option_index + delta, 0, options.size() - 1)
				return true
			if event.is_action_pressed(&"ui_left") or event.is_action_pressed(&"ui_right") or event.is_action_pressed(&"ui_accept"):
				_change_option(-1 if event.is_action_pressed(&"ui_left") else 1)
				return true
		State.CONTROLS:
			if delta != 0:
				mapping_index = clampi(mapping_index + delta, 0, maxi(mappings().size() - 1, 0))
				return true
		State.QUIT_LIST:
			if delta != 0:
				quit_index = clampi(quit_index + delta, 0, 1)
				return true
			if event.is_action_pressed(&"ui_accept"):
				_ask(["$Quit to main menu?  Any unsaved progress will be lost.",
					"$Quit to desktop?  Any unsaved progress will be lost."][quit_index], _quit, State.QUIT_LIST)
				return true
	return event is InputEventKey


## OnBottomBarButtonMousePress / onCategoryButtonPress: what a category opens.
func _open_category(index: int) -> void:
	match CATEGORIES[index]:
		"$QUICKSAVE":
			if _session.save_game(SaveService.QUICKSAVE, _info()):
				_view.close()
		"$SAVE":
			_saves = _session.saves.list_saves()
			list_index = 0
			list_scroll = 0
			state = State.SAVE_LIST
		"$LOAD":
			list_index = 0
			list_scroll = 0
			state = State.CHARACTERS
		"$SETTINGS":
			settings_index = 0
			state = State.SETTINGS
		"$CONTROLS":
			mapping_index = 0
			state = State.CONTROLS
		"$QUIT":
			quit_index = 0
			state = State.QUIT_LIST
		_:
			pass  # INSTALLED CONTENT, CREATIONS, HELP: nothing to list (the game plays UIMenuCancel)


func _end_state() -> void:
	state = State.MAIN
	confirm = {}


func _accept_list() -> void:
	match state:
		State.CHARACTERS:
			_saves = _session.saves.list_saves()
			list_index = 0
			list_scroll = 0
			state = State.LOAD_LIST
		State.SAVE_LIST:
			if list_index == 0:
				_save_to(_session.saves.next_save_path())
			else:
				var path: String = _saves[list_index - 1]["path"]
				_ask("$Save over this game?", func() -> void: _save_to(path), State.SAVE_LIST)
		State.LOAD_LIST:
			if list_index < _saves.size():
				var path: String = _saves[list_index]["path"]
				_ask("$Load this game? All unsaved progress will be lost.", func() -> void: _load(path), State.LOAD_LIST)


func _ask_delete() -> void:
	var index := list_index - (1 if state == State.SAVE_LIST else 0)
	if index < 0 or index >= _saves.size():
		return
	var path: String = _saves[index]["path"]
	var back := state
	_ask("$Delete this save?", func() -> void:
		_session.saves.delete(path)
		_saves = _session.saves.list_saves()
		list_index = clampi(list_index, 0, maxi(_list_rows().size() - 1, 0))
		if _saves.is_empty() and back == State.LOAD_LIST:
			state = State.MAIN
		else:
			state = back, back)


func _ask(text: String, action: Callable, back: State) -> void:
	confirm = {"text": text, "action": action, "back": back}
	state = State.CONFIRM


func _accept_confirm() -> void:
	var action: Callable = confirm.get("action", Callable())
	confirm = {}
	state = State.MAIN
	if action.is_valid():
		action.call()


func _save_to(path: String) -> void:
	if _session.save_game(path, _info()):
		_view.close()  # SaveGame, then CloseMenu (DoSaveGame)


func _load(path: String) -> void:
	_view.close()
	_session.load_game(path)


func _quit() -> void:
	if quit_index == 1:
		_view.get_tree().quit()
	# Main Menu: SkyDot has no main menu.


## What a save made here shows in the load list: the picture of the world when the journal opened.
func _info() -> Dictionary:
	var out := {}
	if shot != null:
		out["shot"] = shot.save_png_to_buffer()
	return out


func _change_option(direction: int) -> void:
	var option: Array = OPTIONS[settings_index][option_index]
	var row: Array = values[settings_index]
	match int(option[1]):
		0:
			row[option_index] = clampf(float(row[option_index]) + 0.05 * direction, 0.0, 1.0)
		1:
			row[option_index] = posmod(int(row[option_index]) + direction, (option[3] as Array).size())
		2:
			row[option_index] = 1 - int(row[option_index])
	if option[0] == "$Invert Y":
		var input := _view.get_tree().root.find_children("*", "PlayerInput", true, false)
		for node: Node in input:
			node.set(&"invert_look_y", int(row[option_index]) == 1)


func _scroll_to(index: int) -> void:
	var shown := 11  # SaveLoadPanel's list: 11 Entry clips
	if index < list_scroll:
		list_scroll = index
	elif index >= list_scroll + shown:
		list_scroll = index - shown + 1


## The mouse: a click on a category opens it (MAIN) or on a row chooses it.
func _click(event: InputEventMouseButton) -> bool:
	if event.button_index == MOUSE_BUTTON_WHEEL_UP or event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
		var up := event.button_index == MOUSE_BUTTON_WHEEL_UP
		var action := InputEventAction.new()
		action.action = &"ui_up" if up else &"ui_down"
		action.pressed = true
		return input(action)
	if event.button_index != MOUSE_BUTTON_LEFT:
		return false
	if state == State.MAIN:
		var hit := _main_hit(event.position)
		if hit >= 0:
			if hit == main_index:
				_open_category(hit)
			main_index = hit
			return true
	return false


func hover(at: Vector2) -> bool:
	if state != State.MAIN:
		return false
	var hit := _main_hit(at)
	if hit >= 0 and hit != main_index:
		main_index = hit
		return true
	return false


func _main_hit(point: Vector2) -> int:
	for slot: Dictionary in _view.centered_slots(PAGE + "/CategoryList_mc/List_mc", CATEGORIES.size(), main_index):
		var local := UiDraw.matrix(slot["clip"]["matrix"])
		local.origin.y = slot["y"]
		if ((slot["xform"] as Transform2D) * local * UiAssets.bounds(JournalView.SWF, int(slot["clip"]["character"]))).has_point(point):
			return slot["row"]
	return -1


# ---- data

## The rows of the list panel shown: [{textField, SaveNumber}].
func _list_rows() -> Array:
	var rows := []
	match state:
		State.CHARACTERS:
			rows.append({"textField": JournalView.translate("$ShowAllSave"), "SaveNumber": ""})
			rows.append({"textField": _player_name(), "SaveNumber": ""})
		State.SAVE_LIST, State.LOAD_LIST:
			if state == State.SAVE_LIST:
				rows.append({"textField": JournalView.translate("$[NEW SAVE]"), "SaveNumber": ""})
			for save: Dictionary in _saves:
				rows.append({"textField": String(save.get("location", "")), "SaveNumber": String(save.get("number", ""))})
		State.CONFIRM:
			var back: State = confirm.get("back", State.MAIN)
			if back == State.SAVE_LIST or back == State.LOAD_LIST:
				var keep := state
				state = back
				rows = _list_rows()
				state = keep
	return rows


func _player_name() -> String:
	if _session.world == null:
		return ""
	return String(_session.world.get_npc(0x7).get("name", ""))


## The control map's Main Gameplay events a player may remap, with their keys: [{text, key}].
func mappings() -> Array:
	var text := UiAssets.file(ControlMap.PATH, ControlMap.PATH.get_file()).get_string_from_utf8() if UiAssets.available() else ""
	var out := []
	var context := ""
	var reached := false
	for line in text.split("\n"):
		var stripped := line.strip_edges()
		if stripped.begins_with("//"):
			context = stripped.trim_prefix("//").strip_edges() if not reached else context
			continue
		if stripped == "":
			if reached:
				break
			continue
		if context != "Main Gameplay":
			continue
		reached = true
		var fields: Array[String] = []
		for field in line.split("\t", false):
			if field.strip_edges() != "":
				fields.append(field.strip_edges())
		if fields.size() < 5 or fields[4] != "1":
			continue
		var keys := ControlMap.parse(text)
		var event := ControlMap.event_of(keys, "Main Gameplay", fields[0])
		var art := ""
		if not (event.get("keys", []) as Array).is_empty():
			art = key_art(int(event["keys"][0]))
		elif not (event.get("mouse", []) as Array).is_empty():
			art = "Mouse%d" % (int(event["mouse"][0]) + 1)  # the movie's Mouse1..Mouse8 art (M1 the left button)
		out.append({"textField": JournalView.translate("$" + fields[0]), "art": art})
	return out


## The movie's key art export for a DirectInput scan code: letters and digits by name, the rest by the names the
## movie exports ("L-Shift", "Space", "Tab", ...).
static func key_art(scan: int) -> String:
	const NAMES := {0x01: "Esc", 0x0c: "Hyphen", 0x0d: "Equal", 0x0e: "Backspace", 0x0f: "Tab", 0x1a: "Bracketleft",
		0x1b: "Bracketright", 0x1c: "Enter", 0x1d: "L-Ctrl", 0x27: "Semicolon", 0x28: "Quotesingle", 0x29: "Tilde",
		0x2a: "L-Shift", 0x2b: "Backslash", 0x33: "Comma", 0x34: "Period", 0x35: "Slash", 0x36: "R-Shift", 0x38: "L-Alt",
		0x39: "Space", 0x3a: "CapsLock", 0x46: "ScrollLock", 0x9d: "R-Ctrl", 0xb8: "R-Alt", 0xc5: "Pause", 0xc7: "Home",
		0xc8: "Up", 0xc9: "PgUp", 0xcb: "Left", 0xcd: "Right", 0xcf: "End", 0xd0: "Down", 0xd1: "PgDn", 0xd2: "Insert",
		0xd3: "Delete"}
	if NAMES.has(scan):
		return NAMES[scan]
	if scan >= 0x3b and scan <= 0x44:
		return "F%d" % (scan - 0x3a)
	var key: Key = ControlMap.DIK.get(scan, KEY_NONE)
	return OS.get_keycode_string(key) if key != KEY_NONE else "UnknownKey"


func buttons() -> Array:
	match state:
		State.SAVE_LIST:
			# onSaveHighlight: Delete only on a save, not on [NEW SAVE].
			if list_index == 0:
				return [["Esc", JournalView.translate("$Cancel"), 1.0]]
			return [["X", JournalView.translate("$Delete"), 1.0], ["Esc", JournalView.translate("$Cancel"), 1.0]]
		State.LOAD_LIST:
			return [["X", JournalView.translate("$Delete"), 1.0], ["T", JournalView.translate("$CharacterSelection"), 1.0],
				["Esc", JournalView.translate("$Cancel"), 1.0]]
		State.CHARACTERS:
			return [["Esc", JournalView.translate("$Cancel"), 1.0]]
		State.OPTIONS, State.CONTROLS:
			return [["T", JournalView.translate("$Defaults"), 1.0], ["Esc", JournalView.translate("$Cancel"), 1.0]]
	return []


# ---- drawing

func draw(view: JournalView) -> void:
	var page := view.at(PAGE)
	if page.is_empty():
		return
	var shown := _shown_panels()
	var look := JournalView.look()
	look.draw_texts = true
	for name: String in PANELS:
		if not shown.has(name):
			look.hidden[name] = true
	look.named_frames["SystemDivider"] = UiAssets.label_frame(JournalView.SWF, 533, "Right" if state == State.MAIN else "Left")
	# The panels' own contents are drawn below: hide their lists, texts and pictures here.
	for name: String in ["List_mc", "OptionsLists", "ScreenshotHolder", "PlayerInfoText", "ConfirmText", "ButtonRect", "TitleGroup"]:
		look.hidden[name] = true
	UiDraw.character(view, JournalView.SWF, int(page["id"]), page["xform"], Color.WHITE, look)
	var rows := []
	for key: String in CATEGORIES:
		rows.append({"textField": JournalView.translate(key)})
	view.draw_centered_list(PAGE + "/CategoryList_mc/List_mc", rows, main_index)
	var panel_state := state if state != State.CONFIRM else confirm.get("back", State.MAIN) as State
	match panel_state:
		State.SAVE_LIST, State.LOAD_LIST, State.CHARACTERS:
			_draw_saves(view, panel_state)
		State.SETTINGS:
			var srows := []
			for key: String in SETTINGS_LIST:
				srows.append({"textField": JournalView.translate(key)})
			view.draw_list(PAGE + "/SettingsPanel/List_mc", srows, settings_index, 0)
		State.OPTIONS:
			_draw_options(view)
		State.CONTROLS:
			_draw_mappings(view)
		State.QUIT_LIST:
			view.draw_list(PAGE + "/PCQuitPanel/List_mc", [{"textField": JournalView.translate("$Main Menu")},
				{"textField": JournalView.translate("$Desktop")}], quit_index, 0)
	if state == State.CONFIRM:
		var text := view.at(PAGE + "/ConfirmPanel/ConfirmText")
		if not text.is_empty():
			var field := JournalView._child(int(text["id"]), "textField")
			view.text_block(int(field["character"]), (text["xform"] as Transform2D) * UiDraw.matrix(field["matrix"]),
				JournalView.translate(String(confirm["text"])))
		view.draw_buttons(PAGE + "/ConfirmPanel/ButtonRect", [["Enter", JournalView.translate("$Yes"), 1.0],
			["Tab", JournalView.translate("$No"), 1.0]], ["AcceptMouseButton", "CancelMouseButton"])


func _shown_panels() -> Dictionary:
	var out := {"CategoryList_mc": true}
	var panel_state := state if state != State.CONFIRM else confirm.get("back", State.MAIN) as State
	match panel_state:
		State.SAVE_LIST, State.LOAD_LIST, State.CHARACTERS:
			out["SaveLoadPanel"] = true
		State.SETTINGS:
			out["SettingsPanel"] = true
		State.OPTIONS:
			out["OptionsListsPanel"] = true
		State.CONTROLS:
			out["InputMappingPanel"] = true
		State.QUIT_LIST:
			out["PCQuitPanel"] = true
	if state == State.CONFIRM:
		out["ConfirmPanel"] = true
	return out


func _draw_saves(view: JournalView, panel_state: State) -> void:
	var keep := state
	state = panel_state
	var rows := _list_rows()
	state = keep
	view.draw_list(PAGE + "/SaveLoadPanel/List_mc", rows, list_index, list_scroll)
	if panel_state != State.LOAD_LIST and panel_state != State.SAVE_LIST:
		return
	var index := list_index - (1 if panel_state == State.SAVE_LIST else 0)
	if index < 0 or index >= _saves.size():
		return
	var save: Dictionary = _saves[index]
	var holder := view.at(PAGE + "/SaveLoadPanel/ScreenshotHolder")
	var texture := _shot_texture(save)
	if not holder.is_empty() and texture != null:
		var sizer := JournalView._child(int(holder["id"]), "sizer")
		var rect := UiDraw.matrix(sizer["matrix"]) * UiAssets.bounds(JournalView.SWF, int(sizer["character"]))
		view.draw_set_transform_matrix(holder["xform"])
		view.draw_texture_rect(texture, rect, false)
		view.draw_set_transform_matrix(Transform2D.IDENTITY)
	var info := view.at(PAGE + "/SaveLoadPanel/PlayerInfoText")
	if info.is_empty():
		return
	var texts := {"textField": "%s, %s, %s %d" % [save.get("name", ""), save.get("race", ""),
			JournalView.translate("$Level"), int(save.get("level", 1))],
		"PlayTimeText": save.get("play_time", ""), "DateText": save.get("date", "")}
	for name: String in texts:
		var field := JournalView._child(int(info["id"]), name)
		if not field.is_empty():
			UiDraw.field(view, JournalView.SWF, int(field["character"]), (info["xform"] as Transform2D) * UiDraw.matrix(field["matrix"]), String(texts[name]), Color(0, 0, 0, 0), true)


func _shot_texture(save: Dictionary) -> Texture2D:
	var path: String = save.get("path", "")
	if _textures.has(path):
		return _textures[path]
	var tex: Texture2D = null
	var png: PackedByteArray = save.get("shot", PackedByteArray())
	if not png.is_empty():
		var image := Image.new()
		if image.load_png_from_buffer(png) == OK:
			tex = ImageTexture.create_from_image(image)
	_textures[path] = tex
	return tex


## The options list (OptionsList of SettingsOptionItem clips): the label, and a slider, stepper or check box by the
## option's movieType, its value as SettingsOptionItem.value sets it (slider position x 20 of 20, stepper text,
## check box frame value + 1).
func _draw_options(view: JournalView) -> void:
	var path := PAGE + "/OptionsListsPanel/OptionsLists/List_mc"
	var list := view.at(path)
	if list.is_empty():
		return
	var clips := JournalView._entries(int(list["id"]))
	var options: Array = OPTIONS[settings_index]
	var scroll := clampi(option_index - clips.size() + 1, 0, maxi(options.size() - clips.size(), 0))
	var y := UiDraw.matrix(clips[0]["matrix"]).origin.y
	for slot in mini(clips.size(), options.size() - scroll):
		var i := scroll + slot
		var option: Array = options[i]
		var value: Variant = values[settings_index][i]
		var type := int(option[1])
		# SettingsOptionItem: the chosen option at full alpha, the others at 30%.
		var alpha := 1.0 if i == option_index else UNSELECTED_ALPHA
		var row := {"textField": JournalView.translate(option[0]), "fit": ["textField"], "hide": [],
			"colours": {"optionStepper/textField": Color(1, 1, 1, alpha)}}
		if type != 0:
			row["hide"].append("scrollBar")
		if type != 1:
			row["hide"].append("optionStepper")
		else:
			var key: String = (option[3] as Array)[int(value)]
			row["optionStepper/textField"] = DIFFICULTY_TEXT.get(key, JournalView.translate(key))
		if type != 2:
			row["hide"].append("checkBox")
		var clip: Dictionary = clips[slot]
		var item := int(clip["character"])
		view.draw_entry(clip, list["xform"], y, row, "", alpha)
		var local := UiDraw.matrix(clip["matrix"])
		local.origin.y = y
		var item_xform: Transform2D = (list["xform"] as Transform2D) * local
		match type:
			0:
				_draw_slider(view, item, item_xform, float(value), alpha)
			2:
				var box := JournalView._child(item, "checkBox")
				UiDraw.character(view, JournalView.SWF, int(box["character"]), item_xform * UiDraw.matrix(box["matrix"]),
					Color(1, 1, 1, alpha), null, Rect2(), int(value))
		y += JournalView._clip_height(clip)
	view.draw_scroll_arrows(path, scroll, maxi(options.size() - clips.size(), 0))


## A slider (SettingsScrollbar laid on its side): the thumb at `value` of the track.
func _draw_slider(view: JournalView, item: int, item_xform: Transform2D, value: float, alpha: float) -> void:
	var bar := JournalView._child(item, "scrollBar")
	if bar.is_empty():
		return
	var bar_id := int(bar["character"])
	var track := JournalView._child(bar_id, "track")
	var thumb := JournalView._child(bar_id, "thumb")
	if track.is_empty() or thumb.is_empty():
		return
	var track_rect := UiDraw.matrix(track["matrix"]) * UiAssets.bounds(JournalView.SWF, int(track["character"]))
	var thumb_h := UiAssets.bounds(JournalView.SWF, int(thumb["character"])).end.y
	var look := JournalView.look()
	var thumb_at := UiDraw.matrix(thumb["matrix"]).origin
	thumb_at.y = track_rect.position.y + (track_rect.size.y - thumb_h) * clampf(value, 0.0, 1.0)
	look.moves["thumb"] = thumb_at
	UiDraw.character(view, JournalView.SWF, bar_id, item_xform * UiDraw.matrix(bar["matrix"]), Color(1, 1, 1, alpha), look)


## The mapping list (InputMappingList): the event's name and its key's art centred on buttonPlacing, the chosen row
## at full alpha and the others at 30%; arrows while it scrolls.
func _draw_mappings(view: JournalView) -> void:
	var path := PAGE + "/InputMappingPanel/List_mc"
	var list := view.at(path)
	if list.is_empty():
		return
	var clips := JournalView._entries(int(list["id"]))
	var rows := mappings()
	var height := JournalView._border_height(int(list["id"]))
	var fit := 0
	var fit_used := 0.0
	while fit < clips.size() and fit_used + JournalView._clip_height(clips[fit]) <= height + 0.5:
		fit_used += JournalView._clip_height(clips[fit])
		fit += 1
	var max_scroll := maxi(rows.size() - fit, 0)
	var scroll := clampi(mapping_index - fit + 1, 0, max_scroll)
	var y := UiDraw.matrix(clips[0]["matrix"]).origin.y
	for slot in mini(fit, rows.size() - scroll):
		var i := scroll + slot
		var clip: Dictionary = clips[slot]
		var alpha := 1.0 if i == mapping_index else UNSELECTED_ALPHA
		view.draw_entry(clip, list["xform"], y, {"textField": rows[i]["textField"]}, "", alpha)
		var place := JournalView._child(int(clip["character"]), "buttonPlacing")
		var local := UiDraw.matrix(clip["matrix"])
		local.origin.y = y
		var art := UiAssets.exported(JournalView.SWF, String(rows[i]["art"]))
		if art < 0:
			art = UiAssets.exported(JournalView.SWF, "UnknownKey")
		if art >= 0 and not place.is_empty():
			var place_rect := UiDraw.matrix(place["matrix"]) * UiAssets.bounds(JournalView.SWF, int(place["character"]))
			var art_bounds := UiAssets.bounds(JournalView.SWF, art)
			var at_place := Vector2(place_rect.position.x + (place_rect.size.x - art_bounds.size.x) * 0.5 - art_bounds.position.x,
				place_rect.position.y - art_bounds.position.y)
			UiDraw.character(view, JournalView.SWF, art, (list["xform"] as Transform2D) * local * Transform2D(0.0, at_place),
				Color(1, 1, 1, alpha))
		y += JournalView._clip_height(clip)
	view.draw_scroll_arrows(path, scroll, max_scroll)
