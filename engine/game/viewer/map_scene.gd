# SPDX-License-Identifier: GPL-3.0-or-later
#
# The world map's 3D side, as the game draws it: the main camera moves up (MapCamera) while the world waits, and
# what it sees is the map world's whole LOD at the map's locked levels with no cells, trees or large references
# under it, lit by WorldMapWeather (DOBJ WMWE) and its time-of-day image spaces instead of the region's weather,
# with the world map's cloud bank (WRLD MODL, else meshes/sky/skyrimworldmapcloudbank.nif; measured with SkyrimRemote
# @node: at the world's origin, the model's own planes at z 0, 1000, 1500 and 12500 and scale 8, nothing fading
# with the zoom, only its UV animation running) and ISWorldMap's depth blur (map_blur.gdshader).
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
const BLUR_SHADER := preload("res://shaders/map_blur.gdshader")

var camera: MapCamera
## The map world shown (0 when closed).
var world := 0
## Off: no depth blur, clouds or object LOD (tests).
var effects := true

var _session: GameSession
var _view: Camera3D
var _lod: SkydotLod
var _weather: SkydotWeather
var _clouds: Node3D
var _blur: MeshInstance3D
var _hidden: Array[Node3D] = []
var _detached: Array[Node] = []
var _near := 0.05
var _far := 4000.0
var _was_transform := Transform3D.IDENTITY
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
	var scale := SkydotWorld.unit_scale()
	_view.near = MapCamera.NEAR_CLIP * scale
	_view.far = FAR * scale
	if effects:
		_add_blur()
	step(0.0)
	return true


func close() -> void:
	if not is_open():
		return
	world = 0
	ImageSpaceSync.weather_override = null
	for node in [_lod, _weather, _clouds, _blur]:
		if node != null and is_instance_valid(node):
			node.queue_free()
	_lod = null
	_weather = null
	_clouds = null
	_blur = null
	_view.near = _near
	_view.far = _far
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


## ISWorldMap: a 7x7 box blur of the frame whose spread grows with the distance from the focal depth (shaders/
## map_blur.gdshader), on a quad just before the camera.
func _add_blur() -> void:
	_blur = MeshInstance3D.new()
	_blur.name = "map_blur"
	var quad := QuadMesh.new()
	quad.size = Vector2(2.0, 2.0)
	quad.flip_faces = true
	_blur.mesh = quad
	var material := ShaderMaterial.new()
	material.shader = BLUR_SHADER
	var scale := SkydotWorld.unit_scale()
	material.set_shader_parameter("focal_depth", 45000.0 * scale)
	material.set_shader_parameter("metres_per_unit", scale)
	material.render_priority = Material.RENDER_PRIORITY_MIN  # before the clouds
	_blur.material_override = material
	_blur.extra_cull_margin = 16384.0
	_blur.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_view.add_child(_blur)
