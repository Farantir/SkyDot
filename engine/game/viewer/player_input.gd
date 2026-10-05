# SPDX-License-Identifier: GPL-3.0-or-later
#
# Keyboard and mouse to the player. What the player does (walk, fly, sprint,
# jump, look) goes to the PlayerRig here; every other key is a command the
# viewer carries out (the `command` signal). Keys are InputMap actions
# declared in project.godot, so a gamepad or VR shell maps its own events to
# the same names.
#
# The mouse looks (Esc releases it, a click captures it again; the right
# button looks while released). WASD to move, Shift sprints, Ctrl walks, Space
# jumps (and swims up), Q and E fly down and up.
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
const SHOW_PATH := &"show_path"
const LOD_DETAIL_DOWN := &"lod_detail_down"
const LOD_DETAIL_UP := &"lod_detail_up"
const RADIUS_DOWN := &"radius_down"
const RADIUS_UP := &"radius_up"
const CYCLE_MSAA := &"cycle_msaa"
const TOGGLE_FLY := &"toggle_fly"
const COMMANDS: Array[StringName] = [ACTIVATE, QUICK_SAVE, QUICK_LOAD, TAKE_SHOT, SHOW_JOURNAL,
	SHOW_POSITION, TOGGLE_PRELOAD, CHANGE_TIME, INSPECT_ACTOR, NEXT_WEATHER, TOGGLE_NAVMESH,
	SHOW_PATH, LOD_DETAIL_DOWN, LOD_DETAIL_UP, RADIUS_DOWN, RADIUS_UP, CYCLE_MSAA, TOGGLE_FLY]

const LOOK_PER_PIXEL := 0.004  # radians
const PITCH_LIMIT := 1.5  # radians up or down

## An action in COMMANDS was pressed; `modifier` is true with Shift held (the
## backwards or the forced variant of some).
signal command(action: StringName, modifier: bool)

## Off for runs that drive themselves: everything here is ignored and the
## player stands still.
var enabled := true
## m/s while flying.
var fly_speed := 3.0
var _rig: PlayerRig


func _init(rig: PlayerRig) -> void:
	_rig = rig


## Capture the mouse for looking, unless there is no window or no one at the
## keyboard.
func capture_mouse() -> void:
	if enabled and DisplayServer.get_name() != "headless":
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


## Every frame: what is held goes to the player.
func apply() -> void:
	var player := _rig.player
	if not enabled:
		player.set_input(Vector2.ZERO, 0.0, SkydotPlayer.RUN)
		player.set_look(_rig.yaw, _rig.pitch)
		return
	var move := Vector2.ZERO
	if Input.is_action_pressed(MOVE_FORWARD): move.y += 1
	if Input.is_action_pressed(MOVE_BACK): move.y -= 1
	if Input.is_action_pressed(MOVE_LEFT): move.x -= 1
	if Input.is_action_pressed(MOVE_RIGHT): move.x += 1
	var vertical := 0.0
	if Input.is_action_pressed(MOVE_UP) or (Input.is_action_pressed(JUMP) and player.is_swimming()):
		vertical += 1
	if Input.is_action_pressed(MOVE_DOWN):
		vertical -= 1
	var gait := SkydotPlayer.RUN
	if Input.is_action_pressed(SPRINT):
		gait = SkydotPlayer.SPRINT
	elif Input.is_action_pressed(WALK):
		gait = SkydotPlayer.WALK
	player.fly_speed = fly_speed
	player.set_input(move, vertical, gait)
	player.set_look(_rig.yaw, _rig.pitch)


func _unhandled_input(event: InputEvent) -> void:
	if not enabled:
		return
	var captured := Input.mouse_mode == Input.MOUSE_MODE_CAPTURED
	if event is InputEventMouseMotion and (captured or Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)):
		_rig.apply_look(_rig.yaw - event.relative.x * LOOK_PER_PIXEL,
			clamp(_rig.pitch - event.relative.y * LOOK_PER_PIXEL, -PITCH_LIMIT, PITCH_LIMIT))
	elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT and not captured:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	elif event.is_action_pressed(RELEASE_MOUSE):
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	elif event.is_action_pressed(JUMP):
		_rig.player.jump()
	else:
		for action in COMMANDS:
			if event.is_action_pressed(action):
				command.emit(action, event is InputEventWithModifiers and event.shift_pressed)
				return
