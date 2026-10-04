# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints the idle, walk and run clips every behaviour project placed actors
# use would get, with speeds in metres per second, and how many actors use
# each project.
#
#   godot4.7 --headless --path game --script res://tools/locomotion_check.gd -- --pack <pack>
extends SceneTree

var _unit_scale := SkydotWorld.unit_scale()

func _initialize() -> void:
    var argv := OS.get_cmdline_user_args()
    var args := {}
    for i in range(0, argv.size() - 1):
        args[argv[i]] = argv[i + 1]
    var pack := SkydotPack.new()
    if pack.open(args.get("--pack", "")) != OK:
        printerr("locomotion_check: ", pack.get_error())
        quit(1)
        return
    var world := pack.open_world()
    var uses := {}
    for entry in world.list_cells("", false):
        var cell: int = entry["id"] if entry is Dictionary else entry
        for ref in world.get_cell_actors(cell):
            var plan: Dictionary = world.get_actor_plan(ref)
            var behaviour: String = plan.get("behaviour", "")
            if behaviour != "":
                uses[behaviour] = uses.get(behaviour, 0) + 1
    var names := uses.keys()
    names.sort()
    var moving := 0
    var standing := 0
    for behaviour in names:
        var l: Dictionary = world.get_locomotion(behaviour)
        var line := "%5d  %s" % [uses[behaviour], behaviour]
        if l["missing"] != "":
            line += "  -- " + l["missing"]
        for gait in ["idle", "walk", "run"]:
            var g: Dictionary = l[gait]
            if g["file"] == "":
                line += "  %s: -" % gait
            elif gait == "idle":
                line += "  %s: %s" % [gait, g["name"]]
            else:
                line += "  %s: %s %.2f m/s" % [gait, g["name"], g["speed"] * _unit_scale]
        print(line)
        if l["walk"]["file"] != "":
            moving += uses[behaviour]
        else:
            standing += uses[behaviour]
    print("actors that can walk: %d, that cannot: %d" % [moving, standing])
    quit(0)
