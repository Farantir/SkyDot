# SPDX-License-Identifier: GPL-3.0-or-later
#
# Placeholders for what VisualEffect.Play and Actor.PlaySubGraphAnimation ask for (step 7b-7, a dragon's
# death). No camera, input or overlay. A visual effect with a target (a dragon's soul flying to the player)
# is a stream of glowing particles from the reference's chest to the target's chest, updated every step
# while it plays and ended by VisualEffect.Stop or after its seconds; one without a target shows nothing
# (the bridge logs it). The sub graph animation "SkinFadeOut" fades the meshes on the actor's skeleton
# that came from its skin (the cell builder marks them with the meta `skydot_skin`: the worn armor under
# them, a dragon's underskin, and the weapons on bone attachments stay) over FADE_SECONDS and hides them. The
# corpse's look does not rest on this: a body built or dressed again wears what the actor is equipped with
# (a dragon's SkinDragonHider covers the skin's slots, so the skin is not built).
# Other sub graph animations are only logged by the bridge.
extends RefCounted
class_name VisualEffects

const FADE_SECONDS := 2.0
const STREAM_LIFETIME := 1.0  # seconds a particle takes from the source to the target
const STREAM_COLOR := Color(0.55, 0.8, 1.0)

var _world: SkydotWorld
var _host: Node3D
var _player_chest: Callable  # () -> Vector3, the player's chest in engine space
var _streams := {}  # "effect:ref" -> {"node": GPUParticles3D, "ref": int, "target": int, "left": float}


func _init(world: SkydotWorld, host: Node3D, player_chest: Callable) -> void:
	_world = world
	_host = host
	_player_chest = player_chest


## How many streams play.
func stream_count() -> int:
	return _streams.size()


## SkydotPapyrus.visual_effect.
func on_visual_effect(effect: int, ref: int, target: int, seconds: float, playing: bool) -> void:
	var key := "%d:%d" % [effect, ref]
	_end(key)
	if not playing or target == 0 or _chest(ref) == null:
		return
	var particles := GPUParticles3D.new()
	particles.name = "VisualEffect_%08X" % effect
	particles.amount = 48
	particles.lifetime = STREAM_LIFETIME
	particles.local_coords = false
	particles.visibility_aabb = AABB(Vector3.ONE * -100.0, Vector3.ONE * 200.0)
	var material := ParticleProcessMaterial.new()
	material.gravity = Vector3.ZERO
	material.spread = 4.0
	material.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE
	material.emission_sphere_radius = 0.4
	material.scale_min = 0.6
	material.scale_max = 1.4
	particles.process_material = material
	var quad := QuadMesh.new()
	quad.size = Vector2.ONE * 0.18
	var look := StandardMaterial3D.new()
	look.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	look.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	look.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	look.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	look.albedo_color = STREAM_COLOR
	quad.material = look
	particles.draw_pass_1 = quad
	_host.add_child(particles)
	_streams[key] = {"node": particles, "ref": ref, "target": target, "left": seconds if seconds > 0.0 else -1.0}
	_aim(_streams[key])


## SkydotPapyrus.sub_graph_animation.
func on_sub_graph_animation(ref: int, name: String) -> void:
	if name == "SkinFadeOut":
		fade_skin(ref)


## Fade the skin meshes of `ref` out and hide them; false if it has no skeleton built.
func fade_skin(ref: int) -> bool:
	var node := _world.find_ref_node(ref) as Node3D
	if node == null or not node.is_inside_tree():
		return false
	var skeleton := node.find_child("Skeleton", true, false) as Skeleton3D
	if skeleton == null:
		return false
	var meshes: Array[MeshInstance3D] = []
	for child in skeleton.get_children():
		if child is MeshInstance3D and child.get_meta("skydot_skin", false):
			meshes.append(child)
	if meshes.is_empty():
		return true
	var tween := node.create_tween()
	tween.set_parallel(true)
	for mesh in meshes:
		tween.tween_property(mesh, "transparency", 1.0, FADE_SECONDS)
	tween.chain().tween_callback(func() -> void:
		for mesh in meshes:
			if is_instance_valid(mesh):
				mesh.visible = false)
	print("visual effects: the skin of 0x%08X fades out over %.1f s (%d meshes)" % [ref, FADE_SECONDS, meshes.size()])
	return true


## Move the streams and end those whose time is up or whose ends are gone.
func step(delta: float) -> void:
	for key in _streams.keys():
		var stream: Dictionary = _streams[key]
		if stream["left"] >= 0.0:
			stream["left"] = float(stream["left"]) - delta
			if stream["left"] <= 0.0:
				_end(key)
				continue
		if not _aim(stream):
			_end(key)


func _end(key: String) -> void:
	if _streams.has(key):
		var node: Node = _streams[key]["node"]
		if is_instance_valid(node):
			node.queue_free()
		_streams.erase(key)


## Point the stream from its reference's chest to its target's; false if either is gone.
func _aim(stream: Dictionary) -> bool:
	var from: Variant = _chest(stream["ref"])
	var to: Variant = _chest(stream["target"])
	if from == null or to == null:
		return false
	var particles: GPUParticles3D = stream["node"]
	var offset: Vector3 = (to as Vector3) - (from as Vector3)
	particles.global_position = from
	var material := particles.process_material as ParticleProcessMaterial
	material.direction = offset.normalized() if offset.length() > 0.01 else Vector3.UP
	var speed := offset.length() / STREAM_LIFETIME
	material.initial_velocity_min = speed * 0.9
	material.initial_velocity_max = speed * 1.1
	return true


## The middle of the body of `ref` (the player's chest; an actor's meshes' box), null if it has no node.
func _chest(ref: int) -> Variant:
	if ref == SkydotPapyrus.PLAYER_REF:
		return _player_chest.call()
	var node := _world.find_ref_node(ref) as Node3D
	if node == null or not node.is_inside_tree():
		return null
	var box := AABB()
	var any := false
	for mesh in node.find_children("*", "MeshInstance3D", true, false):
		var body := mesh as MeshInstance3D
		if body.get_parent() is Skeleton3D:
			var part := body.global_transform * body.get_aabb()
			box = part if not any else box.merge(part)
			any = true
	return box.get_center() if any else node.global_position + Vector3.UP * 1.2
