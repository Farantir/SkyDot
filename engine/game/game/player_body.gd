# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Game layer's contract for the shell's player object: what GameSession, Place,
# SaveService and the magic effects ask of the player (where it looks, putting it
# somewhere, its speeds, sprinting, being shoved, its PLYR save section). A shell's
# PlayerRig extends this and overrides what its camera and SkydotPlayer do; the base
# stores the values and does nothing else, so it is also the headless stand-in tests
# hand to a session or a Place.
class_name PlayerBody
extends RefCounted

## Where the player looks, radians (yaw about the up axis).
var yaw := 0.0
var pitch := 0.0
## What load_section decoded, for place_at.
var loaded_position := Vector3.ZERO
var loaded_yaw := 0.0
var loaded_pitch := 0.0
## Where the player was last put (the base only remembers it; it is what save_section writes).
var position := Vector3.ZERO
## Flying through everything instead of walking. A property over is_flying()/set_flying(), which
## a subclass overrides (a subclass cannot redeclare this member).
var flying: bool:
	get:
		return is_flying()
	set(value):
		set_flying(value)

var _flying := false
var _walk_speed := 0.0
var _run_speed := 0.0
var _sprint_speed := 0.0
var _swim_speed := 0.0
var _swim_walk_speed := 0.0
var _jump_height := 0.0
var _back_walk_ratio := 1.0
var _back_run_ratio := 1.0


func is_flying() -> bool:
	return _flying


func set_flying(on: bool) -> void:
	_flying = on


## Put the player at `position` (a camera position) looking along `new_yaw` and `new_pitch`.
func place_at(new_position: Vector3, new_yaw: float, new_pitch: float) -> void:
	position = new_position
	yaw = new_yaw
	pitch = new_pitch


## How far the player sees, in metres (the camera's far plane).
func set_view_distance(_metres: float) -> void:
	pass


## What the PLYR section of a save holds of the player.
func save_section() -> PackedByteArray:
	return var_to_bytes({"position": position, "yaw": yaw, "pitch": pitch})


## Decode a section (changes nothing but loaded_*, which place_at is then given). False if
## it is not a player's.
func load_section(bytes: PackedByteArray) -> bool:
	if bytes.is_empty():
		return true
	var state = bytes_to_var(bytes)
	if typeof(state) != TYPE_DICTIONARY or typeof(state.get("position")) != TYPE_VECTOR3 \
			or typeof(state.get("yaw")) != TYPE_FLOAT or typeof(state.get("pitch")) != TYPE_FLOAT:
		return false
	loaded_position = state["position"]
	loaded_yaw = state["yaw"]
	loaded_pitch = state["pitch"]
	return true


## Sprinting at the sprint gait; the session drains Stamina meanwhile.
func is_sprinting() -> bool:
	return false


## Whether sprinting is allowed (not with Stamina out).
func set_sprint_allowed(_allowed: bool) -> void:
	pass


## A shove: add `velocity` (m/s) to the player. False when there is no body to push (the base).
func impulse(_velocity: Vector3) -> bool:
	return false


## The speeds and jump, for the game: the data's movement types, slow time (metres per second).
func set_walk_speed(value: float) -> void:
	_walk_speed = value


func get_walk_speed() -> float:
	return _walk_speed


func set_run_speed(value: float) -> void:
	_run_speed = value


func get_run_speed() -> float:
	return _run_speed


func set_sprint_speed(value: float) -> void:
	_sprint_speed = value


func get_sprint_speed() -> float:
	return _sprint_speed


func set_swim_speed(value: float) -> void:
	_swim_speed = value


func get_swim_speed() -> float:
	return _swim_speed


func set_swim_walk_speed(value: float) -> void:
	_swim_walk_speed = value


func get_swim_walk_speed() -> float:
	return _swim_walk_speed


func set_jump_height(value: float) -> void:
	_jump_height = value


func get_jump_height() -> float:
	return _jump_height


## Walking and running backwards, as a fraction of the forward speed.
func set_back_walk_ratio(value: float) -> void:
	_back_walk_ratio = value


func get_back_walk_ratio() -> float:
	return _back_walk_ratio


func set_back_run_ratio(value: float) -> void:
	_back_run_ratio = value


func get_back_run_ratio() -> float:
	return _back_run_ratio
