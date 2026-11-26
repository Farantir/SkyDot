# SPDX-License-Identifier: GPL-3.0-or-later
#
# The skills screen's 3D sky as the game's StatsMenu builds it (SkyrimSE.exe 1.7.104, read from the code that
# places the stars, the camera's turn and its moves; values are the game settings' defaults in the exe, the
# Skyrim.esm GMSTs win where they exist):
#   - interface/intperkskydome.nif (nebula, stars, constellation images, its Idle clip; CameraIntro moves its
#     CameraPosition up from z -3000 as the menu opens). Its nodes Point00..Point17 stand on a ring of radius
#     99 around the centre, 20 degrees apart; tree N (skill actor value 6 + N) hangs under PointN.
#   - one interface/intperkstars01.nif per perk node (not the root), scaled fStatsStarScale, at
#       x = (width / 2 - (XNAM + HNAM)) * fStatsStarXIncrement, y = (YNAM + VNAM) * fStatsStarYIncrement,
#       z = fStatsStarZInitialOffset - (YNAM + VNAM) * fStatsStarZIncrement
#     in PointN's frame, `width` being the largest XNAM of the tree's perk nodes (ActorValueInfo +0x120).
#   - one interface/intperkline01.nif from each star to each star it leads to, scaled distance *
#     fStatsLineScale under the star (so it spans the distance: the mesh is 56.2 long).
#   - the camera stands at the centre and turns about z (a velocity eased in over fStatsRotationRampTime ms up
#     to 1 / fStatsRotationSpeedMax rad/s while a direction is held; let go it slows into the nearest skill, 20
#     degrees a skill, and stops under 0.01 rad/s), looking at PointN + fStatsSkillsLookAt. Into a tree it
#     moves to a star + fStatsStarCameraOffset looking at star + fStatsStarLookAt, along the movie's
#     CameraMovementInstance (8 of its 100 frames every 41 ms, linear).
# Stars play their clips Locked / Unlock / Unlocked, lines Locked / Unlocked (taken on both ends).
class_name StatsSky
extends SubViewportContainer

const SKYDOME := "meshes/interface/intperkskydome.nif"
const STAR := "meshes/interface/intperkstars01.nif"
const LINE := "meshes/interface/intperkline01.nif"
const TREES := 18
const SKILL_ANGLE := TAU / TREES
## The exe's defaults (Setting objects in .data) for those Skyrim.esm does not set.
const DEFAULTS := {
	"fStatsCameraFOV": 50.0, "fStatsStarScale": 0.15, "fStatsLineScale": 0.1175,
	"fStatsStarXIncrement": 5.0, "fStatsStarYIncrement": 15.0, "fStatsStarZIncrement": -9.0,
	"fStatsStarZInitialOffset": 5.0, "fStatsRotationSpeedMax": 0.5, "fStatsRotationRampTime": 2000.0,
	"fStatsSkillsLookAtX": 0.0, "fStatsSkillsLookAtY": 0.0, "fStatsSkillsLookAtZ": 5.0,
	"fStatsStarCameraOffsetX": 0.0, "fStatsStarCameraOffsetY": -8.0, "fStatsStarCameraOffsetZ": 0.0,
	"fStatsStarLookAtX": 0.0, "fStatsStarLookAtY": 0.0, "fStatsStarLookAtZ": 1.0,
	"fStatsCameraNearDistance": 1.0,
}
## The line mesh's length (its BeamEnd node).
const LINE_LENGTH := 56.2
## Below this the turn stops on the skill (the exe's 0.01), and the factor the turn speeds up by each frame
## while slower than the remaining distance allows (1.3).
const STOP_SPEED := 0.01
const CATCH_UP := 1.3
const HELD_RATE := 0.5
## turn_to (a click on another skill): the turn closes this share of what is left per second, at most the held
## turn's top speed (assumed: the game's mouse turn is not measured).
const GOAL_RATE := 6.0
## A star is under the mouse within this share of the view's height.
const STAR_PICK_RADIUS := 0.04
## CameraMovementInstance: 100 frames, the script steps 8 of them every 41 ms.
const MOVE_STEP := 0.08
const MOVE_TICK := 0.041
## The CameraIntro clip of the skydome: CameraPosition's z (game units) over its 0.63 s.
const INTRO_Z := [-3000.0, -2983.8, -2937.0, -2862.2, -2762.1, -2639.1, -2496.1, -2335.6, -2160.2, -1972.5, -1775.3,
	-1571.0, -1362.4, -1152.0, -942.5, -736.4, -536.6, -345.4, -165.6, 0.0]
const INTRO_STEP := 1.0 / 30.0

const DECODE_SHADER := """shader_type canvas_item;
render_mode blend_premul_alpha;
void fragment() {
	vec4 t = texture(TEXTURE, UV);
	vec3 c = t.rgb;
	COLOR = vec4(mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)), t.a);
}
"""

var _session: GameSession
var _viewport: SubViewport
var _camera: Camera3D
var _root := Node3D.new()
var _frame: Node3D  # the Points' parent in the skydome: NIF space (game units, z up), the model's conversion above it
var _points: Array[Node3D] = []
var _trees: Array[Node3D] = []
var _stars := {}  # tree -> {node index -> {node, pos (Vector3, Point space), status}}
var _lines := {}  # tree -> [{node, from, to}]
var _settings := {}
## The camera's turn about z (radians, 0 looks at Point00, positive towards Point01: the next tree) and speed (rad/s).
var yaw := 0.0
var velocity := 0.0
var _input_dir := 0.0
var _goal := NAN  # turn_to: the yaw the camera turns to, NAN when none
var _held := false
var _ramp_ms := 0.0
var _ramp_from := 0.0
var _ramp_to := 0.0
## The camera's move into or out of a tree: from and to {pos, look} in NIF space, progress 0..1.
var _move := {}
var _move_t := 1.0
var _move_clock := 0.0
var _intro_t := -1.0
var _zoom_tree := -1
var _zoom_node := -1
var _last_usec := 0
## The constellation images (cImageNN, tree N): {material, state (0 off, 1 lighting, 2 fading, as StatsMenu's per-tree
## state), since (ms)}. The menu sets their material alpha to 0 and lights the faced one to 1 over 500 ms.
var _images: Array[Dictionary] = []
const GLOW_MS := 500.0


func _init(session: GameSession) -> void:
	_session = session
	stretch = true
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)
	_viewport = SubViewport.new()
	_viewport.own_world_3d = true
	_viewport.msaa_3d = Viewport.MSAA_4X
	# The sky hides the world: where the nebula is thin the game shows black, not the world (the user's shot of an
	# exterior by day; the measured interior shots read the same, their world being dark there).
	_viewport.transparent_bg = false
	add_child(_viewport)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color.BLACK
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color.WHITE
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	_viewport.add_child(world_environment)
	_camera = Camera3D.new()
	_camera.current = true
	# The effect shader works in the game's gamma space (textures, blending; materials.hpp) and the world's camera
	# hands Godot linear colour in its image space pass; here the sky has no such pass, so Godot's output encoding
	# would brighten the gamma values again: the container draws the picture decoded back to what the game shows.
	var decode := ShaderMaterial.new()
	decode.shader = Shader.new()
	decode.shader.code = DECODE_SHADER
	material = decode
	_viewport.add_child(_camera)
	_frame = _root
	_viewport.add_child(_root)
	for key: String in DEFAULTS:
		_settings[key] = DEFAULTS[key]
		if session != null and session.world != null:
			_settings[key] = session.world.get_gmst_float(key, DEFAULTS[key])
	_camera.fov = float(_settings["fStatsCameraFOV"])
	_camera.keep_aspect = Camera3D.KEEP_HEIGHT  # the game's fStatsCameraFOV is the vertical angle (measured against its shot)
	_camera.near = float(_settings["fStatsCameraNearDistance"]) * SkydotWorld.unit_scale()
	_camera.far = 8000.0 * SkydotWorld.unit_scale()


func _s(key: String) -> float:
	return float(_settings[key])


## Build the sky (once) and every tree's stars and lines from `trees`: tree index -> SkillsModel.tree_nodes()
## entries ({index, grid (Vector2 XNAM+HNAM, YNAM+VNAM), links, status}) and its `width`.
func build(trees: Dictionary) -> void:
	if _points.is_empty():
		var dome := _session.world.build_model(SKYDOME) if _session.world != null else null
		if dome != null:
			_root.add_child(dome)
			for i in TREES:
				var point := dome.find_child("Point%02d" % i, true, false) as Node3D
				_points.append(point if point != null else dome)
			if _points[0].get_parent() is Node3D:
				_frame = _points[0].get_parent()
			_play(dome, "Idle")
			for i in TREES:
				_images.append(_image(dome.find_child("cImage%02d" % i, true, false)))
	for t: int in trees:
		_build_tree(t, trees[t])
	_order()


## OrderedRenderingNode (a BSOrderedNode) draws its children in their order, whatever their depth: the star layers,
## the nebulas, the constellation images, then the Points with the stars and lines. Godot sorts transparent
## geometry by depth, so each child's geometry gets a sorting offset by its place.
func _order() -> void:
	if _frame == _root:
		return
	var i := 0
	for child in _frame.get_children():
		i += 1
		var geometry: Array = child.find_children("*", "GeometryInstance3D", true, false)
		if child is GeometryInstance3D:
			geometry.append(child)
		for g: GeometryInstance3D in geometry:
			g.sorting_offset = i * 1000.0


func _build_tree(t: int, tree: Dictionary) -> void:
	if t < 0 or t >= _points.size():
		return
	if t < _trees.size() and _trees[t] != null:
		set_status(t, tree["nodes"])
		return
	while _trees.size() <= t:
		_trees.append(null)
	var holder := Node3D.new()
	_points[t].add_child(holder)
	_trees[t] = holder
	var stars := {}
	var width := float(tree.get("width", 0))
	for n: Dictionary in tree["nodes"]:
		var g: Vector2 = n["grid"]
		var pos := Vector3((width * 0.5 - g.x) * _s("fStatsStarXIncrement"), g.y * _s("fStatsStarYIncrement"),
			_s("fStatsStarZInitialOffset") - g.y * _s("fStatsStarZIncrement"))
		var star := _nif_model(STAR)
		if star == null:
			continue
		star.position = pos
		star.scale = Vector3.ONE * _s("fStatsStarScale")
		holder.add_child(star)
		stars[int(n["index"])] = {"node": star, "pos": pos, "status": "", "links": n["links"]}
	_stars[t] = stars
	var lines: Array = []
	for from: int in stars:
		for to: int in stars[from]["links"]:
			if not stars.has(to):
				continue
			var a: Vector3 = stars[from]["pos"]
			var b: Vector3 = stars[to]["pos"]
			var line := _nif_model(LINE)
			if line == null:
				continue
			var d := (b - a)
			# The mesh runs along its +y and lies in its xy plane; it faces the centre (the camera's side).
			var y := d.normalized()
			var x := y.cross(Vector3(0.0, -1.0, 0.0)).normalized()
			if x.length() < 0.5:
				x = Vector3.RIGHT
			var z := x.cross(y)
			var s := d.length() * _s("fStatsLineScale") * _s("fStatsStarScale")
			line.transform = Transform3D(Basis(x * s, y * s, z * s), a)
			holder.add_child(line)
			lines.append({"node": line, "from": from, "to": to, "status": ""})
	_lines[t] = lines
	set_status(t, tree["nodes"])


## Play the stars' and lines' clips for the statuses in `nodes` (SkillsModel nodes of tree `t`), as the game's shots
## show them: a taken perk Owned (gold), one whose requirements are met (only a perk point missing or none)
## Unlocked (blue-white), the rest Locked (purple); a line Unlocked from a taken star, else Locked.
func set_status(t: int, nodes: Array) -> void:
	var stars: Dictionary = _stars.get(t, {})
	var owned := {}
	for n: Dictionary in nodes:
		var i := int(n["index"])
		if not stars.has(i):
			continue
		var clip := "Locked"
		if n["status"] == "taken":
			clip = "Owned"
		elif bool(n.get("unlockable", false)):
			clip = "Unlocked"
		owned[i] = clip == "Owned"
		var entry: Dictionary = stars[i]
		if entry["status"] != clip:
			entry["status"] = clip
			_play(entry["node"], clip)
	for line: Dictionary in _lines.get(t, []):
		var clip := "Unlocked" if bool(owned.get(line["from"], false)) else "Locked"
		if line["status"] != clip:
			line["status"] = clip
			_play(line["node"], clip)


## A model to place in the skydome's NIF space: without the z-up to y-up conversion (and metres) the pack puts on
## every model's root, which the skydome already has above its Points.
func _nif_model(path: String) -> Node3D:
	var model := _session.world.build_model(path)
	if model != null:
		var conversion := model.find_child("bethconv_z_up_to_y_up", false, false) as Node3D
		if conversion != null:
			conversion.transform = Transform3D.IDENTITY
	return model


func _image(node: Node) -> Dictionary:
	var mesh := node as MeshInstance3D
	if mesh == null and node != null:
		var found := node.find_children("*", "MeshInstance3D", true, false)
		mesh = found[0] if not found.is_empty() else null
	if mesh == null or not (mesh.get_active_material(0) is ShaderMaterial):
		return {}
	var material := (mesh.get_active_material(0) as ShaderMaterial).duplicate() as ShaderMaterial
	mesh.set_surface_override_material(0, material)
	var entry := {"material": material, "state": 0, "since": 0}
	_set_glow(entry, 0.0)
	return entry


func _set_glow(entry: Dictionary, value: float) -> void:
	var colour: Vector4 = entry["material"].get_shader_parameter("emission_color")
	colour.w = value  # BSEffectShaderProperty::SetMaterialAlpha: the base colour's alpha itself
	entry["material"].set_shader_parameter("emission_color", colour)


## Light (true) or fade (false) tree `t`'s constellation image, from wherever it stands now.
func glow(t: int, on: bool) -> void:
	if t < 0 or t >= _images.size() or _images[t].is_empty():
		return
	var entry := _images[t]
	if int(entry["state"]) == (1 if on else 2):
		return
	entry["state"] = 1 if on else 2
	entry["since"] = Time.get_ticks_msec()


func _step_glow() -> void:
	var now := Time.get_ticks_msec()
	for entry in _images:
		if entry.is_empty() or int(entry["state"]) == 0:
			continue
		var x := clampf((now - int(entry["since"])) / GLOW_MS, 0.0, 1.0)
		_set_glow(entry, x if int(entry["state"]) == 1 else 1.0 - x)
		if x >= 1.0 and int(entry["state"]) == 2:
			entry["state"] = 0


func _play(model: Node, clip: String) -> void:
	for animator in model.find_children("*", "SkydotAnimator", true, false):
		animator.call("play", clip)


## The menu opens: the camera rises from below (CameraIntro) facing tree `t`.
func open(t: int) -> void:
	yaw = t * SKILL_ANGLE
	velocity = 0.0
	_held = false
	_goal = NAN
	_zoom_tree = -1
	_move = {}
	_move_t = 1.0
	_intro_t = 0.0
	for i in _images.size():
		if not _images[i].is_empty():
			_images[i]["state"] = 0
			_set_glow(_images[i], 0.0)
	glow(t, true)
	_place_camera()


## Hold a turn: -1 towards the previous skill, +1 the next, 0 lets go (the turn then slows into a skill).
func turn(direction: float) -> void:
	if direction == _input_dir:
		return
	_input_dir = direction
	_goal = NAN
	if direction != 0.0:
		if not is_turning():
			glow(facing(), false)
		_held = true
		_ramp_ms = 0.0
		_ramp_from = velocity
		_ramp_to = direction / _s("fStatsRotationSpeedMax")
	else:
		_held = false


## The tree the camera faces (0..17).
func facing() -> int:
	return posmod(int(round(yaw / SKILL_ANGLE)), TREES)


func is_turning() -> bool:
	return _held or velocity != 0.0 or not is_nan(_goal)


## Turn the shortest way to face tree `t` and stop there (a click on its constellation or its ring label).
func turn_to(t: int) -> void:
	if _zoom_tree >= 0:
		return
	var left := wrapf(t * SKILL_ANGLE - yaw, -PI, PI)
	if absf(left) < 0.0001:
		return
	if not is_turning():
		glow(facing(), false)
	_held = false
	_input_dir = 0.0
	_goal = yaw + left


## The middle of tree `t`'s constellation on the screen (what the camera looks at when it faces it), as fractions
## of this view; null when behind the camera.
func tree_screen(t: int) -> Variant:
	if t < 0 or t >= _points.size() or not _camera.is_inside_tree():
		return null
	var look := Vector3(_s("fStatsSkillsLookAtX"), _s("fStatsSkillsLookAtY"), _s("fStatsSkillsLookAtZ"))
	return _fraction(_frame.global_transform * (_frame.global_transform.affine_inverse() * _points[t].global_position + look))


## The tree whose constellation's middle is nearest across to `at` (fractions of this view); -1 if none is in front.
func tree_at(at: Vector2) -> int:
	var best := -1
	var best_d := INF
	for t in _points.size():
		var p: Variant = tree_screen(t)
		if p == null or (p as Vector2).x < -0.5 or (p as Vector2).x > 1.5:
			continue
		var d := absf((p as Vector2).x - at.x)
		if d < best_d:
			best = t
			best_d = d
	return best


## The star of tree `t` under `at` (fractions of this view), the nearest within STAR_PICK_RADIUS; -1 if none.
func star_at(t: int, at: Vector2) -> int:
	var size := Vector2(_viewport.size)
	var best := -1
	var best_d := STAR_PICK_RADIUS * size.y
	for node: int in _stars.get(t, {}):
		var p: Variant = star_screen(t, node)
		if p == null:
			continue
		var d := ((p as Vector2) - at) * size
		if d.length() <= best_d:
			best = node
			best_d = d.length()
	return best


func _fraction(at: Vector3) -> Variant:
	if _camera.is_position_behind(at):
		return null
	return _camera.unproject_position(at) / Vector2(_viewport.size)


## Move the camera to star `node` of tree `t` (-1: back out to the ring).
func zoom(t: int, node: int) -> void:
	var from := {"pos": _camera_pos(), "look": _camera_look()}
	_zoom_tree = t
	_zoom_node = node
	_move = {"from": from}
	_move_t = 0.0
	_move_clock = 0.0


func zoomed() -> bool:
	return _zoom_tree >= 0


## A star's place on the screen, as fractions of this view (x right, y down); null when behind the camera.
func star_screen(t: int, node: int) -> Variant:
	var entry: Dictionary = _stars.get(t, {}).get(node, {})
	if entry.is_empty() or not is_instance_valid(entry["node"]):
		return null
	return _fraction((entry["node"] as Node3D).global_position)


func _process(_delta: float) -> void:
	# Real time: the menu runs while the game is paused (and while shots hold the world's time still).
	var now := Time.get_ticks_usec()
	var delta := clampf((now - _last_usec) / 1000000.0, 0.0, 0.1) if _last_usec > 0 else 0.0
	_last_usec = now
	if not is_visible_in_tree():
		return
	_step_turn(delta)
	_step_glow()
	if _move_t < 1.0:
		_move_clock += delta
		_move_t = minf(floorf(_move_clock / MOVE_TICK) * MOVE_STEP, 1.0)
	if _intro_t >= 0.0:
		_intro_t += delta
		if _intro_t > INTRO_STEP * (INTRO_Z.size() - 1):
			_intro_t = -1.0
	_place_camera()


func _step_turn(delta: float) -> void:
	if _zoom_tree >= 0:
		return
	if not is_nan(_goal):
		var left := _goal - yaw
		velocity = signf(left) * minf(absf(left) * GOAL_RATE, 1.0 / _s("fStatsRotationSpeedMax"))
		yaw += velocity * delta
		if absf(left) < 0.002 or signf(_goal - yaw) != signf(left):
			yaw = round(_goal / SKILL_ANGLE) * SKILL_ANGLE
			velocity = 0.0
			_goal = NAN
			glow(facing(), true)
		return
	if _held:
		_ramp_ms += delta * 1000.0
		var x := clampf(_ramp_ms / _s("fStatsRotationRampTime"), 0.0, 1.0)
		velocity = _ramp_from + (_ramp_to - _ramp_from) * sin(x * PI * 0.5)
	elif velocity != 0.0:
		# Slow into the next skill in the direction of the turn: the speed the remaining part of the 20 degrees
		# allows (1 / fStatsRotationSpeedMax per whole skill), never faster than now; catch up by 1.3 a frame.
		var p := yaw / SKILL_ANGLE
		var left := (ceilf(p) - p) if velocity > 0.0 else (p - floorf(p))
		if left < 0.0001:
			left = 0.0
		var target := left / _s("fStatsRotationSpeedMax") * signf(velocity)
		var v := minf(velocity, target) if velocity > 0.0 else maxf(velocity, target)
		if absf(v) < absf(target):
			v *= CATCH_UP
		velocity = v
		if absf(velocity) < STOP_SPEED:
			velocity = 0.0
			yaw = round(yaw / SKILL_ANGLE) * SKILL_ANGLE
			glow(facing(), true)
	# While a key is held the game turns half as far as the velocity says (measured, SkyrimRemote held D: 1.0 s -> 2
	# skills, 2.0 s -> 4, the settle afterwards as the exe's formula): the turn is applied on every other frame of a
	# held key. Let go, the settle runs at full rate.
	yaw += velocity * delta * (HELD_RATE if _held else 1.0)


## Where the camera stands and what it looks at, in NIF space.
func _ring_pose() -> Dictionary:
	var dir := Vector3(sin(yaw), cos(yaw), 0.0)  # yaw 0: +y (Point00); positive: towards Point01 (+x, the right)
	var point := dir * 99.34 + Vector3(0.0, 0.0, -0.068)
	var look := point + Vector3(_s("fStatsSkillsLookAtX"), _s("fStatsSkillsLookAtY"), _s("fStatsSkillsLookAtZ"))
	var z := 0.0
	if _intro_t >= 0.0:
		var f := _intro_t / INTRO_STEP
		var i := clampi(int(f), 0, INTRO_Z.size() - 2)
		z = lerpf(INTRO_Z[i], INTRO_Z[i + 1], clampf(f - i, 0.0, 1.0))
	return {"pos": Vector3(0.0, 0.0, z), "look": look + Vector3(0.0, 0.0, z)}


func _star_pose(t: int, node: int) -> Dictionary:
	var entry: Dictionary = _stars.get(t, {}).get(node, {})
	if entry.is_empty() or t >= _points.size():
		return _ring_pose()
	var point := _points[t]
	var to_root := _frame.global_transform.affine_inverse() * point.global_transform
	var star: Vector3 = entry["pos"]
	var offset := Vector3(_s("fStatsStarCameraOffsetX"), _s("fStatsStarCameraOffsetY"), _s("fStatsStarCameraOffsetZ"))
	var look := Vector3(_s("fStatsStarLookAtX"), _s("fStatsStarLookAtY"), _s("fStatsStarLookAtZ"))
	return {"pos": to_root * (star + offset), "look": to_root * (star + look)}


func _camera_pos() -> Vector3:
	return _frame.global_transform.affine_inverse() * _camera.global_position


func _camera_look() -> Vector3:
	return _camera_pos() + _frame.global_transform.affine_inverse().basis * (-_camera.global_transform.basis.z) * 10.0


func _place_camera() -> void:
	if not _camera.is_inside_tree():
		return
	var target := _ring_pose() if _zoom_tree < 0 else _star_pose(_zoom_tree, _zoom_node)
	var pos: Vector3 = target["pos"]
	var look: Vector3 = target["look"]
	if not _move.is_empty() and _move_t < 1.0:
		var from: Dictionary = _move["from"]
		pos = (from["pos"] as Vector3).lerp(pos, _move_t)
		look = (from["look"] as Vector3).lerp(look, _move_t)
	var p := _frame.global_transform * pos
	var l := _frame.global_transform * look
	if p.distance_to(l) > 0.0001:
		_camera.look_at_from_position(p, l, Vector3.UP)
