# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds one interior cell and prints how its actors were planned and built:
# NPC, race, sex, skeleton, idle and the models each wears.
#
#   godot4.7 --headless --path game --script res://tools/cell_actors.gd -- \
#       --pack <pack> --cell RiverwoodSleepingGiantInn
extends SceneTree

func _initialize() -> void:
    var argv := OS.get_cmdline_user_args()
    var args := {}
    for i in range(0, argv.size() - 1):
        args[argv[i]] = argv[i + 1]
    var pack := SkydotPack.new()
    if pack.open(args.get("--pack", "")) != OK:
        printerr("cell_actors: ", pack.get_error())
        quit(1)
        return
    var world := pack.open_world()
    var cell: int = world.find_cell(args.get("--cell", "RiverwoodSleepingGiantInn"))
    if cell == 0:
        printerr("cell_actors: no such cell")
        quit(1)
        return
    for ref in world.get_cell_actors(cell):
        var plan: Dictionary = world.get_actor_plan(ref)
        var info: Dictionary = world.get_actor(ref)
        print("%08x %s: race %08x %s scale %.2f, %d parts, idle %s%s" % [ref, "%08x" % info.get("base", 0), plan.get("race", 0),
            "female" if plan.get("female", false) else "male", plan.get("scale", 0.0), plan.get("parts", []).size(),
            String(plan.get("idle", "")).get_file(), (" MISSING " + plan["missing"]) if plan.get("missing", "") != "" else ""])
        print("    at ", info.get("position", Vector3()))
        for part in plan.get("parts", []):
            print("    ", part)
    var root := world.build_cell(cell)
    var stats: Dictionary = root.get_meta("skydot_stats")
    print("built: ", stats["actors"], " actors, ", stats["actor_parts"], " parts moved onto skeletons, failures ", stats["actor_failures"], ", missing ", stats["missing"])
    root.free()
    quit(0)
