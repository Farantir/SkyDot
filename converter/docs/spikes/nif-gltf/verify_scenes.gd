# Loads every imported scene and reports meshes, materials, textures, skeletons
# and extras. Checks the imported result, since an import can drop textures
# without logging an error.
#
#   godot4.7 --headless --path proj --script verify_scenes.gd
extends SceneTree

func walk(n: Node, out: Dictionary) -> void:
	if n is MeshInstance3D:
		var m: Mesh = n.mesh
		out.meshes += 1
		out.surfaces += m.get_surface_count()
		for i in m.get_surface_count():
			var mat := m.surface_get_material(i)
			if mat is BaseMaterial3D:
				out.materials += 1
				if mat.albedo_texture != null:
					out.albedo += 1
					var fmt: int = mat.albedo_texture.get_format()
					out.formats[fmt] = out.formats.get(fmt, 0) + 1
				if mat.normal_texture != null:
					out.normal += 1
	if n is Skeleton3D:
		out.skeletons += 1
		out.bones += n.get_bone_count()
	# glTF extras arrive as node metadata under the key "extras".
	if n.has_meta("extras") and (n.get_meta("extras") as Dictionary).has("bethconv"):
		out.bethconv_extras += 1
	for c in n.get_children():
		walk(c, out)

func _init() -> void:
	var files: Array[String] = []
	var stack: Array[String] = ["res://meshes"]
	while not stack.is_empty():
		var d := stack.pop_back() as String
		for sub in DirAccess.get_directories_at(d):
			stack.push_back(d + "/" + sub)
		for f in DirAccess.get_files_at(d):
			if f.ends_with(".glb"):
				files.append(d + "/" + f)
	files.sort()

	var out := {"scenes": 0, "failed": 0, "meshes": 0, "surfaces": 0, "materials": 0,
		"albedo": 0, "normal": 0, "skeletons": 0, "bones": 0, "bethconv_extras": 0,
		"formats": {}}
	for f in files:
		var packed := load(f) as PackedScene
		if packed == null:
			out.failed += 1
			print("FAILED to load ", f)
			continue
		var root := packed.instantiate()
		out.scenes += 1
		walk(root, out)
		root.free()
	print(JSON.stringify(out, "  "))
	quit()
