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
# A projectile without the pick-up flag (0x40) is removed on impact, a pickable
# one (an arrow) stays stuck.
class_name Projectiles
extends Node3D

const MAX_LIVE := 64
const MAX_PUFFS := 96  # visual-only puffs in flight; they count apart from MAX_LIVE and never push a shot out
const STUCK_SECONDS := 30.0  # a stuck projectile is removed this long after it was fired
const GRAVITY := 9.81  # m/s^2, times the projectile's gravity multiplier
## Nodes of a model that mark where its projectiles leave, by lowercase name part.
const MUZZLE_NAMES := ["projectile", "fire", "muzzle", "launch"]
const MASK := 1 | 2 | 8  # world, clutter, terrain
const FLAG_MUZZLE_FLASH := 0x8  # PROJ DATA flags
const FLAG_PICKUP := 0x40  # PROJ DATA flags: can be picked up; the others vanish on impact
const FLASH_FADE_SECONDS := 2.0  # a muzzle flash is freed this long after its emitters stop

var _world: SkydotWorld
var _pack: SkydotPack
var _find_ref: Callable  # reference id -> its built node, or null
var _live: Array[Dictionary] = []  # node, velocity (m/s), age, travelled (m), range (m), stuck, exclude
var _warn := WarnOnce.new()
var puffs_spawned := 0  # visual-only puffs launched so far (smoke tests read it)


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
		_warn.once("noammo%d" % weapon, "0x%08X fires without ammo: no projectile" % weapon)
		return
	var ammo_data := _world.get_ammo(ammo)
	var proj := _world.get_projectile(ammo_data.get("projectile", 0))
	if proj.is_empty():
		_warn.once("noproj%d" % ammo, "ammo 0x%08X has no projectile" % ammo)
		return
	var node: Node = _find_ref.call(source)
	if not node is Node3D:
		_warn.once("nosrc%d" % source, "0x%08X is not built: nothing fires from it" % source)
		return
	var origin := _muzzle(node as Node3D)
	var model_path: String = proj["model"] if proj["model"] != "" else ammo_data["model"]
	var model: Node3D = _world.build_model(model_path) if model_path != "" else null
	if model == null:
		model = _fallback_dart()
	var forward := _forward(node as Node3D, origin)
	var scale_m := SkydotWorld.unit_scale()
	var speed: float = proj["speed"] * scale_m
	for animator in model.find_children("*", "SkydotAnimator", true, false):
		animator.start_controllers()  # the dart's trail grows and scrolls from launch
	_cold_start(model)
	var shot := {
		"node": model,
		"pickable": int(proj["flags"]) & FLAG_PICKUP != 0,
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
		_warn.once("noflash%s" % path, "muzzle flash %s is not in the pack" % path)
		return
	_cold_start(flash)
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


## An arrow shot from `origin` along `direction`, made of the PROJ `proj` and the AMMO `ammo_data`
## (SkydotWorld.get_projectile and get_ammo): its speed, gravity, range and model. `actor_test`
## (from, to) -> {ref, point} says whether the step from `from` to `to` enters an actor's body
## (`{}`: no), and then `on_hit(ref, point)` is called and the arrow is gone (it sticks in
## the world when it hits that).
func shoot(ammo: int, origin: Vector3, direction: Vector3, proj: Dictionary, ammo_data: Dictionary,
		actor_test: Callable, on_hit: Callable) -> void:
	var model_path: String = proj["model"] if proj["model"] != "" else ammo_data.get("model", "")
	var model: Node3D = _world.build_model(model_path) if model_path != "" else null
	if model == null:
		model = _fallback_dart()
	for animator in model.find_children("*", "SkydotAnimator", true, false):
		animator.start_controllers()
	_cold_start(model)
	add_child(model)
	model.global_transform = Transform3D(Basis.looking_at(direction, Vector3.UP), origin)
	while _live.size() >= MAX_LIVE:
		_remove(0)
	_live.append({
		"node": model,
		"pickable": int(proj["flags"]) & FLAG_PICKUP != 0,
		"velocity": direction * float(proj["speed"]) * SkydotWorld.unit_scale(),
		"gravity": GRAVITY * float(proj["gravity"]),
		"age": 0.0,
		"travelled": 0.0,
		"range": float(proj["range"]) * SkydotWorld.unit_scale(),
		"stuck": false,
		"exclude": [],
		"actor_test": actor_test,
		"on_hit": on_hit,
	})


## A spell's shot from `origin` along `direction`, made of the PROJ `proj` (SkydotWorld.get_projectile):
## its model, speed, gravity and range (`range_metres` > 0 shortens it). `actor_test` (from, to) ->
## {ref, point} or {} says whether the step enters an actor's body, as for `shoot`. At the impact
## `on_impact(ref, point)` is called once, `ref` 0 when the shot met the world, and the shot is gone.
## A shot that flies its range without meeting anything is gone silently. `flash`: also puff the
## PROJ's muzzle flash (flames do not, they leave ten times a second). `puff`: a visual-only copy
## (the stream between a flame's shots): no actor test, no callback, gone on the world or at its
## range, and counted apart so it never pushes a shot out.
func shoot_spell(origin: Vector3, direction: Vector3, proj: Dictionary, actor_test: Callable,
		on_impact: Callable, range_metres := 0.0, flash := true, puff := false) -> void:
	var model: Node3D = _world.build_model(proj["model"]) if proj["model"] != "" else null
	if model == null:
		_warn.once("nospellmodel%d" % int(proj["id"]), "spell projectile 0x%08X has no model" % int(proj["id"]))
		model = _fallback_dart()
	for animator in model.find_children("*", "SkydotAnimator", true, false):
		animator.start_controllers()
	_cold_start(model)
	add_child(model)
	model.global_transform = Transform3D(Basis.looking_at(direction, Vector3.UP), origin)
	if puff:
		puffs_spawned += 1
		while _count(true) >= MAX_PUFFS:
			_remove(_first(true))
	else:
		while _count(false) >= MAX_LIVE:
			_remove(_first(false))
	var reach := float(proj["range"]) * SkydotWorld.unit_scale()
	if range_metres > 0.0:
		reach = minf(reach, range_metres) if reach > 0.0 else range_metres
	var shot := {
		"node": model,
		"pickable": false,
		"velocity": direction * float(proj["speed"]) * SkydotWorld.unit_scale(),
		"gravity": GRAVITY * float(proj["gravity"]),
		"age": 0.0,
		"travelled": 0.0,
		"range": reach,
		"stuck": false,
		"exclude": _bodies(model),  # a spray model brings colliders of its own
		"puff": puff,
	}
	if not puff:
		shot["actor_test"] = actor_test
		shot["on_hit"] = on_impact
		shot["on_world"] = on_impact
	_live.append(shot)
	if flash:
		_muzzle_flash(proj, origin, direction)


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
	if shot.has("actor_test"):
		var end: Vector3 = hit["position"] if not hit.is_empty() else to
		var body: Dictionary = shot["actor_test"].call(from, end)
		if not body.is_empty():
			node.global_position = body["point"]
			shot["stuck"] = true
			_vanish(shot)
			shot["on_hit"].call(body["ref"], body["point"])
			return
	if not hit.is_empty():
		node.global_position = hit["position"]
		shot["stuck"] = true
		if not shot["pickable"]:
			_vanish(shot)
		if shot.has("on_world"):
			shot["on_world"].call(0, hit["position"])
		return
	shot["travelled"] += from.distance_to(to)
	node.global_transform = Transform3D(Basis.looking_at(velocity.normalized(), Vector3.UP), to)


## Particle systems start empty: a projectile's smoke is made as it flies, not
## pre-filled around the port.
func _cold_start(model: Node) -> void:
	for emitter in model.find_children("*", "SkydotParticles", true, false):
		emitter.set_warm_start(false)


## A projectile that cannot be picked up is gone on impact, with its trail and
## smoke: it is removed on the next physics step.
func _vanish(shot: Dictionary) -> void:
	(shot["node"] as Node3D).visible = false
	shot["age"] = STUCK_SECONDS + 1.0


func _count(puffs: bool) -> int:
	var n := 0
	for shot in _live:
		n += 1 if shot.get("puff", false) == puffs else 0
	return n


func _first(puffs: bool) -> int:
	for i in _live.size():
		if _live[i].get("puff", false) == puffs:
			return i
	return 0


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
