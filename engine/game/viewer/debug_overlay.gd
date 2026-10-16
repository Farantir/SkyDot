# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the viewer shows about itself: the notes at the top left (quest and
# script messages, hints), the journal (J), the navmesh overlay (N), a path
# from the feet to the view (G), the camera's position (P) and what the
# nearest actor does (I), and a line of frame rate, frame time and draw
# counts at the top right (F3). A CanvasLayer, so one `visible` hides the text for a
# shot; the 3D parts are added to `host`.
class_name DebugOverlay
extends CanvasLayer

const NOTE_SECONDS := 8.0
const NOTE_LINES := 6

## F3 toggles the performance line.
var show_perf := true
## The last window's numbers: fps, frame_ms_mean, frame_ms_max, draw_calls,
## primitives, objects. Empty until a window has passed.
var perf := {}

## N toggles the navmesh overlay.
var show_navmesh := false

var _host: Node3D
var _world: SkydotWorld
var _session: GameSession
var _ai: SkydotAi  # null with --ai off
var _camera: Camera3D
var _player: SkydotPlayer
var _notes: Label  # recent quest and script messages
var _journal: Label  # J toggles it
var _perf_label: Label  # F3 toggles it
var _perf_time := 0.0  # seconds in the current window
var _perf_frames := 0
var _perf_max := 0.0  # slowest frame of the window, seconds
# Sums over the window, milliseconds: physics, the renderer's CPU side (frame
# setup and the viewport's draw calls) and its GPU time. The rest of a frame is
# mostly the nodes' _process (scripts, animators, flicker, AI).
var _perf_sums := {"physics_ms": 0.0, "render_cpu_ms": 0.0, "gpu_ms": 0.0}
var _perf_rid := RID()
var _lines: Array[Line] = []
var _path_line: MeshInstance3D  # G draws a path here
var _navmesh_fill: StandardMaterial3D
var _navmesh_lines: StandardMaterial3D


## One note on screen and how long it stays.
class Line:
	var text: String
	var left: float

	func _init(line_text: String, seconds: float) -> void:
		text = line_text
		left = seconds


func _init() -> void:
	_notes = _make_label()
	add_child(_notes)
	_journal = _make_label()
	_journal.visible = false
	add_child(_journal)
	_perf_label = _make_label()
	_perf_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_perf_label.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	_perf_label.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	_perf_label.position = Vector2.ZERO
	_perf_label.offset_left = -16.0
	_perf_label.offset_right = -16.0
	_perf_label.offset_top = 16.0
	add_child(_perf_label)


func setup(session: GameSession, camera: Camera3D, player: SkydotPlayer) -> void:
	_session = session
	_host = session.host
	_world = session.world
	_ai = session.ai
	_camera = camera
	_player = player


func _make_label() -> Label:
	var label := Label.new()
	label.position = Vector2(16, 16)
	label.add_theme_color_override("font_outline_color", Color.BLACK)
	label.add_theme_constant_override("outline_size", 4)
	return label


## Show a line at the top left for a few seconds, and print it.
func note(text: String) -> void:
	print(text)
	_lines.append(Line.new(text, NOTE_SECONDS))
	if _lines.size() > NOTE_LINES:
		_lines.pop_front()
	_show_lines()


## Every frame: add it to the performance window; once a second the line is
## rewritten from the window.
func sample_perf(delta: float) -> void:
	if not _perf_rid.is_valid():
		_perf_rid = get_viewport().get_viewport_rid()
		RenderingServer.viewport_set_measure_render_time(_perf_rid, true)
	_perf_time += delta
	_perf_frames += 1
	_perf_max = maxf(_perf_max, delta)
	_perf_sums["physics_ms"] += Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0
	_perf_sums["render_cpu_ms"] += RenderingServer.get_frame_setup_time_cpu() \
			+ RenderingServer.viewport_get_measured_render_time_cpu(_perf_rid)
	_perf_sums["gpu_ms"] += RenderingServer.viewport_get_measured_render_time_gpu(_perf_rid)
	if _perf_time >= 1.0:
		refresh_perf()


## Close the window now (needs at least one frame) and show its numbers.
func refresh_perf() -> void:
	if _perf_frames == 0:
		return
	perf = {
		"fps": snappedf(_perf_frames / _perf_time, 0.1),
		"frame_ms_mean": snappedf(_perf_time / _perf_frames * 1000.0, 0.01),
		"frame_ms_max": snappedf(_perf_max * 1000.0, 0.01),
		"draw_calls": int(RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME)),
		"primitives": int(RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME)),
		"objects": int(RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME)),
	}
	for key in _perf_sums:
		perf[key] = snappedf(_perf_sums[key] / _perf_frames, 0.01)
		_perf_sums[key] = 0.0
	_perf_time = 0.0
	_perf_frames = 0
	_perf_max = 0.0
	_perf_label.text = perf_text()


## The performance numbers in one line.
func perf_text() -> String:
	if perf.is_empty():
		return ""
	return "%.0f fps  %.1f ms (max %.1f)  render %.1f  gpu %.1f  physics %.1f ms  %d draws  %d prims  %d objects" % [
		perf["fps"], perf["frame_ms_mean"], perf["frame_ms_max"], perf.get("render_cpu_ms", 0.0),
		perf.get("gpu_ms", 0.0), perf.get("physics_ms", 0.0), perf["draw_calls"],
		perf["primitives"], perf["objects"]]


## F3: show or hide the performance line.
func toggle_perf() -> void:
	show_perf = not show_perf
	_perf_label.visible = show_perf


## Let the notes grow old.
func age(delta: float) -> void:
	if _lines.is_empty():
		return
	for line in _lines:
		line.left -= delta
	var before := _lines.size()
	var kept: Array[Line] = []
	for line in _lines:
		if line.left > 0.0:
			kept.append(line)
	_lines = kept
	if _lines.size() != before:
		_show_lines()


func _show_lines() -> void:
	var texts := PackedStringArray()
	for line in _lines:
		texts.append(line.text)
	_notes.text = "\n".join(texts)


## Toggle the journal on screen: running quests with displayed objectives, as
## the game's journal lists them. The console gets every running quest.
func toggle_journal() -> void:
	if _journal.visible:
		_journal.visible = false
		_notes.visible = true
		return
	var running := _session.get_running_quests()
	print("journal: %d running quests" % running.size())
	var lines: Array[String] = []
	for quest in running:
		var state := _session.get_quest_state(quest)
		var info := _world.get_quest(quest)
		var shown: Array = []
		for o in info.get("objectives", []):
			var s: Dictionary = state["objectives"].get(o["index"], {})
			if s.get("displayed", false) and not s.get("completed", false):
				shown.append(o["text"])
		print("  %s (0x%08X) stage %d%s" % [_session.quest_name(quest), quest, state["stage"],
			("  -> " + "; ".join(shown)) if not shown.is_empty() else ""])
		if not shown.is_empty():
			lines.append("%s  (stage %d)" % [_session.quest_name(quest), state["stage"]])
			for text in shown:
				lines.append("    " + text)
	lines.push_front("Journal: %d quests with objectives, %d running (J closes)" % [
		lines.filter(func(l: String) -> bool: return not l.begins_with(" ")).size(), running.size()])
	_journal.text = "\n".join(lines)
	_journal.visible = true
	_notes.visible = false


## The camera in the game's terms: position in game units (as
## `player.getpos`) and angles in degrees (as `player.getangle z` and `x`).
## `yaw` and `pitch` are the view's, in radians.
func position_text(yaw: float, pitch: float) -> String:
	var p := SkydotWorld.godot_to_skyrim(_camera.global_position)
	return "at %.0f,%.0f,%.0f  look %.0f,%.0f" % [p.x, p.y, p.z,
		fposmod(-rad_to_deg(yaw), 360.0), -rad_to_deg(pitch)]


## Tell what the actor nearest the camera is doing (its AI package).
func inspect_actor() -> void:
	if _ai == null:
		note("no AI (--ai off)")
		return
	var best: SkydotActor = null
	var best_d := 8.0
	for actor in _host.get_tree().root.find_children("*", "SkydotActor", true, false):
		var d: float = actor.global_position.distance_to(_camera.global_position)
		if d < best_d:
			best_d = d
			best = actor
	if best == null:
		note("no actor within 8 m")
		return
	var ref: int = best.get_meta("skydot_ref", 0)
	var state := _ai.get_actor_state(ref)
	var text := "%s: %s (%s), %s, step %d of %s" % [best.name, state.get("package_editor_id", "-"),
		state.get("template", ""), state.get("procedure", "-"), int(state.get("step", 0)) + 1,
		",".join(state.get("steps", []))]
	note(text)
	print(text, " ", state)


## N: show or hide the navmeshes of everything built.
func toggle_navmesh() -> void:
	show_navmesh = not show_navmesh
	navmesh_overlay(_host)
	note("navmesh shown" if show_navmesh else "navmesh hidden")


## Show or hide the navmeshes under `root` as a translucent overlay, built
## once per region.
func navmesh_overlay(root: Node) -> void:
	for region in root.find_children("*", "NavigationRegion3D", true, false):
		var overlay: Node3D = region.get_node_or_null("Overlay")
		if overlay != null:
			overlay.visible = show_navmesh
		elif show_navmesh:
			region.add_child(_navmesh_mesh(region.navigation_mesh))


func _navmesh_mesh(nav: NavigationMesh) -> MeshInstance3D:
	if _navmesh_fill == null:
		_navmesh_fill = StandardMaterial3D.new()
		_navmesh_fill.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		_navmesh_fill.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		_navmesh_fill.cull_mode = BaseMaterial3D.CULL_DISABLED
		_navmesh_fill.albedo_color = Color(0.1, 0.9, 0.3, 0.3)
		_navmesh_lines = _navmesh_fill.duplicate()
		_navmesh_lines.albedo_color = Color(0.2, 1.0, 0.4, 0.9)
	var lift := Vector3(0, 0.03, 0)  # above the ground it lies on
	var vertices := nav.get_vertices()
	var faces := PackedVector3Array()
	var edges := PackedVector3Array()
	for i in nav.get_polygon_count():
		var polygon := nav.get_polygon(i)
		for k in polygon.size():
			faces.append(vertices[polygon[k]] + lift)
			edges.append(vertices[polygon[k]] + lift)
			edges.append(vertices[polygon[(k + 1) % polygon.size()]] + lift)
	var mesh := ArrayMesh.new()
	for part in [[faces, Mesh.PRIMITIVE_TRIANGLES, _navmesh_fill], [edges, Mesh.PRIMITIVE_LINES, _navmesh_lines]]:
		var arrays := []
		arrays.resize(Mesh.ARRAY_MAX)
		arrays[Mesh.ARRAY_VERTEX] = part[0]
		mesh.add_surface_from_arrays(part[1], arrays)
		mesh.surface_set_material(mesh.get_surface_count() - 1, part[2])
	var instance := MeshInstance3D.new()
	instance.name = "Overlay"
	instance.mesh = mesh
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return instance


## G: a path from the feet to the navmesh point closest to the view ray.
func path_to_view() -> void:
	var world_3d := _host.get_world_3d()
	var map := world_3d.navigation_map
	var eye := _camera.global_position
	var end := eye - _camera.global_transform.basis.z * 300.0
	# Aim with physics where there is collision; the navigation map's own
	# segment test returns the origin when the segment misses.
	var hit := world_3d.direct_space_state.intersect_ray(
		PhysicsRayQueryParameters3D.create(eye, end, 0xFFFFFFFF, [_player.get_rid()]))
	var target := NavigationServer3D.map_get_closest_point(map, hit["position"]) if not hit.is_empty() \
		else NavigationServer3D.map_get_closest_point_to_segment(map, eye, end, false)
	var path := NavigationServer3D.map_get_path(map, _player.global_position, target, true)
	if _path_line == null:
		_path_line = MeshInstance3D.new()
		_path_line.name = "Path"
		_path_line.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		var material := StandardMaterial3D.new()
		material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		material.albedo_color = Color(1.0, 0.8, 0.1)
		material.no_depth_test = true
		_path_line.material_override = material
		_host.add_child(_path_line)
	var line := ImmediateMesh.new()
	if path.size() >= 2:
		line.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
		for point in path:
			line.surface_add_vertex(point + Vector3(0, 0.1, 0))
		line.surface_end()
	_path_line.mesh = line
	if path.size() < 2:
		note("no path")
		return
	var length := 0.0
	for k in range(1, path.size()):
		length += path[k - 1].distance_to(path[k])
	var short := path[path.size() - 1].distance_to(target)
	note("path: %d points, %.1f m%s" % [path.size(), length,
		"" if short < 0.5 else ", ends %.1f m short" % short])


## The place changed: the drawn path no longer fits.
func clear_path() -> void:
	if _path_line != null:
		_path_line.mesh = null
