# SPDX-License-Identifier: GPL-3.0-or-later
#
# The 3D preview of the item under the cursor (ADR 0010): the item's model, the one a dropped item shows
# (SkydotWorld.build_model of the base's model path), in a SubViewport of its own (own World3D, camera and
# light) drawn beside the list. The mouse turns it (drag with the left button inside the preview) and zooms it
# (wheel), the right stick turns it too. A VR host calls `use_world_node`: the model is then a real 3D object
# beside the panel and turns slowly by itself. Spells have no model and show nothing.
#
# The game's own angle and place are the engine's, not in the movie: InventoryMenu only gives the area where a
# drag rotates the item (MouseRotationRect, the right of the stage's centre); the pose here is a three-quarter view.
class_name ItemPreview
extends Control

const START_YAW := 0.0
const START_PITCH := 0.0
const PITCH_LIMIT := deg_to_rad(80.0)
const DRAG_RADIANS_PER_PIXEL := 0.012
const STICK_SPEED := 2.5  # radians per second at full deflection
const STICK_DEAD := 0.25
const ZOOM_STEP := 1.1
const ZOOM_MIN := 0.5
const ZOOM_MAX := 1.6
const FOV := 30.0

## The game's placement of the 3D item (Skyrim.ini [Interface], read with getini in the game, 2026-11-15):
## fInventory3DItemPosX/Y/Z and fInventory3DItemPosScale, and the ...Wide set used from 16:10 up. Y is the
## distance in front of the camera, the others offsets from it; the scale makes the item's bound fill that much
## more or less of the screen. Measured with the Iron Sword (SkyrimRemote @item3d, 1100x800, the narrow set): its
## bound spans 0.35 of the screen's height, centred 0.4 down, the blade pointing up, flat to the viewer.
const INI_NARROW := {"x": -38.0, "y": -500.0, "z": 16.0, "scale": 1.87}
const INI_WIDE := {"x": -29.0, "y": -500.0, "z": 12.0, "scale": 1.5}
## Radians about X that stand a flat armor model up (front to the viewer).
const ARMOR_STAND := -PI * 0.5
const WIDE_ASPECT := 1.6
const ITEM_HEIGHT := 0.35  # of the screen's height, with the narrow set's scale

## Where the preview is, as fractions of this control (the host's area): the free area right of the list.
var region := Rect2(0.64, 0.08, 0.3, 0.64)
## Radians per second the model turns by itself (the VR host sets it).
var auto_spin := 0.0

var _session: GameSession
var _viewport: SubViewport
var _container: SubViewportContainer
var _camera: Camera3D
var _pivot := Node3D.new()  # turns
var _holder := Node3D.new()  # scales and centres the model
var _model: Node3D
var _form := -1
var _yaw := START_YAW
var _pitch := START_PITCH
var _zoom := 1.0
var _stick := Vector2.ZERO
var _dragging := false


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_container = SubViewportContainer.new()
	_container.stretch = true
	_container.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_container)
	_viewport = SubViewport.new()
	_viewport.own_world_3d = true
	_viewport.transparent_bg = true
	_viewport.msaa_3d = Viewport.MSAA_4X
	_viewport.size = Vector2i(256, 256)
	_container.add_child(_viewport)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_CLEAR_COLOR
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.55, 0.55, 0.6)
	environment.ambient_light_energy = 0.8
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	_viewport.add_child(world_environment)
	_camera = Camera3D.new()
	_camera.fov = FOV
	_camera.current = true
	_viewport.add_child(_camera)
	var light := DirectionalLight3D.new()
	light.rotation = Vector3(deg_to_rad(-40.0), deg_to_rad(-30.0), 0.0)
	light.light_energy = 1.4
	_viewport.add_child(light)
	_pivot.add_child(_holder)
	_viewport.add_child(_pivot)
	_container.visible = false
	resized.connect(_place)
	_place()
	_apply()


func _place() -> void:
	var rect := Rect2(size * region.position, size * region.size)
	_container.position = rect.position
	_container.size = rect.size
	_viewport.size = Vector2i(maxi(int(rect.size.x), 16), maxi(int(rect.size.y), 16))


## The rectangle the preview takes, in this control's pixels.
func rect() -> Rect2:
	return Rect2(size * region.position, size * region.size)


## Whether a model is on show.
func is_showing() -> bool:
	return _model != null


## The form being shown, -1 for none.
func form() -> int:
	return _form


## Show the model of item `form` (a base with a model); anything else, or -1, clears the preview.
func show_item(form: int) -> void:
	if form == _form:
		return
	_form = form
	if _model != null:
		_model.queue_free()
		_model = null
	_container.visible = false
	_pivot.visible = false
	if form < 0:
		return
	var path: String = _session.world.get_base(form).get("model", "")
	if path == "":
		return
	var built := _session.world.build_model(path)
	if built == null:
		return
	_model = built
	_model.transform = SkydotWorld.skyrim_transform(Vector3.ZERO, Vector3.ZERO, 1.0)
	_holder.add_child(_model)
	_fit()
	_pivot.visible = true
	_yaw = START_YAW
	_pitch = START_PITCH
	_zoom = 1.0
	_apply()
	_container.visible = not _in_world


func clear() -> void:
	show_item(-1)


## The game's pose: the longest side of the model's bounds points up, the next one across, so a blade stands
## upright and flat to the viewer (what the game shows at first look), the model scaled to one unit and centred.
## An armor or anything else keeps its own orientation (the authored up is up) when its longest side already
## is the vertical one.
func _fit() -> void:
	_holder.transform = Transform3D.IDENTITY
	var box := AABB()
	var first := true
	for mesh: MeshInstance3D in _model.find_children("*", "MeshInstance3D", true, false):
		if mesh.mesh == null:
			continue
		var to_holder: Transform3D = _holder.global_transform.affine_inverse() * mesh.global_transform if mesh.is_inside_tree() else mesh.transform
		var moved := to_holder * mesh.get_aabb()
		box = moved if first else box.merge(moved)
		first = false
	if first or box.get_longest_axis_size() <= 0.0:
		return
	var sizes := [box.size.x, box.size.y, box.size.z]
	var order := [0, 1, 2]
	order.sort_custom(func(a: int, b: int) -> bool: return sizes[a] > sizes[b])
	var basis := Basis.IDENTITY
	var weapon := String(_session.item_info(_form).get("category", "")) in ["weapon", "ammo"]
	if weapon and order[0] != 1:
		# Authored hilt up in this frame: the blade points up in the game, so turn it half a turn about the depth.
		var up := Vector3.ZERO
		up[order[0]] = -1.0
		var across := Vector3.ZERO
		across[order[1]] = -1.0
		var depth := across.cross(up)
		# The rotation taking `up` to +Y, `across` to +X: the inverse of the basis with those columns.
		basis = Basis(across, up, depth).inverse()
	elif not weapon and order[2] == 1 and String(_session.item_info(_form).get("category", "")) == "armor":
		# An armor's ground model lies flat (its thinnest side up); the game shows it standing, front to the viewer.
		basis = Basis(Vector3.RIGHT, ARMOR_STAND)
	var scale := 1.0 / box.get_longest_axis_size()
	_holder.transform = Transform3D(basis * Basis.from_scale(Vector3.ONE * scale), -(basis * box.get_center()) * scale)


func _distance() -> float:
	var aspect := size.x / maxf(size.y, 1.0)
	var ini := INI_WIDE if aspect >= WIDE_ASPECT else INI_NARROW
	var fraction := ITEM_HEIGHT * float(ini["scale"]) / float(INI_NARROW["scale"]) / region.size.y  # of the preview's height
	return 1.0 / (2.0 * fraction * tan(deg_to_rad(FOV) * 0.5))


func _apply() -> void:
	_pivot.rotation = Vector3(_pitch, _yaw, 0.0)
	_camera.position = Vector3(0.0, 0.0, _distance() / _zoom)


## Mouse drag and wheel inside the preview; true if the event was used.
func handle_input(event: InputEvent) -> bool:
	if event is InputEventJoypadMotion:
		if event.axis == JOY_AXIS_RIGHT_X:
			_stick.x = event.axis_value if absf(event.axis_value) > STICK_DEAD else 0.0
		elif event.axis == JOY_AXIS_RIGHT_Y:
			_stick.y = event.axis_value if absf(event.axis_value) > STICK_DEAD else 0.0
		return false
	if not (event is InputEventMouseButton or event is InputEventMouseMotion):
		return false
	var at: Vector2 = make_input_local(event).position
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT:
			_dragging = event.pressed and rect().has_point(at)
			return _dragging
		if event.pressed and rect().has_point(at) and _model != null:
			if event.button_index == MOUSE_BUTTON_WHEEL_UP:
				zoom_by(ZOOM_STEP)
				return true
			if event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
				zoom_by(1.0 / ZOOM_STEP)
				return true
		return false
	if _dragging and event.button_mask & MOUSE_BUTTON_MASK_LEFT != 0:
		turn(event.relative.x * DRAG_RADIANS_PER_PIXEL, event.relative.y * DRAG_RADIANS_PER_PIXEL)
		return true
	_dragging = false
	return false


## Turn the model by `yaw` and `pitch` radians.
func turn(yaw: float, pitch: float) -> void:
	_yaw += yaw
	_pitch = clampf(_pitch + pitch, -PITCH_LIMIT, PITCH_LIMIT)
	_apply()


func zoom_by(factor: float) -> void:
	_zoom = clampf(_zoom * factor, ZOOM_MIN, ZOOM_MAX)
	_apply()


func zoom() -> float:
	return _zoom


func _process(delta: float) -> void:
	if _model == null:
		return
	if _stick != Vector2.ZERO:
		turn(_stick.x * STICK_SPEED * delta, _stick.y * STICK_SPEED * delta)
	elif auto_spin != 0.0:
		turn(auto_spin * delta, 0.0)


## A VR host: the model is a real object in `parent` (a Node3D of the scene) at `at`, `metres` across, beside
## the panel, instead of a picture in the panel. It turns by itself.
func use_world_node(parent: Node3D, at: Vector3, metres := 0.3) -> void:
	_pivot.get_parent().remove_child(_pivot)
	parent.add_child(_pivot)
	_pivot.position = at
	_pivot.scale = Vector3.ONE * metres
	_container.visible = false
	set_deferred("auto_spin", 0.8)
	_in_world = true


var _in_world := false
