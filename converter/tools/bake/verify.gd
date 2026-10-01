# SPDX-License-Identifier: GPL-3.0-or-later
#
# Mounts the `.pck` in an empty project and counts scenes, meshes, surfaces and
# textures that load. The empty project ensures nothing is read from the bake's
# source files.
extends SceneTree

func _init() -> void:
    var pck: String = "res://bethconv.pck"
    if not ProjectSettings.load_resource_pack(pck):
        printerr("verify: cannot mount ", pck)
        quit(1)
        return

    var scenes := 0
    var meshes := 0
    var surfaces := 0
    var albedo := 0
    var normals := 0

    for path in _walk("res://meshes", ".scn"):
        var packed := ResourceLoader.load(path, "PackedScene")
        if packed == null:
            printerr("verify: cannot load ", path)
            quit(1)
            return
        var root: Node = packed.instantiate()
        if root == null:
            printerr("verify: cannot instantiate ", path)
            quit(1)
            return
        scenes += 1
        for node in _nodes(root):
            if node is MeshInstance3D:
                meshes += 1
                var mesh: Mesh = node.mesh
                if mesh == null:
                    continue
                for i in mesh.get_surface_count():
                    surfaces += 1
                    var material: Material = mesh.surface_get_material(i)
                    if material is BaseMaterial3D:
                        if material.albedo_texture != null:
                            albedo += 1
                        if material.normal_texture != null:
                            normals += 1
        root.free()

    print("verify: scenes=", scenes, " meshes=", meshes, " surfaces=", surfaces,
          " albedo=", albedo, " normals=", normals)
    if scenes == 0:
        printerr("verify: the pck contains no scenes")
        quit(1)
        return
    quit(0)

func _nodes(node: Node) -> Array:
    var all := [node]
    for child in node.get_children():
        all.append_array(_nodes(child))
    return all

func _walk(root: String, suffix: String) -> Array:
    var found := []
    var dir := DirAccess.open(root)
    if dir == null:
        return found
    dir.list_dir_begin()
    var name := dir.get_next()
    while name != "":
        if not name.begins_with("."):
            var full: String = root.path_join(name)
            if dir.current_is_dir():
                found.append_array(_walk(full, suffix))
            elif name.ends_with(suffix):
                found.append(full)
        name = dir.get_next()
    dir.list_dir_end()
    return found
