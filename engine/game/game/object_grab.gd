# SPDX-License-Identifier: GPL-3.0-or-later
#
# Holding Activate on loose clutter carries it (the game's grab, its fZKey* settings): a spring
# pulls the body to a point ahead of the pointer at the distance it was grabbed from, letting go
# drops it, the attack button throws it. A short press still activates (picks up), on release.
#
# The spring is Havok's mouse spring (hkpMouseSpringAction) at the body's centre, per physics tick:
# its velocity is scaled by fZKeyObjectDamping, then it gets the impulse
# -mass * (fZKeySpringElasticity * offset / dt + fZKeySpringDamping * velocity), capped at
# min(mass, fZKeyMaxForceWeightHigh) * fZKeyMaxForce * dt. Only fZKeyMaxForce (100) and
# fZKeyMaxForceWeightHigh (150) are in Skyrim.esm; the other values are the executable's and
# assumed here (DEFAULTS), as are the hold time, the drop distance and the throw.
class_name ObjectGrab
extends Node

## Settings read from the pack, with assumed values for those the ESM does not hold.
const DEFAULTS := {
	"fZKeyMaxForce": 100.0,  # spring force per kg (m/s^2): Havok's maxRelativeForce
	"fZKeyMaxForceWeightHigh": 150.0,  # kg above which the force stops growing with the mass
	"fZKeySpringElasticity": 0.2,  # share of the offset closed per tick (assumed)
	"fZKeySpringDamping": 0.5,  # share of the velocity cancelled per tick (assumed)
	"fZKeyObjectDamping": 0.75,  # velocity kept per tick while held (assumed)
	"fZKeyHeavyWeight": 100.0,  # kg: heavier bodies are not grabbed (assumed meaning)
}
## Seconds Activate is held before it grabs instead of activating (no setting found: assumed).
const HOLD_SECONDS := 0.35
## Metres the body may be away from the hold point (stuck behind something) before it drops (assumed).
const DROP_DISTANCE := 1.0
## The nearest the body is held to the pointer, metres.
const MIN_DISTANCE := 0.6
## Metres the pointer reaches (ScriptActivation.REACH).
const REACH := 2.6
## Throw: metres per second added along the pointer, at most THROW_IMPULSE kg m/s (assumed).
const THROW_SPEED := 8.0
const THROW_IMPULSE := 40.0
const MASK := 1 | 2 | 8  # world, clutter, terrain

var _pointer: Callable  # -> Node3D
var _settings := {}
var _candidate: SkydotDynamicBody  # what a press is aimed at, until it grabs or ends
var _pressed_for := -1.0  # seconds a press on a grabbable body has lasted; < 0 when up or once it grabbed
var _held: SkydotDynamicBody
var _distance := 0.0
var _mask := 0  # the held body's collision mask before it was held


func _init(world: SkydotWorld, pointer: Callable) -> void:
	name = "ObjectGrab"
	_pointer = pointer
	for key in DEFAULTS:
		_settings[key] = world.get_gmst_float(key, DEFAULTS[key]) if world != null else DEFAULTS[key]


func setting(key: String) -> float:
	return _settings[key]


## Activate went down. True if the pointer is on a body that holding can grab: the caller then
## activates only on a short press (`release` says so).
func press() -> bool:
	drop()
	_candidate = aimed_body()
	_pressed_for = 0.0 if _candidate != null else -1.0
	return _candidate != null


## Activate went up: drops what is held. True if it was a short press on a grabbable body,
## which activates now.
func release() -> bool:
	var short := _pressed_for >= 0.0
	_pressed_for = -1.0
	_candidate = null
	drop()
	return short


func is_holding() -> bool:
	return _held != null and is_instance_valid(_held)


func held_body() -> SkydotDynamicBody:
	return _held if is_holding() else null


## The loose body the pointer is on within reach, or null.
func aimed_body() -> SkydotDynamicBody:
	var pointer := _pointer.call() as Node3D
	if pointer == null or not pointer.is_inside_tree():
		return null
	var from := pointer.global_position
	var query := PhysicsRayQueryParameters3D.create(from, from - pointer.global_transform.basis.z * REACH, MASK)
	var hit := pointer.get_world_3d().direct_space_state.intersect_ray(query)
	var body := hit.get("collider") as SkydotDynamicBody
	if body == null or body.mass > setting("fZKeyHeavyWeight"):
		return null
	return body


## Let go of what is held: it keeps its velocity and falls.
func drop() -> void:
	if is_holding():
		_held.collision_mask = _mask
	_held = null


## The attack button while holding: push the body along the pointer and let go. False if nothing is held.
func throw() -> bool:
	var body := held_body()
	var pointer := _pointer.call() as Node3D
	if body == null or pointer == null:
		return false
	drop()
	var impulse := minf(body.mass * THROW_SPEED, THROW_IMPULSE)
	body.apply_central_impulse(-pointer.global_transform.basis.z * impulse)
	return true


func _grab(body: SkydotDynamicBody) -> void:
	var pointer := _pointer.call() as Node3D
	_held = body
	_pressed_for = -1.0  # a hold: letting go no longer activates, also after a throw or a drop
	_mask = body.collision_mask
	body.collision_mask = _mask & ~SkydotPlayer.LAYER_ACTOR  # it does not shove the player
	body.wake()
	_distance = clampf(pointer.global_position.distance_to(body.global_position), MIN_DISTANCE, REACH)


func _physics_process(delta: float) -> void:
	if _pressed_for >= 0.0 and _held == null:
		_pressed_for += delta
		if _pressed_for >= HOLD_SECONDS:
			_pressed_for = -1.0
			if _candidate != null and is_instance_valid(_candidate) and _candidate.is_inside_tree():
				_grab(_candidate)
			_candidate = null
	var body := held_body()
	if body == null:
		_held = null
		return
	var pointer := _pointer.call() as Node3D
	if not body.is_inside_tree() or pointer == null or not pointer.is_inside_tree():
		drop()
		return
	var target := pointer.global_position - pointer.global_transform.basis.z * _distance
	var offset := body.global_position - target
	if offset.length() > DROP_DISTANCE:
		drop()
		return
	var keep := setting("fZKeyObjectDamping")
	body.linear_velocity *= keep
	body.angular_velocity *= keep
	var change := -(offset * setting("fZKeySpringElasticity") / delta
		+ body.linear_velocity * setting("fZKeySpringDamping"))
	var impulse := change * body.mass
	var most := minf(body.mass, setting("fZKeyMaxForceWeightHigh")) * setting("fZKeyMaxForce") * delta
	if impulse.length() > most:
		impulse = impulse.normalized() * most
	body.sleeping = false
	body.apply_central_impulse(impulse)
