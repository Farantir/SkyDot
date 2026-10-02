# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints what NPCs' AI packages make them do over a day: at each hour, the
# package chosen, its procedure steps and where it sends them (space and
# position). NPCs by editor id; scripts start the start-game quests first, so
# quest conditions see what a new game sees.
#
#   godot4.7 --headless --path game --script res://tools/package_check.gd -- \
#       --pack <pack> --npcs AlvorRiverwood,SigridRiverwood [--hours 0,6,8,12,18,22] \
#       [--day 0] [--list on] [--set-stage EDID:STAGE,...]
extends SceneTree

func _initialize() -> void:
    var argv := OS.get_cmdline_user_args()
    var args := {}
    for i in range(0, argv.size() - 1):
        args[argv[i]] = argv[i + 1]
    var pack := SkydotPack.new()
    if pack.open(args.get("--pack", "")) != OK:
        printerr("package_check: ", pack.get_error())
        quit(1)
        return
    var world := pack.open_world()
    var papyrus := SkydotPapyrus.new()
    papyrus.setup(pack, world)
    papyrus.start_game_enabled_quests()
    if args.has("--set-stage"):
        for item in String(args["--set-stage"]).split(","):
            var parts := item.split(":")
            papyrus.set_stage(world.find_quest(parts[0]), int(parts[1]))
    papyrus.update(1.0)
    var ai := SkydotAi.new()
    if ai.setup(world, papyrus) != OK:
        printerr("package_check: no world")
        quit(1)
        return
    var hours: Array = Array(String(args.get("--hours", "0,6,8,12,18,22")).split(",")).map(func(h): return float(h))
    var day := float(args.get("--day", "0"))
    var spaces := {}
    for npc_id in String(args.get("--npcs", "AlvorRiverwood")).split(","):
        var ref := _find_actor(world, npc_id)
        if ref == 0:
            print("%s: no placed actor" % npc_id)
            continue
        print("== %s 0x%08X" % [npc_id, ref])
        if args.get("--list", "off") != "off":
            ai.days = day + 12.0 / 24.0
            for p in ai.get_packages(ref):
                print("   0x%08X %-40s %-24s hour %3d dur %4d dow %2d %s" % [p["id"], p.get("editor_id", "?"),
                    p.get("template", ""), p.get("hour", -1), p.get("duration", 0), p.get("day_of_week", -1),
                    "" if p.get("conditions_pass", true) else "(conditions fail)"])
                for c in p.get("conditions", []):
                    print("        fn %d(0x%X, %d) run_on %d: %s, type 0x%02X vs %s" % [c["function"], c["param1"],
                        c["param2"], c["run_on"], c["value"], c["type"], c["compare_to"]])
        for h in hours:
            ai.days = day + h / 24.0
            var state: Dictionary = ai.get_actor_state(ref)
            var dest: Dictionary = ai.get_destination(ref)
            var where := "-"
            if not dest.is_empty():
                var space: int = dest["space"]
                if not spaces.has(space):
                    var cell := world.get_cell(space)
                    spaces[space] = cell.get("editor_id", "0x%08X" % space) if not cell.is_empty() else "0x%08X" % space
                var p: Vector3 = dest["position"]
                where = "%s (%d, %d, %d) r%d" % [spaces[space], p.x, p.y, p.z, dest["radius"]]
            print("  %05.2f %-40s %-22s %-36s %s" % [h, state.get("package_editor_id", ""), state.get("template", ""),
                ",".join(state.get("steps", [])), where])
    quit(0)

func _find_actor(world: SkydotWorld, editor_id: String) -> int:
    # The NPC_'s editor id: search placed actors' plans.
    if editor_id.begins_with("0x"):
        return editor_id.hex_to_int()
    var npc := _npc_by_editor_id(world, editor_id)
    return world.find_actor_of(npc) if npc != 0 else 0

func _npc_by_editor_id(world: SkydotWorld, editor_id: String) -> int:
    return world.find_npc(editor_id)
