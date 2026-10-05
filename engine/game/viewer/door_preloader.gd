# SPDX-License-Identifier: GPL-3.0-or-later
#
# Near a load door (within `distance` metres) the place behind it is built
# ahead, a little each frame while nothing streams here: the destination's
# resources on the loader's threads, then its references (not its actors) in
# the scene but held (HeldPlace). Going through that door then only adds the
# actors. One place is kept; walking away from the door drops it. It costs
# the memory of a second place.
class_name DoorPreloader
extends RefCounted

const BUDGET_USEC := 4000  # per frame
const SCAN_FRAMES := 15  # between looks for the nearest door

## The place behind a door, built ahead, and how far along it is.
class Preparation:
	## The door it was started for, and the door on the other side.
	var door := 0
	var destination := 0
	var done := false
	var started_usec := 0
	## An interior: its record and its root (null until begun).
	var interior := false
	var cell := 0
	var root: Node3D
	## An exterior: the worldspace, the camera's eye on arrival, the cells to
	## build nearest first, those begun (Vector2i -> Node3D, null where nothing
	## is), those whose references are all placed (a set) and the LOD.
	var world := 0
	var eye := Vector3.ZERO
	var keys: Array[Vector2i] = []
	var cells := {}
	var placed := {}
	var lod: SkydotLod

## Build ahead at all; O toggles it.
var enabled := true
## How near the feet must be to a load door, in metres.
var distance := 15.0
## Door reference -> its node, in the place shown.
var load_doors := {}
## The place behind the nearest load door; null if none is being built.
var current: Preparation
var _scan := 0  # frames until looking for the nearest door again
var _world: SkydotWorld
var _host: Node3D  # held places are added here
var _streamer: WorldStreamer
var _ai: SkydotAi  # null with --ai off


func _init(world: SkydotWorld, host: Node3D, streamer: WorldStreamer, ai: SkydotAi,
		settings: ViewerSettings) -> void:
	_world = world
	_host = host
	_streamer = streamer
	_ai = ai
	enabled = settings.preload_doors
	distance = settings.preload_distance


## Remember the load doors among `nodes`, the references just built.
func register(nodes: Array[Node]) -> void:
	for node in nodes:
		var ref: int = node.get_meta("skydot_ref")
		if not _world.get_door(ref).is_empty():
			load_doors[ref] = node


## The place changed: its doors are gone.
func clear_doors() -> void:
	load_doors.clear()


## O: returns true when building ahead.
func toggle() -> bool:
	enabled = not enabled
	if not enabled:
		drop()
	return enabled


## Every frame: look for the nearest door now and then, and build a little
## of the place behind it. `feet` is the player's position.
func step(feet: Vector3) -> void:
	_scan -= 1
	if _scan <= 0:
		_scan = SCAN_FRAMES
		var nearest := 0
		var nearest_distance := distance
		var door: int = current.door if current != null else 0
		var current_distance := INF
		for ref in load_doors.keys():
			var node: Node3D = load_doors[ref]
			if not is_instance_valid(node) or node.is_queued_for_deletion():
				load_doors.erase(ref)
				continue
			var d := node.global_position.distance_to(feet)
			if ref == door:
				current_distance = d
			if d < nearest_distance:
				nearest = ref
				nearest_distance = d
		if nearest != 0 and nearest != door:
			drop()
			current = _begin(_world.get_door(nearest))
		elif nearest == 0 and door != 0 and current_distance > distance * 1.5:
			drop()
	if current == null or current.done or _streamer.streaming:
		return
	_advance(BUDGET_USEC)


## Going through `door`: what was built ahead for it, handed over (null if
## nothing, or for another door); the rest is dropped.
func take(door: Dictionary) -> Preparation:
	var taken: Preparation = null
	if current != null and current.destination == door["destination"]:
		taken = current
		current = null
		print("using what was prepared (%s)" % ("complete" if taken.done else "in part"))
	drop()
	return taken


## Free what was built ahead.
func drop() -> void:
	var p := current
	current = null
	if p == null:
		return
	if p.root != null:
		p.root.queue_free()
	for cell in p.cells.values():
		if cell != null:
			cell.queue_free()
	if p.lod != null:
		p.lod.queue_free()


func _begin(door: Dictionary) -> Preparation:
	var p := Preparation.new()
	p.door = door["ref"]
	p.destination = door["destination"]
	p.started_usec = Time.get_ticks_usec()
	if _ai != null:
		_ai.begin_placing()  # where actors are, worked out over the next frames
	if door["destination_interior"]:
		p.interior = true
		p.cell = door["destination_cell"]
		return p
	if door["destination_world"] == 0:
		return null
	p.world = door["destination_world"]
	var arrival: Transform3D = door["arrival"]
	p.eye = arrival.origin + Vector3(0, PlayerRig.EYE_HEIGHT, 0)
	var centre := WorldStreamer.cell_at(arrival.origin)
	for dy in range(-_streamer.radius, _streamer.radius + 1):
		for dx in range(-_streamer.radius, _streamer.radius + 1):
			p.keys.append(centre + Vector2i(dx, dy))
	p.keys.sort_custom(func(a: Vector2i, b: Vector2i) -> bool:
		return (a - centre).length_squared() < (b - centre).length_squared())
	p.lod = _streamer.make_lod(p.world)
	if p.lod != null:
		HeldPlace.hold(_host, p.lod)
	return p


func _advance(budget_usec: int) -> void:
	var started := Time.get_ticks_usec()
	var p := current
	if p.interior:
		if p.root == null:
			if _world.request_cell(p.cell) > 0:
				return
			p.root = _world.begin_cell(p.cell)
			if p.root != null:
				HeldPlace.hold(_host, p.root)
		p.done = _world.continue_build_static(p.root, budget_usec)
	else:
		var complete := true
		for key in p.keys:
			var left := budget_usec - (Time.get_ticks_usec() - started)
			if left <= 0:
				complete = false
				break
			if p.placed.has(key):
				continue
			if not p.cells.has(key):
				if _world.request_exterior(p.world, key.x, key.y) != 0:
					complete = false
					continue
				p.cells[key] = _world.begin_exterior(p.world, key.x, key.y)
				if p.cells[key] != null:
					HeldPlace.hold(_host, p.cells[key])
			var cell: Node3D = p.cells[key]
			if cell == null or _world.continue_build_static(cell, left):
				p.placed[key] = true
			else:
				complete = false
		if p.lod != null and p.lod.update(p.eye, WorldStreamer.LOD_BUDGET_USEC) > 0:
			complete = false
		p.done = complete
	if p.done:
		print("prepared what is behind 0x%08X in %.0f ms" % [p.door, (Time.get_ticks_usec() - p.started_usec) / 1000.0])
