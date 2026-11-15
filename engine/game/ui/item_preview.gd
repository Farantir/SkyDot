# SPDX-License-Identifier: GPL-3.0-or-later
#
# The 3D preview of the item under the cursor (ADR 0010): the item's model, the one a dropped item shows
# (SkydotWorld.build_model of the base's model path), in a SubViewport of its own (own World3D, camera and
# light) drawn beside the list. The mouse turns it (drag with the left button inside the preview) and zooms it
# (wheel), the right stick turns it too. A VR host calls `use_world_node`: the model is then a real 3D object
# beside the panel and turns slowly by itself. Spells have no model and show nothing.
#
# The pose is the model's own: its inventory marker (BSInvMarker) turns and zooms it (see _fit); InventoryMenu only
# gives the area where a drag rotates the item (MouseRotationRect, the right of the stage's centre).
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
## A model without a marker is not turned: the Iron Helmet's ground model (helmetgnd.nif has none) shows its face
## upright in the game (SkyrimRemote @item3d 0x12E4D).
const DEFAULT_ROTATION := Vector3.ZERO
## Skyrim's axes (Z up) to Godot's (Y up, Skyrim's +Y to -Z), as the converter turns the models.
const TO_GODOT := Basis(Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0))
## The preview's camera looks along Godot's -Z; the game's item camera looks along Skyrim's -Y (Godot's +Z): half a turn.
const FACING := Basis(Vector3.UP, PI)
## The bound (half the box's diagonal) at zoom 1, in the units _distance() frames: fitted to the game's sizes at
## 1100x800 (SkyrimRemote @item3d: Iron Sword 280 px tall, Iron Dagger 190, Iron Shield 257 across, potion 116; this
## gives them within about 25 %: the game's own bound is not the box's).
const BOUND_UNITS := 0.42
const WIDE_ASPECT := 1.6
const ITEM_HEIGHT := 0.35  # of the screen's height, with the narrow set's scale

## Where the preview is, as fractions of this control (the host's area): centred where the game centres the item's
## bound (x 0.68, y 0.395 of the screen at 1100x800, the same for a sword, a dagger, a shield and a potion).
var region := Rect2(0.53, 0.075, 0.3, 0.64)
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


## The game's pose: the model's own inventory marker (BSInvMarker "INV", converter mesh/28) turns it, Gamebryo's
## clockwise angles applied Z, then Y, then X, then seen from the model's +Y side with Skyrim's Z up (the camera looks
## along the game's -Y: a shield shows its face, a war axe its blade on the right, as in the game); a model without one is not turned. The marker's zoom scales
## it, the model's bound (the half diagonal of its box) sized to BOUND_UNITS, centred on the bound's centre.
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
	var marker := inventory_marker(_model)
	var basis := marker_basis(marker["rotation"])
	var scale := float(marker["zoom"]) * BOUND_UNITS / (box.size.length() * 0.5)
	_holder.transform = Transform3D(basis * Basis.from_scale(Vector3.ONE * scale), -(basis * box.get_center()) * scale)


## The model's inventory marker: {rotation: Vector3 radians, zoom}; the marker's defaults when it has none.
static func inventory_marker(model: Node) -> Dictionary:
	for node: Node in [model, model.get_node_or_null("bethconv_z_up_to_y_up")]:
		if node != null and node.has_meta("extras"):
			var block: Dictionary = (node.get_meta("extras") as Dictionary).get("bethconv", {})
			if block.has("inventory_marker"):
				var marker: Dictionary = block["inventory_marker"]
				var r: Array = marker["rotation"]
				return {"rotation": Vector3(r[0], r[1], r[2]), "zoom": float(marker["zoom"])}
	return {"rotation": DEFAULT_ROTATION, "zoom": 1.0}


## The marker's rotation as a basis of Godot's space (the model as built: Y up, Skyrim's +Y is -Z). Gamebryo's
## X/Y/Z rotations turn clockwise (the standard ones by minus the angle); the product X * Y * Z turns by Z first.
static func marker_basis(rotation: Vector3) -> Basis:
	var skyrim := Basis(Vector3(1, 0, 0), -rotation.x) * Basis(Vector3(0, 1, 0), -rotation.y) * Basis(Vector3(0, 0, 1), -rotation.z)
	return FACING * TO_GODOT * skyrim * TO_GODOT.inverse()


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
