# SPDX-License-Identifier: GPL-3.0-or-later
#
# The camera and the player it follows, and where they look. The player is
# SkydotPlayer (walks with collision, or flies); the camera sits at its eyes
# and faces where `yaw` and `pitch` say. Input only changes those two; the
# places put the rig somewhere with place_camera.
class_name PlayerRig
extends RefCounted

const EYE_HEIGHT := 1.7  # metres

## The player fell below the world and was put back, flying.
signal fell_through

var camera: Camera3D
var player: SkydotPlayer  # the camera follows its eyes
var yaw := 0.0  # radians, about the up axis
var pitch := 0.0
var _last_ground := Vector3.ZERO  # where the player last stood
var _ground_check := 0  # frames until checking the player is not under the land


## `image_space` grades what the camera sees (always on).
func _init(settings: ViewerSettings, image_space: SkydotImageSpace) -> void:
	camera = Camera3D.new()
	camera.near = 0.05
	# The scene is drawn in the game's gamma space; this grades it as the
	# game's image space does and hands Godot linear colour.
	camera.compositor = Compositor.new()
	camera.compositor.compositor_effects = [image_space]
	if settings.has_fov:  # vertical, degrees
		camera.fov = settings.fov
	player = SkydotPlayer.new()
	player.name = "player"
	player.eye_height = EYE_HEIGHT
	player.fly = settings.fly


## Put the camera at `at` (a camera position) looking at `target`, or keep
## the direction if `target` is null, with the player's feet below it.
func place_camera(at: Vector3, target: Variant) -> void:
	camera.position = at
	if target != null:
		camera.look_at(target)
	yaw = camera.rotation.y
	pitch = camera.rotation.x
	# A little above the spot, so feet placed on a floor do not start in it.
	player.teleport(at - Vector3(0, EYE_HEIGHT - 0.05, 0))
	_last_ground = player.global_position
	_ground_check = 3


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
