extends "gd_test.gd"
# ControlMap (game/control_map.gd): the game's controlmap.txt format (contexts, "!0,Event" references, unbound
# 0xff, key combinations), the InputMap it builds (keys by position, mouse buttons, pad buttons and triggers, the
# menus' Scaleform navigation keys).

const MAP := """// header comment
// Main Gameplay
Forward				0x11				0xff	0xff				1	1	0	0x801
Left Attack/Block	0xff				0x1		0x0009				1	1	1	0x841
Activate			0x12				0xff	0x1000				1	1	1	0x804
Tween Menu			0x0f				0xff	0x2000				1	1	1	0x908
Hotkey1				0x02,0x4f			0xff	0x0004				0	0	0	0x908
Pause				0x1					0xff	0xff				1	1	0	0x8
Multi-Screenshot	0x1d+0xb7,0x9d+0xb7	0xff	0xff				0	0	0

// Menu Mode
Accept		!0,Activate				!0,Activate				0x1000			0	0	0	0x8
Cancel		!0,Tween Menu,!0,Pause	!0,Tween Menu,!0,Pause	0x2000			0	0	0	0x8
Up			!0,Forward				!0,Forward				0x0001			0	0	0	0x8

// Item Menus
LeftEquip	!0,Left Attack/Block	!0,Left Attack/Block		!0,Left Attack/Block		0	0	0	0x8
"""

var _saved := {}


func before_each() -> void:
	for action: StringName in [&"activate", &"tween_menu", &"ui_cancel", &"ui_accept", &"ui_up", &"menu_equip_left",
			&"hotkey_1", &"move_forward"]:
		_saved[action] = InputMap.action_get_events(action) if InputMap.has_action(action) else []


func after_each() -> void:
	for action: StringName in _saved:
		if InputMap.has_action(action):
			InputMap.action_erase_events(action)
			for event: InputEvent in _saved[action]:
				InputMap.action_add_event(action, event)


func test_contexts_and_references() -> void:
	var contexts := ControlMap.parse(MAP)
	expect_eq(contexts.size(), 3, "three contexts")
	expect_eq(contexts[0]["name"], "Main Gameplay", "named by the comment before its first event")
	expect_eq(ControlMap.event_of(contexts, "Main Gameplay", "Hotkey1")["keys"], [0x02, 0x4f], "two keys")
	expect_eq(ControlMap.event_of(contexts, "Main Gameplay", "Forward")["mouse"], [], "0xff is unbound")
	expect_eq(ControlMap.event_of(contexts, "Main Gameplay", "Multi-Screenshot")["keys"], [], "combinations are left out")
	var cancel := ControlMap.event_of(contexts, "Menu Mode", "Cancel")
	expect_eq(cancel["keys"], [0x0f, 0x01], "Cancel is the Tween menu key and Pause")
	expect_eq(cancel["pad"], [0x2000], "and B")
	expect_eq(ControlMap.event_of(contexts, "Item Menus", "LeftEquip")["mouse"], [0x1], "a reference in the mouse column")


func test_apply_builds_the_actions() -> void:
	expect(ControlMap.apply(MAP), "applied")
	expect(_has_key(&"activate", KEY_E), "Activate on E, by position")
	expect(_has_key(&"tween_menu", KEY_TAB), "the Tween menu on Tab")
	expect(_has_key(&"ui_cancel", KEY_TAB) and _has_key(&"ui_cancel", KEY_ESCAPE), "menus cancel with Tab and Esc")
	expect(_has_key(&"ui_accept", KEY_E) and _has_key(&"ui_accept", KEY_ENTER), "accept: E and Scaleform's Enter")
	expect(_has_key(&"ui_up", KEY_W) and _has_key(&"ui_up", KEY_UP), "up: W and the arrow")
	expect(not _has_key(&"activate", KEY_F), "the old binding is gone")
	expect(_has_key(&"hotkey_1", KEY_1) and _has_key(&"hotkey_1", KEY_KP_1), "both keys of a hotkey")
	var mouse := false
	var trigger := false
	for event: InputEvent in InputMap.action_get_events(&"menu_equip_left"):
		mouse = mouse or (event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_RIGHT)
		trigger = trigger or (event is InputEventJoypadMotion and event.axis == JOY_AXIS_TRIGGER_LEFT)
	expect(mouse, "the left hand on the right mouse button")
	expect(trigger, "and the left trigger")
	var press := InputEventKey.new()
	press.physical_keycode = KEY_TAB
	press.pressed = true
	expect(press.is_action_pressed(&"ui_cancel"), "a Tab press cancels")
	var stick := false
	for event: InputEvent in InputMap.action_get_events(&"move_forward"):
		stick = stick or event is InputEventJoypadMotion
	expect(stick, "the sticks stay project.godot's")


func test_no_map_changes_nothing() -> void:
	var before := InputMap.action_get_events(&"activate").size()
	expect(not ControlMap.apply(""), "nothing to apply")
	expect_eq(InputMap.action_get_events(&"activate").size(), before, "the defaults stay")


func _has_key(action: StringName, key: Key) -> bool:
	for event: InputEvent in InputMap.action_get_events(action):
		if event is InputEventKey and (event.physical_keycode == key or event.keycode == key):
			return true
	return false
