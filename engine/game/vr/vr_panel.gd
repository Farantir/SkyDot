# SPDX-License-Identifier: GPL-3.0-or-later
#
# A Control (ui/) shown in the world: the VR shell's host for a view. The view
# draws into a SubViewport; an unshaded quad shows that texture, always in
# front of the scene (no depth test), so a menu cannot be hidden by a wall.
# Parent the panel to the camera to lock it to the head (the HUD), or leave it
# in the world and call `recenter` to put it in front of the head (a menu).
class_name VrPanel
extends Node3D

## Metres in front of the head a recentred panel stands.
const MENU_DISTANCE := 1.3

var viewport: SubViewport
var quad: MeshInstance3D
var view: Control


## `view` is drawn at `pixels`; the quad is `width` metres wide, as high as the
## pixels say.
func _init(content: Control, pixels: Vector2i, width: float) -> void:
	view = content
	viewport = SubViewport.new()
	viewport.size = pixels
	viewport.transparent_bg = true
	viewport.disable_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.add_child(content)
	add_child(viewport)
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.no_depth_test = true
	material.render_priority = 10
	material.albedo_texture = viewport.get_texture()
	material.albedo_texture_force_srgb = true  # the view draws in sRGB
	var mesh := QuadMesh.new()
	mesh.size = Vector2(width, width * pixels.y / pixels.x)
	mesh.material = material
	quad = MeshInstance3D.new()
	quad.mesh = mesh
	quad.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(quad)


## Show or hide the panel (a hidden one is not drawn into either).
func set_shown(shown: bool) -> void:
	visible = shown
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS if shown else SubViewport.UPDATE_DISABLED


## Put the panel MENU_DISTANCE in front of `head`, upright, facing it.
func recenter(head: Node3D) -> void:
	var forward := -head.global_transform.basis.z
	forward.y = 0.0
	if forward.length() < 0.01:
		forward = Vector3.FORWARD
	forward = forward.normalized()
	var at := head.global_position + forward * MENU_DISTANCE
	look_at_from_position(at, head.global_position * Vector3(1, 0, 1) + Vector3(0, at.y, 0), Vector3.UP)
	rotate_y(PI)  # a quad faces +Z; look_at points -Z at the head
