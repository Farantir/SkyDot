# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game's key bindings, from its own control map (interface/controls/pc/controlmap.txt, a `ui` text asset of the
# pack): which keyboard key (DirectInput scan code), mouse button and gamepad button (XInput mask) raises each user
# event, per input context (Main Gameplay, Menu Mode, Item Menus, Favorites menu, ...). Blank lines start a context,
# the comment before its first event names it; "!0,Forward" is the Forward event of context 0 in the same column;
# 0xff is unbound; key combinations ("0x1d+0xb7") are left out.
#
# `apply` rebuilds the InputMap actions the game's events stand for (ACTIONS): their keys, mouse buttons and pad
# buttons come from the control map; analog sticks stay as project.godot binds them. The menus' Up/Down/Left/Right/
# Accept/Cancel are Godot's ui_* actions, plus the keys the movies' Scaleform focus handling turns into navigation
# by itself (arrows, Enter: NAVIGATION). Actions not named here (the viewer's debug keys) are left alone, and
# without a control map project.godot's defaults (the same bindings) stay.
class_name ControlMap
extends RefCounted

const PATH := "interface/controls/pc/controlmap.txt"

## action -> [context name, event name]. Keys are bound by position (scan codes, as the game reads them).
const ACTIONS := {
	&"move_forward": ["Main Gameplay", "Forward"],
	&"move_back": ["Main Gameplay", "Back"],
	&"move_left": ["Main Gameplay", "Strafe Left"],
	&"move_right": ["Main Gameplay", "Strafe Right"],
	&"attack_left": ["Main Gameplay", "Left Attack/Block"],
	&"attack_right": ["Main Gameplay", "Right Attack/Block"],
	&"activate": ["Main Gameplay", "Activate"],
	&"ready_weapon": ["Main Gameplay", "Ready Weapon"],
	&"tween_menu": ["Main Gameplay", "Tween Menu"],
	&"jump": ["Main Gameplay", "Jump"],
	&"sprint": ["Main Gameplay", "Sprint"],
	&"walk": ["Main Gameplay", "Run"],  # held with always-run on (the default): walks
	&"power": ["Main Gameplay", "Shout"],
	&"favorites": ["Main Gameplay", "Favorites"],
	&"hotkey_1": ["Main Gameplay", "Hotkey1"],
	&"hotkey_2": ["Main Gameplay", "Hotkey2"],
	&"hotkey_3": ["Main Gameplay", "Hotkey3"],
	&"hotkey_4": ["Main Gameplay", "Hotkey4"],
	&"hotkey_5": ["Main Gameplay", "Hotkey5"],
	&"hotkey_6": ["Main Gameplay", "Hotkey6"],
	&"hotkey_7": ["Main Gameplay", "Hotkey7"],
	&"hotkey_8": ["Main Gameplay", "Hotkey8"],
	&"quick_save": ["Main Gameplay", "Quicksave"],
	&"quick_load": ["Main Gameplay", "Quickload"],
	&"wait": ["Main Gameplay", "Wait"],
	&"journal": ["Main Gameplay", "Journal"],
	&"pause": ["Main Gameplay", "Pause"],
	&"inventory": ["Main Gameplay", "Quick Inventory"],
	&"magic": ["Main Gameplay", "Quick Magic"],
	&"skills": ["Main Gameplay", "Quick Stats"],
	&"map": ["Main Gameplay", "Quick Map"],
	&"ui_up": ["Menu Mode", "Up"],
	&"ui_down": ["Menu Mode", "Down"],
	&"ui_left": ["Menu Mode", "Left"],
	&"ui_right": ["Menu Mode", "Right"],
	&"ui_accept": ["Menu Mode", "Accept"],
	&"ui_cancel": ["Menu Mode", "Cancel"],
	&"menu_equip_left": ["Item Menus", "LeftEquip"],
	&"menu_drop": ["Item Menus", "XButton"],
	&"menu_favorite": ["Item Menus", "YButton"],
	&"map_up": ["Map Menu", "Up"],
	&"map_down": ["Map Menu", "Down"],
	&"map_left": ["Map Menu", "Left"],
	&"map_right": ["Map Menu", "Right"],
	&"map_cancel": ["Map Menu", "Cancel"],
	&"map_click": ["Map Menu", "Click"],
	&"map_place_marker": ["Map Menu", "PlacePlayerMarker"],
	&"map_player_position": ["Map Menu", "PlayerPosition"],
	&"map_local": ["Map Menu", "LocalMap"],
	&"map_journal": ["Map Menu", "Journal"],
	&"menu_charge": ["Inventory", "ChargeItem"],
}

## Keys Scaleform's focus handling turns into navigation codes in every menu (gfx.ui.NavigationCode: UP, DOWN,
## LEFT, RIGHT, ENTER), besides the control map's Menu Mode events.
const NAVIGATION := {
	&"ui_up": [KEY_UP],
	&"ui_down": [KEY_DOWN],
	&"ui_left": [KEY_LEFT],
	&"ui_right": [KEY_RIGHT],
	&"ui_accept": [KEY_ENTER, KEY_KP_ENTER],
}

## DirectInput scan code -> Godot key (by position).
const DIK := {
	0x01: KEY_ESCAPE, 0x02: KEY_1, 0x03: KEY_2, 0x04: KEY_3, 0x05: KEY_4, 0x06: KEY_5, 0x07: KEY_6, 0x08: KEY_7,
	0x09: KEY_8, 0x0a: KEY_9, 0x0b: KEY_0, 0x0c: KEY_MINUS, 0x0d: KEY_EQUAL, 0x0e: KEY_BACKSPACE, 0x0f: KEY_TAB,
	0x10: KEY_Q, 0x11: KEY_W, 0x12: KEY_E, 0x13: KEY_R, 0x14: KEY_T, 0x15: KEY_Y, 0x16: KEY_U, 0x17: KEY_I,
	0x18: KEY_O, 0x19: KEY_P, 0x1a: KEY_BRACKETLEFT, 0x1b: KEY_BRACKETRIGHT, 0x1c: KEY_ENTER, 0x1d: KEY_CTRL,
	0x1e: KEY_A, 0x1f: KEY_S, 0x20: KEY_D, 0x21: KEY_F, 0x22: KEY_G, 0x23: KEY_H, 0x24: KEY_J, 0x25: KEY_K,
	0x26: KEY_L, 0x27: KEY_SEMICOLON, 0x28: KEY_APOSTROPHE, 0x29: KEY_QUOTELEFT, 0x2a: KEY_SHIFT, 0x2b: KEY_BACKSLASH,
	0x2c: KEY_Z, 0x2d: KEY_X, 0x2e: KEY_C, 0x2f: KEY_V, 0x30: KEY_B, 0x31: KEY_N, 0x32: KEY_M, 0x33: KEY_COMMA,
	0x34: KEY_PERIOD, 0x35: KEY_SLASH, 0x36: KEY_SHIFT, 0x37: KEY_KP_MULTIPLY, 0x38: KEY_ALT, 0x39: KEY_SPACE,
	0x3a: KEY_CAPSLOCK, 0x3b: KEY_F1, 0x3c: KEY_F2, 0x3d: KEY_F3, 0x3e: KEY_F4, 0x3f: KEY_F5, 0x40: KEY_F6,
	0x41: KEY_F7, 0x42: KEY_F8, 0x43: KEY_F9, 0x44: KEY_F10, 0x45: KEY_NUMLOCK, 0x46: KEY_SCROLLLOCK, 0x47: KEY_KP_7,
	0x48: KEY_KP_8, 0x49: KEY_KP_9, 0x4a: KEY_KP_SUBTRACT, 0x4b: KEY_KP_4, 0x4c: KEY_KP_5, 0x4d: KEY_KP_6,
	0x4e: KEY_KP_ADD, 0x4f: KEY_KP_1, 0x50: KEY_KP_2, 0x51: KEY_KP_3, 0x52: KEY_KP_0, 0x53: KEY_KP_PERIOD,
	0x57: KEY_F11, 0x58: KEY_F12, 0x9c: KEY_KP_ENTER, 0x9d: KEY_CTRL, 0xb5: KEY_KP_DIVIDE, 0xb7: KEY_PRINT,
	0xb8: KEY_ALT, 0xc5: KEY_PAUSE, 0xc7: KEY_HOME, 0xc8: KEY_UP, 0xc9: KEY_PAGEUP, 0xcb: KEY_LEFT, 0xcd: KEY_RIGHT,
	0xcf: KEY_END, 0xd0: KEY_DOWN, 0xd1: KEY_PAGEDOWN, 0xd2: KEY_INSERT, 0xd3: KEY_DELETE,
}

## The control map's mouse ids -> Godot buttons (0xa, the mouse's movement, is not a button).
const MOUSE := {0x0: MOUSE_BUTTON_LEFT, 0x1: MOUSE_BUTTON_RIGHT, 0x2: MOUSE_BUTTON_MIDDLE,
	0x8: MOUSE_BUTTON_WHEEL_UP, 0x9: MOUSE_BUTTON_WHEEL_DOWN}

## XInput button masks -> Godot pad buttons.
const PAD := {0x0001: JOY_BUTTON_DPAD_UP, 0x0002: JOY_BUTTON_DPAD_DOWN, 0x0004: JOY_BUTTON_DPAD_LEFT,
	0x0008: JOY_BUTTON_DPAD_RIGHT, 0x0010: JOY_BUTTON_START, 0x0020: JOY_BUTTON_BACK, 0x0040: JOY_BUTTON_LEFT_STICK,
	0x0080: JOY_BUTTON_RIGHT_STICK, 0x0100: JOY_BUTTON_LEFT_SHOULDER, 0x0200: JOY_BUTTON_RIGHT_SHOULDER,
	0x1000: JOY_BUTTON_A, 0x2000: JOY_BUTTON_B, 0x4000: JOY_BUTTON_X, 0x8000: JOY_BUTTON_Y}

## The control map's pseudo masks for the triggers -> Godot axes (0x000b and 0x000c, the sticks, stay project.godot's).
const TRIGGERS := {0x0009: JOY_AXIS_TRIGGER_LEFT, 0x000a: JOY_AXIS_TRIGGER_RIGHT}

const COLUMNS := ["keys", "mouse", "pad"]


## The control map as contexts: [{name, events: {event: {keys: [], mouse: [], pad: []}}}], references resolved.
static func parse(text: String) -> Array:
	var contexts: Array = []
	var current := {"name": "", "events": {}, "raw": {}}
	for line in text.split("\n"):
		var stripped := line.strip_edges()
		if stripped == "":
			if not current["raw"].is_empty():
				contexts.append(current)
				current = {"name": "", "events": {}, "raw": {}}
			continue
		if stripped.begins_with("//"):
			if current["raw"].is_empty():
				current["name"] = stripped.trim_prefix("//").strip_edges()
			continue
		var fields: Array[String] = []
		for field in line.split("\t", false):
			if field.strip_edges() != "":
				fields.append(field.strip_edges())
		if fields.size() < 4:
			continue
		current["raw"][fields[0]] = [fields[1], fields[2], fields[3]]
	if not current["raw"].is_empty():
		contexts.append(current)
	for context: Dictionary in contexts:
		for event: String in context["raw"]:
			var entry := {}
			for column in 3:
				entry[COLUMNS[column]] = _values(contexts, context["raw"][event][column], column)
			context["events"][event] = entry
	for context: Dictionary in contexts:
		context.erase("raw")
	return contexts


## One column of an event: "0x11", "0x02,0x4f", "!0,Tween Menu,!0,Pause"; 0xff and combinations are dropped.
static func _values(contexts: Array, text: String, column: int) -> Array[int]:
	var out: Array[int] = []
	var tokens := text.split(",")
	var i := 0
	while i < tokens.size():
		var token := tokens[i].strip_edges()
		if token.begins_with("!") and i + 1 < tokens.size():
			var index := token.substr(1).to_int()
			var name := tokens[i + 1].strip_edges()
			if index < contexts.size() and contexts[index]["raw"].has(name):
				out.append_array(_values(contexts, contexts[index]["raw"][name][column], column))
			i += 2
			continue
		if token.is_valid_hex_number(true) and not token.contains("+"):
			var value := token.hex_to_int()
			if value != 0xff:
				out.append(value)
		i += 1
	return out


## The event `event` of context `context` ({keys, mouse, pad}); empty when the map has none.
static func event_of(contexts: Array, context: String, event: String) -> Dictionary:
	for entry: Dictionary in contexts:
		if entry["name"] == context:
			return entry["events"].get(event, {})
	return {}


## Rebind ACTIONS from the pack's control map. False (nothing changed) when the pack has none.
static func apply_from_pack() -> bool:
	var text := UiAssets.file(PATH, PATH.get_file()).get_string_from_utf8()
	return apply(text)


## Rebind ACTIONS from control map text. False (nothing changed) when it holds no Main Gameplay context.
static func apply(text: String) -> bool:
	var contexts := parse(text)
	if event_of(contexts, "Main Gameplay", "Forward").is_empty():
		return false
	for action: StringName in ACTIONS:
		var source: Array = ACTIONS[action]
		var event := event_of(contexts, source[0], source[1])
		if event.is_empty():
			continue
		if not InputMap.has_action(action):
			InputMap.add_action(action, 0.2)
		for old: InputEvent in InputMap.action_get_events(action):
			var trigger: bool = old is InputEventJoypadMotion and (old as InputEventJoypadMotion).axis in [JOY_AXIS_TRIGGER_LEFT, JOY_AXIS_TRIGGER_RIGHT]
			if old is InputEventKey or old is InputEventMouseButton or old is InputEventJoypadButton or trigger:
				InputMap.action_erase_event(action, old)
		for event_new in events_for(event):
			InputMap.action_add_event(action, event_new)
		for keycode: Key in NAVIGATION.get(action, []):
			InputMap.action_add_event(action, _key(keycode))
	return true


## Godot events for one control map event.
static func events_for(event: Dictionary) -> Array[InputEvent]:
	var out: Array[InputEvent] = []
	for code: int in event.get("keys", []):
		if DIK.has(code):
			out.append(_key(DIK[code]))
	for id: int in event.get("mouse", []):
		if MOUSE.has(id):
			var button := InputEventMouseButton.new()
			button.button_index = MOUSE[id]
			out.append(button)
	for mask: int in event.get("pad", []):
		if PAD.has(mask):
			var pad := InputEventJoypadButton.new()
			pad.button_index = PAD[mask]
			out.append(pad)
		elif TRIGGERS.has(mask):
			var axis := InputEventJoypadMotion.new()
			axis.axis = TRIGGERS[mask]
			axis.axis_value = 1.0
			out.append(axis)
	return out


static func _key(keycode: Key) -> InputEventKey:
	var key := InputEventKey.new()
	key.physical_keycode = keycode
	return key
