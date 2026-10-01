# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the physics bodies of every mesh in a pack, one copy each, and counts
# them by shape type. Godot prints an error for any shape it cannot build.
#
#   godot4.7 --headless --path game --script res://tools/collision_check.gd -- \
#       --pack <pack> [--filter meshes/architecture/]
extends SceneTree

func _initialize() -> void:
    var argv := OS.get_cmdline_user_args()
    var args := {"--filter": ""}
    for i in range(0, argv.size() - 1):
        args[argv[i]] = argv[i + 1]
    if not args.has("--pack"):
        printerr("collision_check: --pack is required")
        quit(2)
        return
    var pack := SkydotPack.new()
    if pack.open(args["--pack"]) != OK:
        printerr("collision_check: ", pack.get_error())
        quit(1)
        return
    var index := FileAccess.open(args["--pack"].path_join("vpath.idx"), FileAccess.READ)
    var seen := {}
    var models := 0
    var with_bodies := 0
    var bodies := 0
    var shapes := {}
    var started := Time.get_ticks_msec()
    while not index.eof_reached():
        var line := index.get_line()
        if line.begins_with("#") or line == "":
            continue
        var fields := line.split("\t")
        if fields.size() < 3 or fields[2] != "mesh" or seen.has(fields[1]):
            continue
        if args["--filter"] != "" and not fields[0].contains(args["--filter"]):
            continue
        seen[fields[1]] = true
        var model := pack.load_scene(fields[0])
        if model == null:
            continue
        models += 1
        var copy := model.instantiate()
        var count := model.attach_collision(copy)
        if count > 0:
            with_bodies += 1
            bodies += count
            for shape in copy.find_children("*", "CollisionShape3D", true, false):
                var type: String = shape.shape.get_class() if shape.shape != null else "none"
                shapes[type] = shapes.get(type, 0) + 1
        copy.free()
    print("collision_check: %d models, %d with bodies, %d bodies in %.1f s" %
          [models, with_bodies, bodies, (Time.get_ticks_msec() - started) / 1000.0])
    for type in shapes:
        print("collision_check: ", type, " ", shapes[type])
    quit(0)
