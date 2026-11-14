# SPDX-License-Identifier: GPL-3.0-or-later
#
# State dump for skycompare (--dump FILE): the references, lights and effects
# within a radius of the camera, in the same JSON schema as SkyrimRemote's
# @dump (SKYCOMPARE.md), with the game's own form IDs. Refs come from the
# pack's cell records (the loaded cells) plus the actors (those of the loaded
# cells and every built one), "loaded3d" and the effects from the scene. Sorted by id and rounded, so two dumps of one view are identical.
class_name StateDump
extends RefCounted

const SHADOW_FLAGS := (1 << 10) | (1 << 11) | (1 << 12)  # spot, hemisphere, omnidirectional shadow


static func _id(value: int) -> String:
	return "0x%08X" % value


## The nif path as the dumps give it: lower case, "/" separators, without "meshes/".
static func model_path(path: String) -> String:
	var m := path.to_lower().replace("\\", "/")
	return m.trim_prefix("meshes/")


static func _cells_in(node: Node, out: Dictionary) -> void:
	if node is Node3D and node.has_meta("skydot_cell"):
		out[int(node.get_meta("skydot_cell"))] = true
		return
	for child in node.get_children():
		_cells_in(child, out)


## "particle", "shader" or "" for what a placed model draws as an effect.
static func effect_kind(node: Node) -> String:
	if not node.find_children("*", "GPUParticles3D", true, false).is_empty():
		return "particle"
	for mesh in node.find_children("*", "MeshInstance3D", true, false):
		for i in mesh.get_surface_override_material_count():
			var material: Material = mesh.get_active_material(i)
			if material is ShaderMaterial and material.shader != null \
					and material.shader.code.contains("effect.gdshaderinc"):
				return "shader"
	return ""


## The actor refs (ACHR) around: those the loaded cells place, and every built actor under `node` (persistent ones
## live in the world's persistent cell and are built wherever their AI place is).
static func _actors_in(node: Node, world: SkydotWorld, cells: Dictionary, out: Dictionary) -> void:
	for cell in cells:
		for ref in world.get_cell_actors(cell):
			out[int(ref)] = true
	var pending: Array[Node] = [node]
	while not pending.is_empty():
		for child in pending.pop_back().get_children():
			if child is SkydotActor and child.has_meta("skydot_ref"):
				out[int(child.get_meta("skydot_ref"))] = true
			elif child.get_child_count() > 0 and not child.has_meta("skydot_ref"):
				pending.append(child)


static func build(host: Node, world: SkydotWorld, camera: Vector3, radius: float, papyrus: SkydotPapyrus = null) -> Dictionary:
	var eye := SkydotWorld.godot_to_skyrim(camera)
	var cells := {}
	_cells_in(host, cells)
	var dump := {"engine": "skydot", "camera": [snappedf(eye.x, 0.1), snappedf(eye.y, 0.1), snappedf(eye.z, 0.1)],
		"radius": radius, "refs": [], "lights": [], "effects": []}
	var seen := {}
	for cell in cells:
		for entry in world.get_refs(cell):
			var id: int = entry["id"]
			seen[id] = true
			var node := world.find_ref_node(id)
			var origin: Vector3 = (entry["transform"] as Transform3D).origin
			_add(dump, world, id, entry["base"], node,
				SkydotWorld.godot_to_skyrim(node.global_position if node is Node3D else origin),
				not entry["disabled"], eye, {})
	var actors := {}
	_actors_in(host, world, cells, actors)
	for id: int in actors:
		if seen.has(id):
			continue
		var actor := world.get_actor(id)
		if actor.is_empty():
			continue
		var node := world.find_ref_node(id)
		var pos: Vector3 = actor["position"]
		if node is Node3D:
			pos = SkydotWorld.godot_to_skyrim(node.global_position)
		# A ragdoll has the body (dead, knocked down): the reference is where its root body is, as the game's.
		var lying: Variant = papyrus.get_ragdoll_position(id) if papyrus != null else null
		if lying is Vector3:
			pos = lying
		# A built actor is enabled whatever its editor flag says (a script or the AI enabled it).
		_add(dump, world, id, actor["base"], node, pos, node != null or not actor["disabled"], eye, actor)
	for list in [dump["refs"], dump["lights"], dump["effects"]]:
		list.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return a["id"] < b["id"])
	return dump


## One ref (pos in Skyrim units) into the refs, lights and effects of `dump` if it is within its radius of `eye`.
## `actor` is the get_actor record of an actor ref (its NPC base is no placeable base), else empty.
static func _add(dump: Dictionary, world: SkydotWorld, id: int, base: int, node: Node, pos: Vector3,
		enabled: bool, eye: Vector3, actor: Dictionary) -> void:
	var radius: float = dump["radius"]
	if pos.distance_to(eye) > radius:
		return
	var info: Dictionary = world.get_base(base)
	if not actor.is_empty() and info.is_empty():
		info = {"type": "NPC_", "name": actor.get("name", "")}
	var model := model_path(str(info.get("model", "")))
	var edid := str(info.get("editor_id", ""))
	var ident := {"model": model, "type": str(info.get("type", "")), "name": str(info.get("name", ""))}
	if edid != "":
		ident["edid"] = edid
	var ref_entry := {"id": _id(id), "base": _id(base),
		"pos": [snappedf(pos.x, 0.1), snappedf(pos.y, 0.1), snappedf(pos.z, 0.1)],
		"enabled": enabled, "loaded3d": node != null}
	ref_entry.merge(ident)
	dump["refs"].append(ref_entry)
	if info.get("light") is Dictionary:
		var light: Dictionary = info["light"]
		var color: Color = light["color"]
		var fade: float = light["fade"]
		var light_entry := {"id": _id(id), "base": _id(base),
			"color": [snappedf(color.r, 0.001), snappedf(color.g, 0.001), snappedf(color.b, 0.001)],
			"radius": roundf(light["radius"] / SkydotWorld.unit_scale()),
			"fade": snappedf(fade if fade > 0.0 else 1.0, 0.001),
			"shadow": (int(light["flags"]) & SHADOW_FLAGS) != 0}
		light_entry.merge(ident)
		dump["lights"].append(light_entry)
	if node != null and not model.contains("marker"):
		var kind := effect_kind(node)
		if kind != "":
			var effect_entry := {"id": _id(id), "kind": kind, "base": _id(base)}
			effect_entry.merge(ident)
			dump["effects"].append(effect_entry)


## The enabled, built DOOR ref nearest to the camera (0 if none), from a `build` result: the same pick skycompare
## makes for the game from the dump.
static func nearest_door(dump: Dictionary) -> int:
	var eye := Vector3(dump["camera"][0], dump["camera"][1], dump["camera"][2])
	var best := 0
	var best_distance := INF
	for ref: Dictionary in dump["refs"]:
		if ref.get("type") != "DOOR" or not ref["enabled"] or not ref["loaded3d"]:
			continue
		var d := Vector3(ref["pos"][0], ref["pos"][1], ref["pos"][2]).distance_squared_to(eye)
		if d < best_distance:
			best_distance = d
			best = String(ref["id"]).hex_to_int()
	return best


static func write(path: String, dump: Dictionary) -> bool:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return false
	file.store_string(JSON.stringify(dump, " ", false) + "\n")
	file.close()
	return true
