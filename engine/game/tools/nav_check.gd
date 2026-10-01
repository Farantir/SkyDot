# SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks navmesh connectivity on a real pack: builds a block of exterior
# cells, then paths across every portal link whose two navmeshes are both
# built and reports how many Godot joined.
#
#   godot4.7 --headless --path game --script res://tools/nav_check.gd -- \
#       --pack <pack> [--world Tamriel] [--at 4,-12] [--radius 2]
extends SceneTree

func _initialize() -> void:
    var args := {"--world": "Tamriel", "--at": "4,-12", "--radius": "2"}
    var argv := OS.get_cmdline_user_args()
    for i in range(0, argv.size() - 1):
        if args.has(argv[i]) or argv[i] == "--pack":
            args[argv[i]] = argv[i + 1]
    if not args.has("--pack"):
        printerr("nav_check: --pack is required")
        quit(2)
        return
    _run(args)

func _run(args: Dictionary) -> void:
    var pack := SkydotPack.new()
    if pack.open(args["--pack"]) != OK:
        printerr("nav_check: ", pack.get_error())
        quit(1)
        return
    var world := pack.open_world()
    world.collision = false
    world.skyrim_materials = false
    world.effects = false
    var world_id := world.find_world(args["--world"])
    var at: PackedStringArray = args["--at"].split(",")
    var cx := int(at[0])
    var cy := int(at[1])
    var radius := int(args["--radius"])

    var built := {}
    var started := Time.get_ticks_usec()
    for y in range(cy - radius, cy + radius + 1):
        for x in range(cx - radius, cx + radius + 1):
            var cell_id := world.get_exterior_cell(world_id, x, y)
            if cell_id == 0:
                continue
            for nav in world.get_navmeshes(cell_id):
                built[nav["id"]] = nav
            var navmesh := _navmesh_only(world, cell_id)
            if navmesh != null:
                root.add_child(navmesh)
    print("nav_check: %d navmeshes in %d cells, built in %.1f ms" %
          [built.size(), (2 * radius + 1) * (2 * radius + 1), (Time.get_ticks_usec() - started) / 1000.0])
    var map := root.world_3d.navigation_map
    var waited := Time.get_ticks_usec()
    var iteration := NavigationServer3D.map_get_iteration_id(map)
    for i in 600:
        await physics_frame
        if NavigationServer3D.map_get_iteration_id(map) > iteration + 1:
            break
    print("nav_check: map synchronized after %.1f ms" % ((Time.get_ticks_usec() - waited) / 1000.0))

    var by_type := {}
    var worst := []
    for nav in built.values():
        for link in nav["links"]:
            if not built.has(link["navmesh"]) or link["to"] == null:
                continue
            var key: String = "type %d" % link["type"]
            if not by_type.has(key):
                by_type[key] = [0, 0]
            by_type[key][0] += 1
            var from: Vector3 = link["from"]
            var to: Vector3 = link["to"]
            var p := NavigationServer3D.map_get_path(map, from, to, true)
            var ok := p.size() >= 2 and p[p.size() - 1].distance_to(to) < 0.5
            if ok:
                var length := 0.0
                for k in range(1, p.size()):
                    length += p[k - 1].distance_to(p[k])
                ok = length < from.distance_to(to) * 3.0 + 2.0
            if ok:
                by_type[key][1] += 1
            elif worst.size() < 10:
                worst.append("%x -> %x at %s" % [nav["id"], link["navmesh"], SkydotWorld.godot_to_skyrim(from)])
    for key in by_type:
        print("nav_check: %s links %d, joined %d" % [key, by_type[key][0], by_type[key][1]])
    for line in worst:
        print("nav_check: not joined: ", line)
    quit(0)

## Only the navmesh regions of a cell, without models.
func _navmesh_only(world: SkydotWorld, cell_id: int) -> Node3D:
    var cell := world.build_cell(cell_id)
    var navmesh := cell.find_child("Navmesh", false, false)
    if navmesh != null:
        cell.remove_child(navmesh)
    cell.free()
    return navmesh
