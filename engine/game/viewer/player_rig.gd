# SPDX-License-Identifier: GPL-3.0-or-later
#
# The camera and the player it follows, and where they look. The player is
# SkydotPlayer (walks with collision, or flies); the camera sits at its eyes
# and faces where `yaw` and `pitch` say. Input only changes those two; the
# places put the rig somewhere with place_at (the game's player contract).
class_name PlayerRig
extends RefCounted


## The player fell below the world and was put back, flying.
signal fell_through

var camera: Camera3D
var player: SkydotPlayer  # the camera follows its eyes
var yaw := 0.0  # radians, about the up axis
var pitch := 0.0
## What load_section decoded, for place_at.
var loaded_position := Vector3.ZERO
var loaded_yaw := 0.0
var loaded_pitch := 0.0
var _last_ground := Vector3.ZERO  # where the player last stood
var _ground_check := 0  # frames until checking the player is not under the land


## `image_space` grades what the camera sees (always on). `head` is the camera
## to use (a VR shell's XRCamera3D); a plain Camera3D if null.
func _init(settings: ViewerSettings, image_space: SkydotImageSpace, head: Camera3D = null) -> void:
	camera = head if head != null else Camera3D.new()
	camera.near = 0.05
	# The scene is drawn in the game's gamma space; this grades it as the
	# game's image space does and hands Godot linear colour.
	camera.compositor = Compositor.new()
	camera.compositor.compositor_effects = [image_space]
	if settings.has_fov:  # vertical, degrees
		camera.fov = settings.fov
	player = SkydotPlayer.new()
	player.name = "player"
	player.eye_height = GameSession.EYE_HEIGHT
	player.fly = settings.fly


## GameSession's player contract (game/game_session.gd).
##
## Put the camera at `position` (a camera position) looking along `new_yaw` and
## `new_pitch`, with the player's feet below it.
func place_at(position: Vector3, new_yaw: float, new_pitch: float) -> void:
	camera.position = position
	apply_look(new_yaw, new_pitch)
	# A little above the spot, so feet placed on a floor do not start in it.
	player.teleport(position - Vector3(0, GameSession.EYE_HEIGHT - 0.05, 0))
	_last_ground = player.global_position
	_ground_check = 3


## Flying through everything, or walking.
var flying: bool:
	get:
		return player.fly
	set(value):
		player.fly = value


## Sprinting at the sprint gait (the session drains Stamina meanwhile) and
## whether it is allowed (not with Stamina out): GameSession's optional part
## of the player contract, ADR 0003.
func is_sprinting() -> bool:
	return player.is_sprinting()


func set_sprint_allowed(allowed: bool) -> void:
	player.set_sprint_allowed(allowed)


func set_view_distance(metres: float) -> void:
	camera.far = metres


## What the PLYR section holds of the player: where the camera is and looks.
func save_section() -> PackedByteArray:
	return var_to_bytes({"position": camera.position, "yaw": yaw, "pitch": pitch})


## Decode a section (changes nothing): false if it is not a player's. The
## values are in loaded_* for place_at.
func load_section(bytes: PackedByteArray) -> bool:
	var state = bytes_to_var(bytes)
	if typeof(state) != TYPE_DICTIONARY or typeof(state.get("position")) != TYPE_VECTOR3 \
			or typeof(state.get("yaw")) != TYPE_FLOAT or typeof(state.get("pitch")) != TYPE_FLOAT:
		return false
	loaded_position = state["position"]
	loaded_yaw = state["yaw"]
	loaded_pitch = state["pitch"]
	return true


func apply_look(new_yaw: float, new_pitch: float) -> void:
	yaw = new_yaw
	pitch = new_pitch
	camera.rotation = Vector3(pitch, yaw, 0)


## The camera goes to the player's eyes.
func follow() -> void:
	camera.global_position = player.get_eye_position()


## V: walk or fly. Returns true when flying now.
func toggle_fly() -> bool:
	player.fly = not player.fly
	if not player.fly:
		_last_ground = player.global_position
	return player.fly


## While walking: put the feet on the land a spot given in game units may be
## under, remember where the player last stood and catch it if it falls
## through the world.
func track() -> void:
	if player.hold or player.fly:
		return
	if _ground_check > 0:
		_ground_check -= 1
		if _ground_check == 0:
			_lift_onto_land()
	if player.is_on_floor():
		_last_ground = player.global_position
	elif _last_ground.y - player.global_position.y > 200.0:
		player.teleport(_last_ground)
		player.fly = true
		fell_through.emit()


func _lift_onto_land() -> void:
	var feet := player.global_position
	var query := PhysicsRayQueryParameters3D.create(feet + Vector3(0, 2000, 0), feet,
		SkydotPlayer.LAYER_TERRAIN)
	var hit := camera.get_world_3d().direct_space_state.intersect_ray(query)
	if not hit.is_empty():
		player.teleport(hit["position"] + Vector3(0, 0.05, 0))
		_last_ground = player.global_position
