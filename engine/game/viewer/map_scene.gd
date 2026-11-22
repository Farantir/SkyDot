# SPDX-License-Identifier: GPL-3.0-or-later
#
# The world map's 3D side, as the game draws it: the main camera moves up (MapCamera) while the world waits, and
# what it sees is the map world's whole LOD at the map's locked levels with no cells, trees or large references
# under it, lit by WorldMapWeather (DOBJ WMWE) and its time-of-day image spaces instead of the region's weather,
# with the world map's cloud bank (WRLD MODL, else meshes/sky/skyrimworldmapcloudbank.nif; measured with SkyrimRemote
# @node: at the world's origin, the model's own planes at z 0, 1000, 1500 and 12500 and scale 8, nothing fading
# with the zoom, only its UV animation running). No ISWorldMap depth blur: the game's map frame is sharper (mean
# horizontal gradient 3.48 in game_map_default) than ours drawn without any blur (2.40); a 7x7 box blur as
# Community Shaders' ISWorldMap.hlsl has it with the [MapMenu] defaults halved ours (1.08).
# Shell code both shells use: `open` hides the place's nodes (a weather or an interior's environment is taken out
# of the tree so the map's light and sky are the only ones; out of the tree its clock stands still), `close` puts
# everything back and frees what the map loaded.
class_name MapScene
extends Node3D

## The map's locked LOD levels ([MapMenu] uLockedTerrainLOD, uLockedObjectMapLOD).
const TERRAIN_LEVEL := 32
const OBJECT_LEVEL := 16
## The map camera's far plane (measured: the NiCamera's frustum far 353840 with the map open).
const FAR := 353840.0

var camera: MapCamera
## The map world shown (0 when closed).
var world := 0
## Off: no clouds or object LOD (tests).
var effects := true
## A tracked head (the VR shell): the camera is not ours to place, so this node (the XR origin) is moved and
## turned instead, to put the head where the map camera stands looking its way; the head then looks around from
## there (Skyrim VR floats the player above the map). Null: the camera itself is placed (the flat viewer).
var origin: Node3D

var _session: GameSession
var _view: Camera3D
var _lod: SkydotLod
var _weather: SkydotWeather
var _clouds: Node3D
var _hidden: Array[Node3D] = []
var _detached: Array[Node] = []
var _near := 0.05
var _far := 4000.0
var _was_transform := Transform3D.IDENTITY
var _was_origin := Transform3D.IDENTITY
var _settled := false


func _init(session: GameSession, view: Camera3D) -> void:
	name = "MapScene"
	_session = session
	_view = view


func is_open() -> bool:
	return world != 0


## Show the map of `place` ({world, position}: WorldMap.map_place). False when there is none.
func open(place: Dictionary) -> bool:
	if is_open() or place.is_empty():
		return false
	world = int(place["world"])
	_settled = false
	var info := _session.world_map.world_info(world)
	_hide_place()
	_lod = SkydotLod.new()
	_lod.name = "map_lod"
	_lod.large_refs = false
	if _lod.setup(_session.pack, _session.world, world) != OK:
		print("map: no LOD for 0x%X: %s" % [world, _lod.get_error()])
		_lod.free()
		_lod = null
	else:
		_lod.set_map_levels(TERRAIN_LEVEL, OBJECT_LEVEL if effects else 0)
		add_child(_lod)
		_lod.update(Vector3.ZERO, 1 << 40)  # everything ready now (the LOD's scenes are cached), else as it loads
	_add_weather()
	if effects:
		_add_clouds(String(info.get("cloud_model", "")))
	camera = MapCamera.new()
	camera.configure(info)
	camera.height_at = func(x: float, y: float) -> float: return _lod.get_terrain_height(x, y) if _lod != null else NAN
	camera.open(place["position"])
	_near = _view.near
	_far = _view.far
	_was_transform = _view.global_transform
	if origin != null:
		_was_origin = origin.global_transform
	var scale := SkydotWorld.unit_scale()
	_view.near = MapCamera.NEAR_CLIP * scale
	_view.far = FAR * scale
	step(0.0)
	return true


func close() -> void:
	if not is_open():
		return
	world = 0
	ImageSpaceSync.weather_override = null
	for node in [_lod, _weather, _clouds]:
		if node != null and is_instance_valid(node):
			node.queue_free()
	_lod = null
	_weather = null
	_clouds = null
	_view.near = _near
	_view.far = _far
	if origin != null:
		origin.global_transform = _was_origin
	else:
		_view.global_transform = _was_transform
	_show_place()
	_session.world.call_deferred("trim_cache")


## A frame with the map open: the camera's ground follows the land, the LOD builds what has loaded.
func step(delta: float) -> void:
	if not is_open():
		return
	if _lod != null:
		_lod.update(_view.global_position, 8000)
		if not _settled and not is_nan(_lod.get_terrain_height(camera.position.x, camera.position.y)):
			# The LOD under the camera has loaded: the opening pose on its ground (MapCamera.open).
			_settled = true
			camera.open(camera.target)
	camera.step(delta)
	if origin != null:
		# The head's pose in the origin stays the player's; the origin goes where that puts the head on the map
		# camera's position, facing its way.
		var head := origin.global_transform.affine_inverse() * _view.global_transform
		var wanted := camera.transform()
		# The whole view is tilted by the map camera's pitch too, so looking straight ahead looks down at the
		# map as the flat view does (the head's own tilt adds to it).
		origin.global_transform = wanted * Transform3D(Basis.IDENTITY, -head.origin)
	else:
		_view.global_transform = camera.transform()


## Where `position` (game units) is on the view (pixels), null behind the camera.
func project(position: Vector3) -> Variant:
	var p := SkydotWorld.skyrim_position(position)
	if _view.is_position_behind(p):
		return null
	return _view.unproject_position(p)


## The land under view pixel `at` (game units): the ray through it walked onto the LOD's surface; null when it
## misses the map.
func ground_point(at: Vector2) -> Variant:
	var origin := SkydotWorld.godot_to_skyrim(_view.project_ray_origin(at))
	var dir := SkydotWorld.godot_to_skyrim(_view.project_ray_origin(at) + _view.project_ray_normal(at)) - origin
	if dir.z >= -1e-6:
		return null
	var height := camera.ground
	var point := origin
	for i in 12:
		point = origin + dir * ((height - origin.z) / dir.z)
		var h: float = camera.height_at.call(point.x, point.y)
		if is_nan(h):
			break
		height = h
	return point


## What the map draws while open, for a shot's JSON and the tests.
func describe() -> Dictionary:
	if not is_open():
		return {}
	return {"world": world, "camera": camera.position, "target": camera.target, "zoom": camera.zoom,
		"yaw": camera.yaw, "pitch": camera.pitch, "ground": camera.ground,
		"lod": _lod.get_stats() if _lod != null else {},
		"weather": _weather.get_weather() if _weather != null else 0,
		"image_space": (_weather.get_image_space() if _weather != null else {}).keys()}


## The place's nodes out of sight: cells, its LOD and large references hidden; its weather or an interior's
## environment out of the tree (a second WorldEnvironment would be ignored; its time stands still meanwhile).
func _hide_place() -> void:
	var host := _session.host
	for child in host.get_children():
		if child is SkydotWeather or child is WorldEnvironment or _holds_environment(child):
			_detached.append(child)
		elif child is Node3D and (child as Node3D).visible:
			(child as Node3D).visible = false
			_hidden.append(child)
	for child in _detached:
		host.remove_child(child)


func _show_place() -> void:
	for child in _detached:
		if is_instance_valid(child):
			_session.host.add_child(child)
	_detached.clear()
	for child in _hidden:
		if is_instance_valid(child):
			child.visible = true
	_hidden.clear()


static func _holds_environment(node: Node) -> bool:
	for child in node.get_children():
		if child is WorldEnvironment:
			return true
	return false


## WorldMapWeather (DOBJ WMWE) at the clock's hour, standing still.
func _add_weather() -> void:
	var weather := _session.world.get_default_object("WMWE")
	_weather = SkydotWeather.new()
	_weather.name = "map_weather"
	_session.clock.configure(_weather)
	_weather.auto_weather = false
	_weather.shadows = false
	add_child(_weather)
	if _weather.setup(_session.world, world, _view) != OK:
		print("map: no weather for 0x%X" % world)
		_weather.queue_free()
		_weather = null
		return
	_weather.time_scale = 0.0
	if weather != 0:
		_weather.set_weather(weather, 0.0)
	ImageSpaceSync.weather_override = _weather


## The cloud bank where the model puts it: at the world's origin (measured).
func _add_clouds(model: String) -> void:
	if model.is_empty():
		return
	_clouds = _session.world.build_model(model)
	if _clouds == null:
		print("map: no cloud bank ", model)
		return
	_clouds.name = "map_clouds"
	add_child(_clouds)
	# Lit by the weather's sunlight (8:01: 0.66/0.52/0.38) and faded by its soft depth (7000 units, the valleys
	# lie that close under the planes) the bank came out a few faint brown wisps; the game's (game_map_default,
	# game_map_out) is bright white and covers the valleys in large patches, as it does unlit and without the soft
	# fade (side4_default). How the game lights it on the map is not known.
	for mi in _clouds.find_children("*", "MeshInstance3D", true, false):
		var m := (mi as MeshInstance3D).get_active_material(0) as ShaderMaterial
		if m != null:
			m.set_shader_parameter("lighting_influence", 0.0)
			m.set_shader_parameter("soft_effect", false)
