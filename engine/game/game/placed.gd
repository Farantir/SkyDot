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

## A script-placed actor was built (a dead one lies down: Corpses.on_actor_arrived).
signal actor_built(ref: int)

const EXPLOSION_EMIT_SECONDS := 1.5
const EXPLOSION_LIFE_SECONDS := 5.0

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _ai: SkydotAi  # attaches the actors (summons) it builds; may be null
var _warn := WarnOnce.new("", true)
## Set by the session: where the place on screen is. An actor made for a cell that is not built (a quest creates a
## soldier at a marker in Whiterun) waits for it.
var place_on_screen: Place
var streamer: SkydotStreamer
var _noted := {}
var _waiting: Array[Array] = []  # [ref, base, cell, position, rotation, scale, disabled]


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, ai: SkydotAi = null) -> void:
	_world = world
	_papyrus = papyrus
	_ai = ai
	name = "PlacedRefs"


## Build reference `ref` of `base` at `position` (Skyrim space) with `rotation`
## (radians). The node appears on the next frame, so the script that placed it
## has finished its step.
func place(ref: int, base: int, cell: int, position: Vector3, rotation: Vector3, scale: float,
		disabled: bool) -> void:
	_build.call_deferred(ref, base, cell, position, rotation, scale, disabled)


func _build(ref: int, base: int, cell: int, position: Vector3, rotation: Vector3, scale: float,
		disabled: bool) -> void:
	if _world.is_actor_base(base):
		if not _cell_is_built(cell, position):
			_waiting.append([ref, base, cell, position, rotation, scale, disabled])
			if not _noted.has(ref):
				_noted[ref] = true
				print("placed 0x%08X waits for cell 0x%X (%s) at (%.0f, %.0f, %.0f)" % [ref, cell, _world.get_cell(cell).get("editor_id", ""), position.x, position.y, position.z])
			return
		_build_actor(ref, base, position, rotation, disabled)
		return
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


## A summon or another actor a script placed: a SkydotActor like the cell's, under this node, taken
## by the AI (its NPC_ comes from the reference's base).
## Whether the cell `cell` (0: the one the player is in) is built now: the interior on screen, or the exterior
## cell at `position` in the worldspace on screen.
func _cell_is_built(cell: int, position: Vector3) -> bool:
	if cell == 0 or place_on_screen == null or streamer == null:
		return true
	var info := _world.get_cell(cell)
	if info.is_empty():
		return true
	if info["interior"]:
		return streamer.world_id == 0 and place_on_screen.cell_id == cell
	return streamer.world_id != 0 and info["world"] == streamer.world_id and streamer.get_loaded_cell(
		Vector2i(floori(position.x / SkydotWorld.CELL_UNITS), floori(position.y / SkydotWorld.CELL_UNITS))) != null


## A script moved reference `ref` (MoveTo): an actor still waiting for its cell waits for the new one, and is
## built now if that is built.
func moved(ref: int, position: Vector3, rotation: Vector3) -> void:
	for i in _waiting.size():
		var entry := _waiting[i]
		if entry[0] != ref:
			continue
		entry[2] = _papyrus.get_runtime_cell(ref)
		entry[3] = position
		entry[4] = rotation
		if _cell_is_built(entry[2], position):
			_waiting.remove_at(i)
			_build(entry[0], entry[1], entry[2], entry[3], entry[4], entry[5], entry[6])
		return


## A cell was built: the actors that waited for one are built if theirs is.
func cell_built(_cell_id: int) -> void:
	var waiting := _waiting
	_waiting = []
	for entry in waiting:
		_build(entry[0], entry[1], entry[2], entry[3], entry[4], entry[5], entry[6])


func _build_actor(ref: int, base: int, position: Vector3, rotation: Vector3, disabled: bool) -> void:
	# A dead one built again (after a load) lies where it died, not where it was placed.
	var corpse := _papyrus.get_corpse_place(ref)
	if not corpse.is_empty() and _papyrus.is_actor_dead(ref):
		position = corpse["position"]
		rotation.z = corpse["rot_z"]
	var node := _world.build_placed_actor(ref, base, position, rotation.z)
	if node == null or node.get_child_count() == 0:
		_warn.once("noactor%d" % base, "placed 0x%08X: the actor of base 0x%08X does not build" % [ref, base])
		if node != null:
			node.queue_free()
		_papyrus.ref_built.call_deferred(ref)
		return
	node.visible = not disabled
	add_child(node)
	print("placed 0x%08X built: actor of 0x%08X" % [ref, base])
	if _ai != null:
		_ai.attach_built(node)
	_papyrus.ref_built(ref)
	actor_built.emit(ref)


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
	_waiting = _waiting.filter(func(entry: Array) -> bool: return entry[0] != ref)
	var node := _world.find_ref_node(ref)
	if node != null and is_ancestor_of(node):
		print("placed 0x%08X removed" % ref)
		# An actor node is the child of a root of its own; both go.
		var gone: Node = node
		if node is SkydotActor and node.get_parent() != self:
			gone = node.get_parent()
		gone.queue_free()


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
