# SPDX-License-Identifier: GPL-3.0-or-later
#
# Keyboard, mouse and gamepad to the player. What the player does (walk, fly, sprint,
# jump, look) goes to the PlayerRig here; every other key is a command the
# viewer carries out (the `command` signal). Keys are InputMap actions
# declared in project.godot, so a gamepad or VR shell maps its own events to
# the same names.
#
# The mouse looks (Esc releases it, a click captures it again; the right
# button looks while released). WASD to move, Shift sprints, Ctrl walks, Space
# jumps (and swims up), Q and E fly down and up.
#
# A gamepad (Skyrim SE's Xbox layout; a Steam Deck's controls are the same
# events) is bound in project.godot: the left stick moves (analog: under half
# deflection walks unless sprint is held), the right stick looks (turn rate =
# look_speed * deflection squared, so small pushes aim finely), A activates, Y
# jumps, LB sprints (held), Start shows the journal. `uses_gamepad` says which
# device was used last, for the prompt's key label.
class_name PlayerInput
extends Node

# Held: sampled every frame by apply().
const MOVE_FORWARD := &"move_forward"
const MOVE_BACK := &"move_back"
const MOVE_LEFT := &"move_left"
const MOVE_RIGHT := &"move_right"
const MOVE_UP := &"move_up"
const MOVE_DOWN := &"move_down"
const SPRINT := &"sprint"
const LOOK_LEFT := &"look_left"
const LOOK_RIGHT := &"look_right"
const LOOK_UP := &"look_up"
const LOOK_DOWN := &"look_down"
const WALK := &"walk"
# Pressed once: the player's own.
const JUMP := &"jump"
const RELEASE_MOUSE := &"release_mouse"
# Pressed once: commands for the viewer, sent with `command`.
const ACTIVATE := &"activate"
const QUICK_SAVE := &"quick_save"
const QUICK_LOAD := &"quick_load"
const TAKE_SHOT := &"take_shot"
const SHOW_JOURNAL := &"show_journal"
const SHOW_POSITION := &"show_position"
const TOGGLE_PRELOAD := &"toggle_preload"
const CHANGE_TIME := &"change_time"
const INSPECT_ACTOR := &"inspect_actor"
const NEXT_WEATHER := &"next_weather"
const TOGGLE_NAVMESH := &"toggle_navmesh"
const TOGGLE_PERF := &"toggle_perf"
const SHOW_PATH := &"show_path"
const LOD_DETAIL_DOWN := &"lod_detail_down"
const LOD_DETAIL_UP := &"lod_detail_up"
const RADIUS_DOWN := &"radius_down"
const RADIUS_UP := &"radius_up"
const CYCLE_MSAA := &"cycle_msaa"
const TOGGLE_FLY := &"toggle_fly"
const INVENTORY := &"inventory"
const MAGIC := &"magic"
const READY_WEAPON := &"ready_weapon"
const POWER := &"power"
# Held: the attack buttons go down and up (`attack_changed` for the right hand,
# `attack_left_changed` for the left: a spell in the left hand; blocking otherwise).
const ATTACK_RIGHT := &"attack_right"
const ATTACK_LEFT := &"attack_left"
const COMMANDS: Array[StringName] = [ACTIVATE, QUICK_SAVE, QUICK_LOAD, TAKE_SHOT, SHOW_JOURNAL,
	SHOW_POSITION, TOGGLE_PRELOAD, CHANGE_TIME, INSPECT_ACTOR, NEXT_WEATHER, TOGGLE_NAVMESH,
	TOGGLE_PERF, SHOW_PATH, LOD_DETAIL_DOWN, LOD_DETAIL_UP, RADIUS_DOWN, RADIUS_UP, CYCLE_MSAA, TOGGLE_FLY, INVENTORY, MAGIC, READY_WEAPON, POWER]

const LOOK_PER_PIXEL := 0.004  # radians
const PITCH_LIMIT := 1.5  # radians up or down
## A left stick pushed less than this far walks.
## Xbox layout button names by Godot's JoyButton index.
const PAD_LABELS := ["A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "LB", "RB"]
const WALK_DEFLECTION := 0.5
## The stick turns the view up and down this much as fast as sideways.
const PITCH_SPEED_FACTOR := 0.7

## An action in COMMANDS was pressed; `modifier` is true with Shift held (the
## backwards or the forced variant of some).
signal command(action: StringName, modifier: bool)
## The attack button went down (true) or up (false). A mouse click does not
## count while the mouse is released: it captures the mouse.
signal attack_changed(pressed: bool)
## The left hand's attack button went down (true) or up (false).
signal attack_left_changed(pressed: bool)

## Set while a menu has the keyboard: the player stands still and the
## keys are not commands.
var blocked := false
## Paralysed (a magic effect): like `blocked`, set by the session's state each frame.
var paralyzed := false
## Off for runs that drive themselves: everything here is ignored and the
## player stands still.
var enabled := true
## m/s while flying.
var fly_speed := 3.0
## rad/s at full deflection of the right stick (--stick-look-speed).
var look_speed := 3.0
## Pushing the right stick up looks down (--invert-look-y).
var invert_look_y := false
## The last input came from a gamepad (else keyboard or mouse).
var uses_gamepad := false
var _rig: PlayerRig
var _attacking := false
var _attacking_left := false


func _init(rig: PlayerRig) -> void:
	_rig = rig


## Capture the mouse for looking, unless there is no window or no one at the
## keyboard.
func capture_mouse() -> void:
	if enabled and DisplayServer.get_name() != "headless":
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


## The label of what `action` is bound to on the device used last: "F" or "A".
func key_label(action: StringName) -> String:
	for event in InputMap.action_get_events(action):
		if uses_gamepad and event is InputEventJoypadButton:
			return PAD_LABELS[event.button_index] if event.button_index < PAD_LABELS.size() else "Pad"
		if not uses_gamepad and event is InputEventKey:
			return OS.get_keycode_string(event.keycode if event.keycode != 0 else event.physical_keycode)
	return ""


## Every frame: what is held goes to the player, and the right stick turns the
## view by `delta` seconds' worth.
func apply(delta: float) -> void:
	var player := _rig.player
	if not enabled or blocked or paralyzed:
		player.set_input(Vector2.ZERO, 0.0, SkydotPlayer.RUN)
		player.set_look(_rig.yaw, _rig.pitch)
		return
	var move := Input.get_vector(MOVE_LEFT, MOVE_RIGHT, MOVE_BACK, MOVE_FORWARD)
	var look := Input.get_vector(LOOK_LEFT, LOOK_RIGHT, LOOK_DOWN, LOOK_UP)
	if look != Vector2.ZERO:
		var turn := look * look.length() * look_speed * delta  # squared response
		_rig.apply_look(_rig.yaw - turn.x,
			clampf(_rig.pitch + turn.y * PITCH_SPEED_FACTOR * (-1.0 if invert_look_y else 1.0),
				-PITCH_LIMIT, PITCH_LIMIT))
	var vertical := 0.0
	if Input.is_action_pressed(MOVE_UP) or (Input.is_action_pressed(JUMP) and player.is_swimming()):
		vertical += 1
	if Input.is_action_pressed(MOVE_DOWN):
		vertical -= 1
	var gait := SkydotPlayer.RUN
	if Input.is_action_pressed(SPRINT):
		gait = SkydotPlayer.SPRINT
	elif Input.is_action_pressed(WALK) or (move != Vector2.ZERO and move.length() < WALK_DEFLECTION):
		gait = SkydotPlayer.WALK
	player.fly_speed = fly_speed
	player.set_input(move, vertical, gait)
	player.set_look(_rig.yaw, _rig.pitch)


func _input(event: InputEvent) -> void:
	if event is InputEventJoypadButton or (event is InputEventJoypadMotion and absf(event.axis_value) > 0.3):
		uses_gamepad = true
	elif event is InputEventKey or event is InputEventMouseButton or event is InputEventMouseMotion:
		uses_gamepad = false


## Emit `attack_changed` when the button changes (a trigger sends events all
## the time it is held).
func _set_attacking(down: bool) -> void:
	if down != _attacking:
		_attacking = down
		attack_changed.emit(down)


func _set_attacking_left(down: bool) -> void:
	if down != _attacking_left:
		_attacking_left = down
		attack_left_changed.emit(down)


func _unhandled_input(event: InputEvent) -> void:
	if not enabled or blocked:
		# A menu opened under a held button: letting go still counts.
		if enabled and _attacking and event.is_action(ATTACK_RIGHT) and not event.is_pressed():
			_set_attacking(false)
		if enabled and _attacking_left and event.is_action(ATTACK_LEFT) and not event.is_pressed():
			_set_attacking_left(false)
		return
	var captured := Input.mouse_mode == Input.MOUSE_MODE_CAPTURED
	if event is InputEventMouseMotion and (captured or Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)):
		_rig.apply_look(_rig.yaw - event.relative.x * LOOK_PER_PIXEL,
			clamp(_rig.pitch - event.relative.y * LOOK_PER_PIXEL, -PITCH_LIMIT, PITCH_LIMIT))
	elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT and not captured:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	elif event.is_action(ATTACK_RIGHT) and (captured or not event is InputEventMouseButton):
		_set_attacking(event.is_pressed())
	elif event.is_action(ATTACK_LEFT) and (captured or not event is InputEventMouseButton):
		_set_attacking_left(event.is_pressed())
	elif event.is_action_pressed(RELEASE_MOUSE):
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	elif event.is_action_pressed(JUMP):
		_rig.player.jump()
	else:
		for action in COMMANDS:
			if event.is_action_pressed(action):
				command.emit(action, event is InputEventWithModifiers and event.shift_pressed)
				return
