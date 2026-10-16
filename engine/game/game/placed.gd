# SPDX-License-Identifier: GPL-3.0-or-later
#
# References made at run time: what PlaceAtMe puts in the world, and the model
# swaps of destruction stages. No viewer, camera or input in here. The VM owns
# the references (ids, bases, positions, saves, scripts); this builds their
# nodes from the base's model (an explosion's own, for EXPL) with
# SkydotWorld.build_model, so they have Skyrim's materials, particles and
# animators, and tells the VM when a node is up (OnLoad). An explosion
# plays its model once: the emitters run EXPLOSION_EMIT_SECONDS, the model
# stays EXPLOSION_LIFE_SECONDS (a fixed life: the animator does not report the
# length of its controllers), then the VM is told to remove the reference.
class_name PlacedRefs
extends Node3D

const EXPLOSION_EMIT_SECONDS := 1.5
const EXPLOSION_LIFE_SECONDS := 5.0

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _warn := WarnOnce.new("", true)


func _init(world: SkydotWorld, papyrus: SkydotPapyrus) -> void:
	_world = world
	_papyrus = papyrus
	name = "PlacedRefs"


## Build reference `ref` of `base` at `position` (Skyrim space) with `rotation`
## (radians). The node appears on the next frame, so the script that placed it
## has finished its step.
func place(ref: int, base: int, cell: int, position: Vector3, rotation: Vector3, scale: float,
		disabled: bool) -> void:
	_build.call_deferred(ref, base, cell, position, rotation, scale, disabled)


func _build(ref: int, base: int, cell: int, position: Vector3, rotation: Vector3, scale: float,
		disabled: bool) -> void:
	var explosion := _world.get_explosion(base)
	var path: String = explosion.get("model", "") if not explosion.is_empty() else _world.get_base(base).get("model", "")
	if path == "":
		_warn.once("nomodel%d" % base, "placed 0x%08X (base 0x%08X): the base has no model" % [ref, base])
		_papyrus.ref_built.call_deferred(ref)
		return
	var model := _world.build_model(path)
	if model == null:
		_warn.once("nobuild" + path, "placed 0x%08X: model %s is not in the pack" % [ref, path])
		return
	model.transform = SkydotWorld.skyrim_transform(position, rotation, scale)
	model.visible = not disabled
	add_child(model)
	_world.tag_runtime_ref(ref, cell, model)
	print("placed 0x%08X built: %s%s" % [ref, path, " (explosion)" if not explosion.is_empty() else ""])
	if not explosion.is_empty():
		_play_explosion(ref, model)
	_papyrus.ref_built(ref)


## The explosion's controllers and particles start from nothing, run for a
## while, then the emitters stop and the VM removes the reference.
func _play_explosion(ref: int, model: Node3D) -> void:
	for animator in model.find_children("*", "SkydotAnimator", true, false):
		animator.start_controllers()
	for emitter in model.find_children("*", "SkydotParticles", true, false):
		emitter.set_warm_start(false)
	get_tree().create_timer(EXPLOSION_EMIT_SECONDS).timeout.connect(_stop_emitters.bind(model))
	get_tree().create_timer(EXPLOSION_LIFE_SECONDS).timeout.connect(_expire.bind(ref))


func _stop_emitters(model: Node3D) -> void:
	if not is_instance_valid(model):
		return
	for emitter in model.find_children("*", "SkydotParticles", true, false):
		emitter.set_active(false)


func _expire(ref: int) -> void:
	print("placed 0x%08X ends" % ref)
	_papyrus.remove_placed(ref)


## The reference is gone (Delete, an effect that ended, a loaded save).
func remove(ref: int) -> void:
	var node := _world.find_ref_node(ref)
	if node != null and is_ancestor_of(node):
		print("placed 0x%08X removed" % ref)
		node.queue_free()


## A destruction stage's model replaces the reference's (path "": the base's
## own comes back). The node keeps its place, tags and activation bounds.
func swap_model(ref: int, model_path: String, stage: int) -> void:
	var old := _world.find_ref_node(ref) as Node3D
	if old == null:
		return
	var path := model_path
	if path == "":
		var base: int = _papyrus.get_ref_base(ref)
		path = _world.get_base(base).get("model", "")
	var fresh := _world.build_model(path) if path != "" else null
	if fresh == null:
		_warn.once("noswap%d" % ref, "0x%08X: stage model %s is not in the pack" % [ref, path])
		return
	fresh.transform = old.transform
	fresh.visible = old.visible
	for key in old.get_meta_list():
		fresh.set_meta(key, old.get_meta(key))
	var parent := old.get_parent()
	parent.add_child(fresh)
	_world.tag_runtime_ref(ref, old.get_meta("skydot_cell", 0), fresh)
	print("0x%08X model swapped to stage %d: %s" % [ref, stage, path])
	old.queue_free()
