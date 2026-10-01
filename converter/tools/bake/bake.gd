# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs in the bake project after `--import`. Saves each imported `.glb` as a
# `.scn` under its virtual path (so the engine can derive it from MODL, e.g.
# `testpack/cube_se.nif` -> `res://meshes/testpack/cube_se.scn`), then packs
# scenes, textures and scripts into a `.pck` with PCKPacker, which needs no
# export templates.
@tool
extends SceneTree

const k_scene_root := "res://meshes"

func _init() -> void:
    var saved := _save_scenes()
    if saved < 0:
        quit(1)
        return
    var packed := _write_pck()
    if packed < 0:
        quit(1)
        return
    print("bake: scenes=", saved, " packed=", packed)
    quit(0)

## Load every imported `.glb` and save it as a `.scn` under `k_scene_root`.
func _save_scenes() -> int:
    var saved := 0
    for path in _walk("res://meshes", ".glb"):
        var scene := ResourceLoader.load(path, "PackedScene")
        if scene == null:
            printerr("bake: cannot load ", path)
            return -1
        var target: String = path.get_basename() + ".scn"
        var rc := ResourceSaver.save(scene, target)
        if rc != OK:
            printerr("bake: cannot save ", target, ", rc=", rc)
            return -1
        saved += 1
    return saved

## Packs scenes, textures and scripts under their `res://` paths. DDS files are
## loaded directly by Godot, so they go in unchanged.
func _write_pck() -> int:
    var packer := PCKPacker.new()
    if packer.pck_start("res://bethconv.pck") != OK:
        printerr("bake: cannot start the pck")
        return -1

    var added := 0
    for group in [["res://meshes", ".scn"], ["res://textures", ".dds"],
                  ["res://scripts", ".pex"]]:
        for source in _walk(group[0], group[1]):
            if packer.add_file(source, source) != OK:
                printerr("bake: cannot add ", source)
                return -1
            added += 1

    if packer.flush(false) != OK:
        printerr("bake: cannot flush the pck")
        return -1
    return added

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
    # DirAccess order is filesystem-dependent; sort so the .pck lists files in
    # a stable order.
    found.sort()
    return found
