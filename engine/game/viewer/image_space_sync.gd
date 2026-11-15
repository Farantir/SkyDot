# SPDX-License-Identifier: GPL-3.0-or-later
#
# The image space the camera grades with, every frame: the weather's outside,
# the cell's inside, none with --image-space off, a neutral grade with
# --image-space neutral. Our shaders compute the fog themselves. Shell code
# both the viewer and the VR shell use.
class_name ImageSpaceSync
extends RefCounted

## An image space modifier over the grade while it is set (SkydotWorld.get_image_space_modifier; the open item
## menus set the inventory menu's, DOBJ IMID, MenuImageSpace): its cinematic saturation, brightness and contrast
## multiply and add onto the grade's, and its depth of field blurs the far world.
static var modifier := {}
## Godot's far blur amount for an IMAD depth of field strength of 1. The game's ISDepthOfField mixes the frame
## with a blurred copy by saturate((depth - distance) / range) * strength (Community Shaders' ISDepthOfField.hlsl);
## at 1100x800 its fully blurred far world fits a Gaussian of sigma ~5 px, and this amount gives Godot's bokeh about
## that (sigma 5-6, SkyrimRemote frames with and without the inventory at Riverwood, 2026-11-15). The ramp
## measured on the ground (tilt 70..15 degrees) follows the formula with ISMTween's 128 and 768.
const DOF_AMOUNT := 0.15


static func apply(viewport: Viewport, settings: ViewerSettings, session: GameSession,
		image_space: SkydotImageSpace) -> void:
	SkydotMaterials.sync_fog(viewport.find_world_3d().environment)
	if not settings.image_space:
		image_space.clear()
	else:
		var space: Dictionary
		# A sky seen from an interior ("show sky") does not grade the cell.
		if session.clock.has_weather() and not session.clock.weather.is_sky_only():
			space = session.clock.weather.get_image_space()
			if not settings.volumetric:
				space.erase("volumetric")
		else:
			space = session.place.interior_image_space.duplicate()
		if settings.neutral_grade:
			space.erase("cinematic")
			space.erase("tint")
		if not modifier.is_empty():
			space = _modified(space)
		image_space.set_image_space(space)
	_depth_of_field(viewport)


## IMAD: value * multiply + add for cinematic saturation (17), brightness (18) and contrast (19).
static func _modified(space: Dictionary) -> Dictionary:
	var out := space.duplicate()
	var cinematic := PackedFloat32Array([1.0, 1.0, 1.0])
	if space.has("cinematic") and (space["cinematic"] as PackedFloat32Array).size() >= 3:
		cinematic = (space["cinematic"] as PackedFloat32Array).duplicate()
	var multiply: PackedFloat32Array = modifier.get("multiply", PackedFloat32Array())
	var add: PackedFloat32Array = modifier.get("add", PackedFloat32Array())
	for i in 3:
		if multiply.size() > 17 + i and add.size() > 17 + i:
			cinematic[i] = cinematic[i] * multiply[17 + i] + add[17 + i]
	out["cinematic"] = cinematic
	return out


static var _attributes: CameraAttributesPractical
static var _blurred_camera: Camera3D


## The modifier's depth of field on the camera: far blur from its distance over its range (game units).
static func _depth_of_field(viewport: Viewport) -> void:
	var camera := viewport.get_camera_3d()
	var wanted := float(modifier.get("dof_strength", 0.0)) > 0.0
	if _blurred_camera != null and (not wanted or _blurred_camera != camera):
		if is_instance_valid(_blurred_camera):
			_blurred_camera.attributes = null
		_blurred_camera = null
	if not wanted or camera == null or _blurred_camera == camera:
		return
	if _attributes == null:
		_attributes = CameraAttributesPractical.new()
	var scale := SkydotWorld.unit_scale()
	_attributes.dof_blur_far_enabled = true
	_attributes.dof_blur_far_distance = float(modifier["dof_distance"]) * scale
	_attributes.dof_blur_far_transition = float(modifier["dof_range"]) * scale
	_attributes.dof_blur_amount = DOF_AMOUNT * float(modifier["dof_strength"])
	camera.attributes = _attributes
	_blurred_camera = camera
