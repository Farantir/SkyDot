# SPDX-License-Identifier: GPL-3.0-or-later
#
# The player in VR (ADR 0006): a PlayerRig whose camera is the head. An
# XROrigin3D stands at the SkydotPlayer's feet with the XRCamera3D and two
# controllers under it; the headset moves the camera, the sticks move the
# player and turn the origin. It is still the player object the session knows
# (ADR 0002): `yaw` and `pitch` are where the head looks. Without a tracking
# session (no runtime) the camera is a plain one at the eyes and the
# keyboard and mouse look as in the viewer.
#
# Controllers (the default OpenXR action map): the left stick moves relative
# to the head (click: sprint), the right stick's x snap-turns SNAP_DEGREES
# about the head, the right trigger activates along the right controller's aim
# ray, A (right) jumps and accepts, B cancels, the left menu button asks for
# the inventory. They feed the InputMap actions PlayerInput and the menus
# read, so neither knows where the input comes from.
class_name VrRig
extends PlayerRig

## The left menu button was pressed.
signal inventory_requested

const SNAP_DEGREES := 30.0
## The right stick must pass this to snap-turn and fall below the lower one to
## turn again.
const SNAP_ON := 0.7
const SNAP_OFF := 0.3
## The left stick picks menu entries (ui_up, ui_down) past this.
const MENU_ON := 0.6
const MENU_OFF := 0.3

var origin: XROrigin3D
var left: XRController3D
var right: XRController3D  # its aim pose is the activation ray
## A session drives the camera (else it is a plain camera).
var tracking := false
var _snap_ready := true
var _menu_ready := true


func _init(settings: ViewerSettings, image_space: SkydotImageSpace) -> void:
	var head := XRCamera3D.new()
	head.name = "XRCamera3D"
	super(settings, image_space, head)
	origin = XROrigin3D.new()
	origin.name = "XROrigin3D"
	origin.add_child(camera)
	left = _controller(&"left_hand", &"grip")
	right = _controller(&"right_hand", &"aim")
	left.input_vector2_changed.connect(_left_stick)
	left.button_pressed.connect(_left_button.bind(true))
	left.button_released.connect(_left_button.bind(false))
	right.input_vector2_changed.connect(_right_stick)
	right.button_pressed.connect(_right_button.bind(true))
	right.button_released.connect(_right_button.bind(false))
	camera.position = Vector3(0, GameSession.EYE_HEIGHT, 0)


## The session runs: the headset places the camera from now on.
func set_tracking(on: bool) -> void:
	tracking = on


func _controller(tracker: StringName, pose: StringName) -> XRController3D:
	var node := XRController3D.new()
	node.name = String(tracker)
	node.tracker = tracker
	node.pose = pose
	origin.add_child(node)
	return node


## Put the head at `position` looking along `new_yaw`, the feet below it.
func place_at(position: Vector3, new_yaw: float, new_pitch: float) -> void:
	player.teleport(position - Vector3(0, GameSession.EYE_HEIGHT - 0.05, 0))
	_last_ground = player.global_position
	_ground_check = 3
	origin.global_position = player.global_position
	apply_look(new_yaw, new_pitch)


## Turn the origin so the head looks along `new_yaw` (the pitch is the
## headset's own; a plain camera looks up and down).
func apply_look(new_yaw: float, new_pitch: float) -> void:
	yaw = new_yaw
	pitch = new_pitch
	if tracking:
		origin.rotation.y = new_yaw - camera.transform.basis.get_euler(EULER_ORDER_YXZ).y
	else:
		origin.rotation.y = new_yaw
		camera.rotation = Vector3(new_pitch, 0, 0)


func save_section() -> PackedByteArray:
	return var_to_bytes({"position": camera.global_position, "yaw": yaw, "pitch": pitch})


## The origin goes to the player's feet and the head's direction is read back.
func follow() -> void:
	origin.global_position = player.global_position
	if not tracking:
		camera.position = Vector3(0, player.get_eye_position().y - player.global_position.y, 0)
	var euler := camera.global_basis.get_euler(EULER_ORDER_YXZ)
	yaw = euler.y
	pitch = euler.x


## Turn by `degrees` (left positive) about the head, not the feet, and carry
## the player along so the head stays where it was.
func snap_turn(degrees: float) -> void:
	var angle := deg_to_rad(degrees)
	var before := origin.global_position
	var pivot := camera.global_position
	pivot.y = before.y
	origin.global_position = pivot + (before - pivot).rotated(Vector3.UP, angle)
	origin.rotate_y(angle)
	player.teleport(player.global_position + origin.global_position - before)


static func _action(action: StringName, pressed: bool, strength := 1.0) -> void:
	var event := InputEventAction.new()
	event.action = action
	event.pressed = pressed
	event.strength = strength
	Input.parse_input_event(event)


static func _tap(action: StringName) -> void:
	_action(action, true)
	_action(action, false)


## Hold the pair of actions at the stick's deflection along one axis.
static func _axis(positive: StringName, negative: StringName, value: float) -> void:
	_action(positive, value > 0.0, maxf(value, 0.0))
	_action(negative, value < 0.0, maxf(-value, 0.0))


func _left_stick(input_name: String, value: Vector2) -> void:
	if input_name != "primary":
		return
	_axis(PlayerInput.MOVE_RIGHT, PlayerInput.MOVE_LEFT, value.x)
	_axis(PlayerInput.MOVE_FORWARD, PlayerInput.MOVE_BACK, value.y)
	# The same push picks menu entries, once per push.
	if absf(value.y) < MENU_OFF:
		_menu_ready = true
	elif _menu_ready and absf(value.y) > MENU_ON:
		_menu_ready = false
		_tap(&"ui_up" if value.y > 0.0 else &"ui_down")


func _right_stick(input_name: String, value: Vector2) -> void:
	if input_name != "primary":
		return
	if absf(value.x) < SNAP_OFF:
		_snap_ready = true
	elif _snap_ready and absf(value.x) > SNAP_ON:
		_snap_ready = false
		snap_turn(-SNAP_DEGREES if value.x > 0.0 else SNAP_DEGREES)


func _left_button(input_name: String, pressed: bool) -> void:
	match input_name:
		"primary_click":
			_action(PlayerInput.SPRINT, pressed)
		"menu_button":
			if pressed:
				inventory_requested.emit()


func _right_button(input_name: String, pressed: bool) -> void:
	match input_name:
		"trigger_click":
			_action(PlayerInput.ACTIVATE, pressed)
		"ax_button":
			_action(PlayerInput.JUMP, pressed)
			_action(&"ui_accept", pressed)
		"by_button":
			_action(&"ui_cancel", pressed)
