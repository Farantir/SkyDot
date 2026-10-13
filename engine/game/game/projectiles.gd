# SPDX-License-Identifier: GPL-3.0-or-later
#
# Projectiles in flight: what Weapon.Fire launches. No viewer, camera or
# input in here, only the pack, the world and the scene they fly through. A
# projectile is the PROJ's model carried by its speed and gravity, stopped by
# the first thing its path crosses and left stuck there, as arrows are. It
# does no damage and makes no sound yet. The model and the muzzle flash are
# built with SkydotWorld.build_model, so they have Skyrim's materials, add-ons,
# particles and animators; a PROJ with the muzzle flash flag (0x8) puffs its
# NAM1 model (dart traps: dust) at the shot's origin for its duration, then
# stops the emitters and removes the model once the last particle has faded.
class_name Projectiles
extends Node3D

const MAX_LIVE := 64
const STUCK_SECONDS := 30.0  # a stuck projectile is removed this long after it was fired
const GRAVITY := 9.81  # m/s^2, times the projectile's gravity multiplier
## Nodes of a model that mark where its projectiles leave, by lowercase name part.
const MUZZLE_NAMES := ["projectile", "fire", "muzzle", "launch"]
const MASK := 1 | 2 | 8  # world, clutter, terrain
const FLAG_MUZZLE_FLASH := 0x8  # PROJ DATA flags
const FLASH_FADE_SECONDS := 2.0  # a muzzle flash is freed this long after its emitters stop

var _world: SkydotWorld
var _pack: SkydotPack
var _find_ref: Callable  # reference id -> its built node, or null
var _live: Array[Dictionary] = []  # node, velocity (m/s), age, travelled (m), range (m), stuck, exclude
var _warned := {}


func _init(world: SkydotWorld, pack: SkydotPack, find_ref: Callable) -> void:
	_world = world
	_pack = pack
	_find_ref = find_ref
	name = "Projectiles"


## Launch the ammo's projectile from `source` along its forward axis (Skyrim's
## +Y), or from its fire node if the model has one. Does nothing, logging once,
## when there is no ammo (a weapon without ammo, a bow, has no projectile) or
## no source in the scene.
func fire(weapon: int, source: int, ammo: int) -> void:
	if ammo == 0:
		_warn_once("noammo%d" % weapon, "0x%08X fires without ammo: no projectile" % weapon)
		return
	var ammo_data := _world.get_ammo(ammo)
	var proj := _world.get_projectile(ammo_data.get("projectile", 0))
	if proj.is_empty():
		_warn_once("noproj%d" % ammo, "ammo 0x%08X has no projectile" % ammo)
		return
	var node: Node = _find_ref.call(source)
	if not node is Node3D:
		_warn_once("nosrc%d" % source, "0x%08X is not built: nothing fires from it" % source)
		return
	var origin := _muzzle(node as Node3D)
	var model_path: String = proj["model"] if proj["model"] != "" else ammo_data["model"]
	var model: Node3D = _world.build_model(model_path) if model_path != "" else null
	if model == null:
		model = _fallback_dart()
	var forward := _forward(node as Node3D, origin)
	var scale_m := SkydotWorld.unit_scale()
	var speed: float = proj["speed"] * scale_m
	var shot := {
		"node": model,
		"velocity": forward * speed,
		"gravity": GRAVITY * float(proj["gravity"]),
		"age": 0.0,
		"travelled": 0.0,
		"range": float(proj["range"]) * scale_m,
		"stuck": false,
		"exclude": _bodies(node),
	}
	add_child(model)
	model.global_transform = Transform3D(Basis.looking_at(forward, Vector3.UP), origin.origin)
	while _live.size() >= MAX_LIVE:
		_remove(0)
	_live.append(shot)
	_muzzle_flash(proj, origin.origin, forward)
	print("projectile 0x%08X from 0x%08X: speed %.1f m/s, gravity x%s, range %.1f m" % [
		ammo_data["projectile"], source, speed, proj["gravity"], shot["range"]])


## The PROJ's muzzle flash model at the shot's origin, facing `forward`, if it
## has the flag and a model. Its emitters stop after the PROJ's duration.
func _muzzle_flash(proj: Dictionary, position: Vector3, forward: Vector3) -> void:
	var path: String = proj.get("muzzle_flash_model", "")
	if int(proj["flags"]) & FLAG_MUZZLE_FLASH == 0 or path == "":
		return
	var flash := _world.build_model(path)
	if flash == null:
		_warn_once("noflash%s" % path, "muzzle flash %s is not in the pack" % path)
		return
	add_child(flash)
	flash.global_transform = Transform3D(Basis.looking_at(forward, Vector3.UP), position)
	var duration := maxf(float(proj.get("muzzle_flash_duration", 0.0)), 0.0)
	var emitters := flash.find_children("*", "SkydotParticles", true, false)
	print("muzzle flash %s: %d particle systems, %d add-ons, %.2f s" % [
		path, emitters.size(), flash.find_children("AddOn", "Node3D", true, false).size(), duration])
	var timer := get_tree().create_timer(duration)
	timer.timeout.connect(_stop_flash.bind(flash))


func _stop_flash(flash: Node3D) -> void:
	if not is_instance_valid(flash):
		return
	for emitter in flash.find_children("*", "SkydotParticles", true, false):
		emitter.set_active(false)
	get_tree().create_timer(FLASH_FADE_SECONDS).timeout.connect(flash.queue_free)


func _physics_process(delta: float) -> void:
	var space := get_world_3d().direct_space_state
	var i := 0
	while i < _live.size():
		var shot := _live[i]
		shot["age"] += delta
		if shot["age"] > STUCK_SECONDS or (not shot["stuck"] and shot["travelled"] > shot["range"]):
			_remove(i)
			continue
		if not shot["stuck"]:
			_advance(shot, delta, space)
		i += 1


func _advance(shot: Dictionary, delta: float, space: PhysicsDirectSpaceState3D) -> void:
	var node: Node3D = shot["node"]
	var velocity: Vector3 = shot["velocity"]
	velocity.y -= shot["gravity"] * delta
	var from := node.global_position
	var to := from + velocity * delta
	var query := PhysicsRayQueryParameters3D.create(from, to, MASK, shot["exclude"])
	var hit := space.intersect_ray(query)
	shot["velocity"] = velocity
	if not hit.is_empty():
		node.global_position = hit["position"]
		shot["stuck"] = true
		return
	shot["travelled"] += from.distance_to(to)
	node.global_transform = Transform3D(Basis.looking_at(velocity.normalized(), Vector3.UP), to)


func _remove(index: int) -> void:
	var node: Node = _live[index]["node"]
	node.queue_free()
	_live.remove_at(index)


## Where a shot leaves `source`: its fire node's transform, else its own.
func _muzzle(source: Node3D) -> Transform3D:
	var part := _fire_node(source)
	return part.global_transform if part != null else source.global_transform


## The node of a model that marks where its projectiles leave, or null.
func _fire_node(source: Node3D) -> Node3D:
	for part in source.find_children("*", "Node3D", true, false):
		var lowered := String(part.name).to_lower()
		for key in MUZZLE_NAMES:
			if lowered.contains(key):
				return part as Node3D
	return null


## The way a shot flies. A fire node is a NIF node, whose +Y is the model's
## forward and which the built model carries in Skyrim's frame (the global
## basis maps the NIF axes to the scene). A reference without one fires along
## its own Skyrim +Y, which the scene has as -Z.
func _forward(source: Node3D, muzzle: Transform3D) -> Vector3:
	if _fire_node(source) != null:
		return muzzle.basis.y.normalized()
	return -muzzle.basis.z.normalized()


## The physics bodies of a model, which its own shots pass through.
func _bodies(source: Node) -> Array[RID]:
	var rids: Array[RID] = []
	for body in source.find_children("*", "CollisionObject3D", true, false):
		rids.append((body as CollisionObject3D).get_rid())
	return rids


## A thin box 10 cm long, when the PROJ has no model.
func _fallback_dart() -> Node3D:
	var mesh := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(0.01, 0.01, 0.1)
	mesh.mesh = box
	return mesh


func _warn_once(key: String, text: String) -> void:
	if not _warned.has(key):
		_warned[key] = true
		print(text)

