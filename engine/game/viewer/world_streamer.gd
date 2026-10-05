# SPDX-License-Identifier: GPL-3.0-or-later
#
# Streams a worldspace's exterior cells around the camera: drops those out of
# range, loads those in range nearest first (their scenes and textures on the
# asset cache's threads, the build itself here in steps within a frame
# budget) and keeps the worldspace's LOD running beyond them. A cell stays
# hidden until its build is done, then shows, masks its LOD and is announced
# (cell_finished) so the viewer can attach its scripts.
class_name WorldStreamer
extends RefCounted

const CELL_UNITS := SkydotWorld.CELL_UNITS  # game units along a cell, as the converter uses
const LOD_BUDGET_USEC := 3000  # per frame for the LOD

## `cell` was built and is shown; `cell_id` is its record. The streamer has
## already attached its AI actors.
signal cell_finished(cell: Node3D, cell_id: int)

## The worldspace being streamed, or 0 inside.
var world_id := 0
## Cells around the camera built in full; keys change it.
var radius := 2
## SkydotLod.split_distance for every LOD made; keys change it.
var lod_split := 1.5
## The worldspace's LOD, or null.
var lod: SkydotLod
## Vector2i -> Node3D (null if nothing is there)
var loaded := {}
## Vector2i -> Node3D built in steps, hidden until done
var building := {}
## Exterior cells in range were still loading at the last update.
var streaming := false
## The LOD still had work queued at the last update.
var lod_busy := false
## The slowest update in microseconds, for the benchmark.
var max_usec := 0
var build_budget_usec := 8000  # per frame for streaming cells in
var _world: SkydotWorld
var _pack: SkydotPack
var _settings: ViewerSettings
var _host: Node3D  # cells are added here
var _camera: Camera3D
var _ai: SkydotAi  # null with --ai off
var _unit_scale := SkydotWorld.unit_scale()  # metres per game unit


func _init(world: SkydotWorld, pack: SkydotPack, host: Node3D, camera: Camera3D, ai: SkydotAi,
		settings: ViewerSettings) -> void:
	_world = world
	_pack = pack
	_host = host
	_camera = camera
	_ai = ai
	_settings = settings
	radius = settings.radius
	lod_split = settings.lod_split
	build_budget_usec = settings.build_budget_usec


## The grid cell of a position in the engine's space.
static func cell_at(position: Vector3) -> Vector2i:
	var p := position / SkydotWorld.unit_scale()
	return Vector2i(floori(p.x / CELL_UNITS), floori(-p.z / CELL_UNITS))


## The camera's cell in the worldspace grid.
func camera_cell() -> Vector2i:
	var p := _camera.position / _unit_scale
	return Vector2i(floori(p.x / CELL_UNITS), floori(-p.z / CELL_UNITS))


## How far the camera sees without LOD: past the cells in range.
func view_distance() -> float:
	return (radius + 1) * CELL_UNITS * _unit_scale * 1.5


## Stream `id` from now on (after clear).
func start(id: int) -> void:
	world_id = id


## Free every cell and forget the worldspace (its LOD is freed with the place).
func clear() -> void:
	for key in loaded:
		if loaded[key] != null:
			loaded[key].queue_free()
	loaded.clear()
	for key in building:
		building[key].queue_free()
	building.clear()
	world_id = 0
	lod = null
	streaming = false
	lod_busy = false


## The worldspace's LOD, set up as the options say; null if it has none or
## --lod off.
func make_lod(id: int) -> SkydotLod:
	if not _settings.lod:
		return null
	var made := SkydotLod.new()
	if made.setup(_pack, _world, id) != OK:
		print("no LOD: ", made.get_error())
		made.free()
		return null
	made.name = "lod"
	made.split_distance = lod_split
	if _settings.has_tree_distance:
		made.tree_distance = _settings.tree_distance
	return made


## Cells built ahead (DoorPreloader) join the streaming ones; each is released
## when it is finished, so their cost is spread over frames, but the one under
## the camera at once, for the ground. Null cells are known to be empty.
func adopt(cells: Dictionary) -> void:
	var centre := camera_cell()
	for key in cells:
		var cell: Node3D = cells[key]
		if cell == null:
			loaded[key] = null
			continue
		if key == centre:
			HeldPlace.release(cell)
			cell.visible = false  # until finished
		building[key] = cell


## Every frame in a worldspace: stream, then the LOD.
func update() -> void:
	if world_id == 0:
		return
	var started := Time.get_ticks_usec()
	streaming = step()
	if lod != null:
		lod_busy = lod.update(_camera.global_position, LOD_BUDGET_USEC) > 0
	max_usec = max(max_usec, Time.get_ticks_usec() - started)


## Build everything in range and the whole LOD now, waiting for the loaders
## (a screenshot or benchmark wants a complete world).
func load_everything() -> void:
	while step():
		OS.delay_msec(5)
	while lod != null and lod.update(_camera.global_position, 1000000) > 0:
		OS.delay_msec(5)


## Drop cells out of range, then load cells in range nearest first. Returns
## false when every cell in range is loaded.
func step() -> bool:
	var centre := camera_cell()
	var dropped := false
	for key in loaded.keys():
		var offset: Vector2i = key - centre
		if max(abs(offset.x), abs(offset.y)) > radius + 1:
			if loaded[key] != null:
				loaded[key].queue_free()
				dropped = true
				if lod != null:
					lod.set_cell_loaded(key.x, key.y, false)
			loaded.erase(key)
	for key in building.keys():
		var offset: Vector2i = key - centre
		if max(abs(offset.x), abs(offset.y)) > radius + 1:
			building[key].queue_free()
			building.erase(key)
	if dropped:
		_world.call_deferred("trim_cache")

	var nearest := func(a: Vector2i, b: Vector2i) -> bool:
		return (a - centre).length_squared() < (b - centre).length_squared()
	var started := Time.get_ticks_usec()
	var order: Array = building.keys()
	order.sort_custom(nearest)
	for key in order:
		var left := build_budget_usec - (Time.get_ticks_usec() - started)
		if left <= 0:
			return true
		if _world.continue_build(building[key], left):
			_finish_cell(key, building[key])
			building.erase(key)

	var wanted: Array[Vector2i] = []
	for dy in range(-radius, radius + 1):
		for dx in range(-radius, radius + 1):
			var key := centre + Vector2i(dx, dy)
			if not loaded.has(key) and not building.has(key):
				wanted.append(key)
	if wanted.is_empty():
		return not building.is_empty()
	wanted.sort_custom(nearest)

	for key in wanted.slice(0, 4):
		if _world.request_exterior(world_id, key.x, key.y) != 0:
			continue
		var left := build_budget_usec - (Time.get_ticks_usec() - started)
		if left <= 0:
			break
		var cell := _world.begin_exterior(world_id, key.x, key.y)
		if cell == null:
			loaded[key] = null
			continue
		cell.visible = false
		_host.add_child(cell)
		if _world.continue_build(cell, left):
			_finish_cell(key, cell)
		else:
			building[key] = cell
	return true


func _finish_cell(key: Vector2i, cell: Node3D) -> void:
	if cell.process_mode == Node.PROCESS_MODE_DISABLED:
		HeldPlace.release(cell)  # prepared behind a door
	cell.visible = true
	if _ai != null:
		_ai.attach_built(cell)
	print("cell ", key, ": ", cell.get_meta("skydot_stats"))
	cell_finished.emit(cell, _world.get_exterior_cell(world_id, key.x, key.y))
	if lod != null:
		lod.set_cell_loaded(key.x, key.y, true)
	loaded[key] = cell


## Hold the player while the ground under it is still being built (and while
## `fading` through a door), and give it the water level of the cell it is in.
func hold_player(player: SkydotPlayer, fading: bool) -> void:
	if world_id != 0:
		var key := camera_cell()
		var cell = loaded.get(key)
		player.hold = (not loaded.has(key) and not player.fly) or fading
		var water: Node3D = cell.get_node_or_null("Water") if cell != null else null
		if water != null:
			player.water_height = water.global_position.y
		else:
			player.clear_water()
	else:
		player.hold = fading
		player.clear_water()


## [ and ]: finer or coarser LOD. Returns what to tell the user.
func scale_lod_split(factor: float) -> String:
	lod_split = clampf(lod_split * factor, 0.5, 16.0)
	if lod != null:
		lod.split_distance = lod_split
	# A level-8 quad splits into level-4 ones within split times 8 cells.
	return "LOD detail %.2f (finest LOD within %.0f m)" % [lod_split, lod_split * 8 * CELL_UNITS * _unit_scale]


## - and =: more or fewer cells in full detail. Returns what to tell the user.
func change_radius(by: int) -> String:
	radius = clampi(radius + by, 1, 8)
	if lod == null:
		_camera.far = view_distance()
	return "full detail within %d cells (%d x %d)" % [radius, 2 * radius + 1, 2 * radius + 1]
