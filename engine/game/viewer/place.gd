# SPDX-License-Identifier: GPL-3.0-or-later
#
# The place being shown: an interior, or a worldspace streamed around the
# camera. It builds a place (the cell or the sky, light and LOD, with
# SkydotWeather outside), puts the camera in it and removes the place when the
# next one is entered. What the place consists of is `nodes`; who needs to
# know that a cell was built hears `built`.
class_name Place
extends RefCounted

## A cell of the new place was built: its root and record (0 for none). The
## AI actors are attached; scripts and overlays are the listener's.
signal built(root: Node, cell_id: int)
## The old place is about to go.
signal left
## Something to tell the user.
signal message(text: String)
## The place cannot be entered; the run is over.
signal failed(text: String)

## Everything the current cell or worldspace added.
var nodes: Array[Node] = []
## The interior being shown, or 0 outside.
var cell_id := 0
## Inside: the cell's image space (IMGS) record, if it has one.
var interior_image_space := {}
var _world: SkydotWorld
var _host: Node3D  # the place's nodes are added here
var _settings: ViewerSettings
var _rig: PlayerRig
var _ai: SkydotAi  # null with --ai off
var _clock: SkydotClock
var _streamer: WorldStreamer
var _preloader: DoorPreloader


func _init(world: SkydotWorld, host: Node3D, settings: ViewerSettings, rig: PlayerRig, ai: SkydotAi,
		clock: SkydotClock, streamer: WorldStreamer, preloader: DoorPreloader) -> void:
	_world = world
	_host = host
	_settings = settings
	_rig = rig
	_ai = ai
	_clock = clock
	_streamer = streamer
	_preloader = preloader


## The reference nodes under `root` (not those inside another reference).
static func ref_nodes(root: Node) -> Array[Node]:
	var out: Array[Node] = []
	var pending: Array[Node] = [root]
	while not pending.is_empty():
		for child in pending.pop_back().get_children():
			if child.has_meta("skydot_ref"):
				out.append(child)
			elif child.get_child_count() > 0:
				pending.append(child)
	return out


static func mesh_bounds(root: Node) -> AABB:
	var bounds := AABB()
	var first := true
	for node in root.find_children("*", "MeshInstance3D", true, false):
		var mesh := node as MeshInstance3D
		var box := mesh.global_transform * mesh.get_aabb()
		if first:
			bounds = box
			first = false
		else:
			bounds = bounds.merge(box)
	return bounds


## Add `node` to the place (and to the scene, unless it already is).
func add(node: Node) -> void:
	if node.get_parent() == null:
		_host.add_child(node)
	nodes.append(node)


## Remove the current cell or worldspace.
func leave() -> void:
	left.emit()
	interior_image_space = {}
	_clock.release_weather()
	for node in nodes:
		if is_instance_valid(node):
			node.queue_free()
	nodes.clear()
	_streamer.clear()  # its LOD is freed with the nodes
	_preloader.clear_doors()
	_world.call_deferred("trim_cache")


## Build an interior and put the camera at `at` (a camera position) looking at
## `target`, or at eye height in the middle of the cell when `at` is null.
## `prepared` is the cell built ahead without its actors (DoorPreloader).
func enter_interior(id: int, at: Variant, target: Variant, prepared: Node3D = null) -> void:
	leave()
	cell_id = id
	var cell := _world.get_cell(id)
	if _ai != null:
		_ai.set_space(id)
		_ai.settle_actors()  # a pass begun when preparing, or a full one
	var root := prepared
	if root != null:
		HeldPlace.release(root)
		_world.continue_build(root, 1 << 62)  # the actors, where they are now
	else:
		root = _world.build_cell(id)
	add(root)
	if _ai != null:
		_ai.attach_built(root)
	print("cell %s: %s" % [cell["editor_id"], root.get_meta("skydot_stats")])
	built.emit(root, id)
	_add_environment(cell)
	_rig.camera.far = 500.0
	var spawn = _interior_spawn(id) if at == null and not _rig.player.fly else null
	if spawn != null:
		var eye: Vector3 = spawn.origin + Vector3(0, PlayerRig.EYE_HEIGHT, 0)
		var forward: Vector3 = -spawn.basis.z
		forward.y = 0.0
		_rig.place_camera(eye, eye + (forward.normalized() if forward.length() > 0.001 else Vector3.FORWARD))
	elif at == null:
		if not _rig.player.fly:
			_rig.player.fly = true
			print("no door leads into %s: flying (V walks)" % cell["editor_id"])
		var bounds := mesh_bounds(root)
		print("bounds: ", bounds)
		var eye := bounds.get_center()
		eye.y = bounds.position.y + min(PlayerRig.EYE_HEIGHT, bounds.size.y * 0.5)
		_rig.place_camera(eye, null)
		_rig.apply_look(0.0, 0.0)
	else:
		_rig.place_camera(at, target)
	print("entered ", cell["editor_id"])


## Stream worldspace `world_id` around `at`, looking at `target` (null keeps
## the current direction). `prepared` holds cells and LOD built ahead
## (DoorPreloader); they finish with their actors as streamed cells do.
## Returns false if it failed.
func enter_exterior(world_id: int, at: Vector3, target: Variant,
		prepared: DoorPreloader.Preparation = null) -> bool:
	leave()
	_streamer.start(world_id)
	cell_id = 0
	if _ai != null:
		_ai.set_space(world_id)
		_ai.settle_actors()  # a pass begun when preparing, or a full one
	var weather := 0
	if not _settings.weather.is_empty():
		weather = _world.find_weather(_settings.weather)
		if weather == 0:
			failed.emit("no weather named " + _settings.weather)
			return false
	if not _start_weather(weather):
		_add_sky(_world.get_sky(_streamer.world_id, _clock.hour, weather), _settings.shadows)
	_rig.camera.far = _streamer.view_distance()
	_rig.place_camera(at, target)
	var lod: SkydotLod = prepared.lod if prepared != null else null
	if lod == null:
		lod = _streamer.make_lod(world_id)
	if lod != null:
		HeldPlace.release(lod)
		add(lod)
		_streamer.lod = lod
		_rig.camera.far = 40000.0
	_streamer.adopt(prepared.cells if prepared != null else {})
	for w in _world.list_worlds():
		if w["id"] == world_id:
			print("entered ", w["editor_id"])
	return true


## An actor the AI brought into the place on screen: build it where its
## place now is, if that part of the world is built.
func on_actor_arrived(ref: int) -> void:
	var where := _world.get_actor_place(ref)
	if where.is_empty():
		return
	var parent: Node = null
	if _streamer.world_id == 0:
		if where["space"] == cell_id and not nodes.is_empty():
			parent = nodes[0]
	elif where["space"] == _streamer.world_id:
		var p: Vector3 = where["position"]
		parent = _streamer.loaded.get(Vector2i(floori(p.x / WorldStreamer.CELL_UNITS),
			floori(p.y / WorldStreamer.CELL_UNITS)))
	if parent == null:
		return
	var node := _world.build_actor(ref)
	if node == null:
		return
	parent.add_child(node)
	if _ai != null:
		_ai.attach_built(node)


## Where the game puts the player entering interior `id`: the arrival spot of
## a door elsewhere that leads to one of its doors, or null.
func _interior_spawn(id: int):
	for ref in _world.get_refs(id):
		var door := _world.get_door(ref["id"])
		if door.is_empty():
			continue
		var back := _world.get_door(door["destination"])
		if not back.is_empty() and back["destination_cell"] == id:
			return back["arrival"]
	return null


## Time and weather outside (SkydotWeather): the climate's or region's
## weathers in turn, unless `weather` fixes one. False if the worldspace has
## no climate.
func _start_weather(weather: int) -> bool:
	var node := SkydotWeather.new()
	node.name = "weather"
	_clock.configure(node)  # hour, day and speed
	node.shadows = _settings.shadows
	if weather != 0:
		node.auto_weather = false
	add(node)
	if node.setup(_world, _streamer.world_id, _rig.camera) != OK:
		nodes.erase(node)
		node.queue_free()
		return false
	if weather != 0:
		node.set_weather(weather, 0.0)
	_clock.bind_weather(node)
	node.weather_changed.connect(func(_id: int) -> void:
		message.emit("weather: " + str(node.get_state()["editor_id"])))
	print("weather: ", node.get_state()["editor_id"])
	return true


## Sky colours, sun or moon, ambient light and depth fog from get_sky; a
## neutral daylight setup if the worldspace has no climate.
func _add_sky(sky_values: Dictionary, shadows: bool) -> void:
	var material := ProceduralSkyMaterial.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = Sky.new()
	env.sky.sky_material = material
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = shadows
	if sky_values.is_empty():
		env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
		sun.rotation_degrees = Vector3(-40, 30, 0)
	else:
		print("sky: ", sky_values["weather"], ", daylight ", sky_values["daylight"])
		material.sky_top_color = sky_values["sky_upper"]
		material.sky_horizon_color = sky_values["horizon"]
		material.ground_horizon_color = sky_values["horizon"]
		material.ground_bottom_color = sky_values["sky_lower"]
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_color = sky_values["ambient"]
		SkydotMaterials.set_game_light(sun, sky_values["sunlight"])
		var towards: Vector3 = sky_values["sun_direction"]
		sun.look_at_from_position(Vector3.ZERO, -towards,
			Vector3.UP if abs(towards.y) < 0.99 else Vector3.FORWARD)
		if sky_values.has("fog_far"):
			env.fog_enabled = true
			env.fog_mode = Environment.FOG_MODE_DEPTH
			env.fog_depth_begin = sky_values["fog_near"]
			env.fog_depth_end = sky_values["fog_far"]
			env.fog_depth_curve = sky_values["fog_power"]
			env.fog_density = sky_values["fog_max"]
			env.fog_light_color = sky_values["fog_far_color"]
			env.set_meta("skydot_fog_near_color", sky_values["fog_near_color"])
			env.fog_sky_affect = 0.0
	var node := WorldEnvironment.new()
	node.environment = env
	add(node)
	add(sun)


func _add_environment(cell: Dictionary) -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.02, 0.02, 0.02)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR  # the camera's image space grades it
	var lighting = cell.get("lighting")
	# Our shaders light with this (SkydotMaterials.sync_fog); XCLL's ambient
	# colour is the fallback.
	if cell.get("directional_ambient", []).size() == 6:
		env.set_meta("skydot_directional_ambient", cell["directional_ambient"])
	interior_image_space = _world.get_image_space(cell.get("image_space", 0))
	if lighting != null:
		env.ambient_light_color = lighting["ambient"]
		env.ambient_light_energy = 1.0
		if lighting["fog_far"] > lighting["fog_near"]:
			# Our shaders compute the game's fog from these (SkydotMaterials.sync_fog).
			env.fog_enabled = true
			env.fog_mode = Environment.FOG_MODE_DEPTH
			env.fog_depth_begin = lighting["fog_near"]
			env.fog_depth_end = lighting["fog_far"]
			env.fog_depth_curve = lighting["fog_power"] if lighting["fog_power"] > 0.0 else 1.0
			env.fog_density = lighting["fog_max"] if lighting["fog_max"] > 0.0 else 1.0
			env.fog_light_color = lighting["fog_far_color"]
			env.set_meta("skydot_fog_near_color", lighting["fog_near_color"])
	else:
		env.ambient_light_color = Color(0.3, 0.3, 0.3)
	var node := WorldEnvironment.new()
	node.environment = env
	add(node)

	if lighting != null and lighting["directional"] != Color(0, 0, 0):
		var sun := DirectionalLight3D.new()
		SkydotMaterials.set_game_light(sun, lighting["directional"])
		sun.rotation_degrees = Vector3(-float(lighting["directional_rotation_z"]),
			float(lighting["directional_rotation_xy"]), 0)
		add(sun)
