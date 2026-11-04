# SPDX-License-Identifier: GPL-3.0-or-later
#
# State dump for skycompare (--dump FILE): the references, lights and effects
# within a radius of the camera, in the same JSON schema as SkyrimRemote's
# @dump (SKYCOMPARE.md), with the game's own form IDs. Refs come from the
# pack's cell records (the loaded cells), "loaded3d" and the effects from the
# scene. Sorted by id and rounded, so two dumps of one view are identical.
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


static func build(host: Node, world: SkydotWorld, camera: Vector3, radius: float) -> Dictionary:
	var eye := SkydotWorld.godot_to_skyrim(camera)
	var cells := {}
	_cells_in(host, cells)
	var refs: Array = []
	var lights: Array = []
	var effects: Array = []
	var scale := SkydotWorld.unit_scale()
	for cell in cells:
		for entry in world.get_refs(cell):
			var id: int = entry["id"]
			var node := world.find_ref_node(id)
			var origin: Vector3 = (entry["transform"] as Transform3D).origin
			if node is Node3D:
				origin = node.global_position
			var pos := SkydotWorld.godot_to_skyrim(origin)
			if pos.distance_to(eye) > radius:
				continue
			var base: int = entry["base"]
			var info: Dictionary = world.get_base(base)
			var model := model_path(str(info.get("model", "")))
			var edid := str(info.get("editor_id", ""))
			var ident := {"model": model, "type": str(info.get("type", "")), "name": str(info.get("name", ""))}
			if edid != "":
				ident["edid"] = edid
			var ref_entry := {"id": _id(id), "base": _id(base),
				"pos": [snappedf(pos.x, 0.1), snappedf(pos.y, 0.1), snappedf(pos.z, 0.1)],
				"enabled": not entry["disabled"], "loaded3d": node != null}
			ref_entry.merge(ident)
			refs.append(ref_entry)
			if info.get("light") is Dictionary:
				var light: Dictionary = info["light"]
				var color: Color = light["color"]
				var fade: float = light["fade"]
				var light_entry := {"id": _id(id), "base": _id(base),
					"color": [snappedf(color.r, 0.001), snappedf(color.g, 0.001), snappedf(color.b, 0.001)],
					"radius": roundf(light["radius"] / scale),
					"fade": snappedf(fade if fade > 0.0 else 1.0, 0.001),
					"shadow": (int(light["flags"]) & SHADOW_FLAGS) != 0}
				light_entry.merge(ident)
				lights.append(light_entry)
			if node != null and not model.contains("marker"):
				var kind := effect_kind(node)
				if kind != "":
					var effect_entry := {"id": _id(id), "kind": kind, "base": _id(base)}
					effect_entry.merge(ident)
					effects.append(effect_entry)
	for list in [refs, lights, effects]:
		list.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return a["id"] < b["id"])
	return {"engine": "skydot", "camera": [snappedf(eye.x, 0.1), snappedf(eye.y, 0.1), snappedf(eye.z, 0.1)],
		"radius": radius, "refs": refs, "lights": lights, "effects": effects}


static func write(path: String, dump: Dictionary) -> bool:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return false
	file.store_string(JSON.stringify(dump, " ", false) + "\n")
	file.close()
	return true
