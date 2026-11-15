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

## The game's item camera and placement (SkyrimSE.exe 1.6.1170: the UI scene's camera setup next to
## fUICameraNearDistance, Inventory3DManager's Begin3D and its model fit; INI values read with getini in the game,
## 2026-11-15, all the defaults). The camera sits at the UI scene's origin looking along its -Y, Z up, X to the
## left. Its frustum is 0.75 * tan(fDefaultFOV * 0.15) wide either side of the centre (radians; 80 degrees give
## 12), the height by the screen's aspect.
const DEFAULT_FOV := 80.0  # fDefaultFOV:Display
const FRUSTUM_ANGLE := 0.15
const FRUSTUM_WIDTH := 0.75
## fInventory3DItemPosX/Y/Z and fInventory3DItemPosScale; the ...Wide set when the screen is wider than 4:3. The
## model's bound sphere (Gamebryo's world bound, see bound_sphere) is scaled to fInventory3DBoundRadiusScale *
## PosScale * the inventory marker's zoom game units, centred on the item position; screens wider than 16:9
## shrink it by 16:9 over their aspect.
const INI_NARROW := {"x": -38.0, "y": -500.0, "z": 16.0, "scale": 1.87}
const INI_WIDE := {"x": -29.0, "y": -500.0, "z": 12.0, "scale": 1.5}
const WIDE_ASPECT := 4.0 / 3.0
const MAX_ASPECT := 16.0 / 9.0
const BOUND_RADIUS_SCALE := 12.5  # fInventory3DBoundRadiusScale
## The item menus' light (MenuLight of the Inventory light scheme): fInventoryMenuLightX/Y/Z,
## fInventoryLightDiffuseColorR/G/B, fInventoryLightRadius, fInventoryLightDimmerValue; the second light has
## radius 0 and adds nothing. The game's point lights fade as 1 - (distance / radius)^2.
const MENU_LIGHTS := [
	{"position": Vector3(100.0, -350.0, 100.0), "color": Color(0.98, 0.95, 0.83), "radius": 400.0, "dimmer": 1.75},
	{"position": Vector3(100.0, -350.0, 100.0), "color": Color(0.98, 0.95, 0.83), "radius": 0.0, "dimmer": 1.75},
]
## A model without a marker is not turned: the Iron Helmet's ground model (helmetgnd.nif has none) shows its face
## upright in the game (SkyrimRemote @item3d 0x12E4D).
const DEFAULT_ROTATION := Vector3.ZERO
## Skyrim's axes (Z up) to Godot's (Y up, Skyrim's +Y to -Z), as the converter turns the models.
const TO_GODOT := Basis(Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0))
## The preview's camera looks along Godot's -Z; the game's item camera looks along Skyrim's -Y (Godot's +Z): half a turn.
const FACING := Basis(Vector3.UP, PI)
## The UI scene has no fog, but SkyDot's materials fog by the global weather fog (metres from the camera): the
## preview's scene is built this much smaller than the game's units, so the item sits centimetres from its camera.
const SCENE_SCALE := 0.01
const NEAR := 15.0  # fUICameraNearDistance
const FAR := 20480.0  # fUICameraFarDistance

## Where a drag turns the item, as fractions of this control (the host's area; InventoryMenu's MouseRotationRect).
var region := Rect2(0.53, 0.075, 0.3, 0.64)
## Where the item's bound centre shows, as fractions of this control; negative: where the game's INI puts it.
var item_center := Vector2(-1.0, -1.0)
## Radians per second the model turns by itself (the VR host sets it).
var auto_spin := 0.0

var _session: GameSession
## The preview's own image space pass (neutral unless a host sets one).
var image_space := SkydotImageSpace.new()
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
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	_viewport.add_child(world_environment)
	_camera = Camera3D.new()
	_camera.keep_aspect = Camera3D.KEEP_WIDTH
	_camera.fov = rad_to_deg(2.0 * atan(FRUSTUM_WIDTH * tan(deg_to_rad(DEFAULT_FOV) * FRUSTUM_ANGLE)))
	_camera.near = NEAR * SkydotWorld.unit_scale() * SCENE_SCALE
	_camera.far = FAR * SkydotWorld.unit_scale() * SCENE_SCALE
	_camera.current = true
	# SkyDot's materials draw the game's gamma-space numbers; the image space pass turns them into linear colour.
	_camera.compositor = Compositor.new()
	_camera.compositor.compositor_effects = [image_space]
	_viewport.add_child(_camera)
	for light: Dictionary in MENU_LIGHTS:
		if float(light["radius"]) > 0.0:
			var omni := OmniLight3D.new()
			omni.set_meta(&"menu_light", light)
			_viewport.add_child(omni)
			_lights.append(omni)
	_pivot.add_child(_holder)
	_viewport.add_child(_pivot)
	_container.visible = false
	resized.connect(_place)
	_place()
	_apply()


func _place() -> void:
	_container.position = Vector2.ZERO
	_container.size = size
	_apply()


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
## along the game's -Y: a shield shows its face, a war axe its blade on the right, as in the game); a model without
## one is not turned. The holder scales the model's bound sphere to a radius of the marker's zoom, centred on the
## pivot; _apply scales the pivot to the game's size.
func _fit() -> void:
	_holder.transform = Transform3D.IDENTITY
	var bound := bound_sphere(_model, _holder)
	if bound["radius"] <= 0.0:
		return
	var marker := inventory_marker(_model)
	var basis := marker_basis(marker["rotation"])
	var scale := float(marker["zoom"]) / float(bound["radius"])
	_holder.transform = Transform3D(basis * Basis.from_scale(Vector3.ONE * scale), -(basis * (bound["center"] as Vector3)) * scale)


## The model's bound as Gamebryo's world bound (NiAVObject::worldBound, Inventory3DManager sizes the item by its
## radius): each shape's sphere (the box centre of its vertices, out to the farthest one), merged two at a time
## into the smallest sphere holding both (NiBound::Merge). {center, radius} in `space`'s coordinates.
static func bound_sphere(model: Node3D, space: Node3D) -> Dictionary:
	var center := Vector3.ZERO
	var radius := 0.0
	for mesh: MeshInstance3D in model.find_children("*", "MeshInstance3D", true, false):
		if mesh.mesh == null:
			continue
		var to_space: Transform3D = space.global_transform.affine_inverse() * mesh.global_transform if mesh.is_inside_tree() else mesh.transform
		var points := PackedVector3Array()
		for surface in mesh.mesh.get_surface_count():
			points.append_array(mesh.mesh.surface_get_arrays(surface)[Mesh.ARRAY_VERTEX])
		if points.is_empty():
			continue
		var box := AABB(points[0], Vector3.ZERO)
		for point in points:
			box = box.expand(point)
		var c := box.get_center()
		var r := 0.0
		for point in points:
			r = maxf(r, point.distance_squared_to(c))
		r = sqrt(r) * to_space.basis.get_scale().x
		c = to_space * c
		if radius <= 0.0:
			center = c
			radius = r
			continue
		var d := c - center
		var length := d.length()
		var grow := r - radius
		if grow * grow >= length * length:
			if grow >= 0.0:
				center = c
				radius = r
			continue
		if length > 0.00001:
			center += d * ((length + grow) / (2.0 * length))
		radius = 0.5 * (length + radius + r)
	return {"center": center, "radius": radius}


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


## The game's item position and size for this screen: the INI set by aspect, the item where the host asks for it
## (item_center) at the game's depth, the pivot scaled so a bound radius of 1 is the game's size; the light faded
## for the item's distance from it.
func _apply() -> void:
	_pivot.rotation = Vector3(_pitch, _yaw, 0.0)
	if _in_world:
		return
	var unit := SkydotWorld.unit_scale() * SCENE_SCALE
	var aspect := size.x / maxf(size.y, 1.0)
	var ini: Dictionary = INI_WIDE if aspect > WIDE_ASPECT else INI_NARROW
	# The game's UI space (X left, Y away from the camera's view, Z up) in the camera's: x = -X, y = Z, z = Y.
	var at := Vector3(-float(ini["x"]), float(ini["z"]), float(ini["y"]))
	if item_center.x >= 0.0:
		var half_width := FRUSTUM_WIDTH * tan(deg_to_rad(DEFAULT_FOV) * FRUSTUM_ANGLE) * -at.z
		at.x = (item_center.x * 2.0 - 1.0) * half_width
		at.y = (1.0 - item_center.y * 2.0) * half_width / maxf(aspect, 0.01)
	var scale := BOUND_RADIUS_SCALE * float(ini["scale"]) * _zoom
	if aspect > MAX_ASPECT:
		scale *= MAX_ASPECT / aspect
	_pivot.position = at * unit
	_pivot.scale = Vector3.ONE * scale * unit
	for omni: OmniLight3D in _lights:
		var light: Dictionary = omni.get_meta(&"menu_light")
		var p: Vector3 = light["position"]
		var radius := float(light["radius"])
		var from := Vector3(-p.x, p.z, p.y)
		var fade := 1.0 - pow(clampf(from.distance_to(at) / radius, 0.0, 1.0), 2.0)
		omni.position = from * unit
		SkydotMaterials.set_game_light(omni, (light["color"] as Color) * float(light["dimmer"]) * fade)
		omni.omni_range = radius * unit * 100.0  # the fade is in the energy: no further fall-off over the item
		omni.omni_attenuation = 0.0


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
	_pivot.scale = Vector3.ONE * metres * 0.5  # the holder's bound radius is 1
	_container.visible = false
	set_deferred("auto_spin", 0.8)
	_in_world = true


var _in_world := false
var _lights: Array[OmniLight3D] = []
