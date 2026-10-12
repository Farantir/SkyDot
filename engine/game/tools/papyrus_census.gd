# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lists every native the pack's scripts call, bound in the engine or not, with
# the number of call instructions that reach it (read from the scripts' code,
# nothing runs). Natives marked "ignored" are meant to do nothing.
#
#   godot4.7 --headless --path game --script res://tools/papyrus_census.gd -- \
#       --pack <pack> [--top 40] [--all on]
#
# Prints the totals and the unbound natives with most call sites first;
# `--all on` lists the bound ones too, as a markdown table.
extends SceneTree

func _initialize() -> void:
    var argv := OS.get_cmdline_user_args()
    var args := {}
    for i in range(0, argv.size() - 1):
        args[argv[i]] = argv[i + 1]
    var pack := SkydotPack.new()
    if pack.open(args.get("--pack", "")) != OK:
        printerr("papyrus_census: ", pack.get_error())
        quit(1)
        return
    var world := pack.open_world()
    var papyrus := SkydotPapyrus.new()
    if papyrus.setup(pack, world) != OK:
        printerr("papyrus_census: no world")
        quit(1)
        return
    # vpath.idx: path, hash, kind, source, tab separated; scripts/<class>.pex.
    var classes := PackedStringArray()
    var index := FileAccess.open(String(args["--pack"]).path_join("vpath.idx"), FileAccess.READ)
    while index != null and not index.eof_reached():
        var path := index.get_line().get_slice("\t", 0)
        if path.begins_with("scripts/") and path.ends_with(".pex") and path.count("/") == 1:
            classes.append(path.trim_prefix("scripts/").trim_suffix(".pex"))
    var rows: Array = papyrus.census(classes)
    var bound := 0
    var ignored := 0
    var unbound: Array = []
    var sites := {"bound": 0, "ignored": 0, "unbound": 0}
    for row in rows:
        if row["ignored"]:
            ignored += 1
            sites["ignored"] += row["sites"]
        elif row["bound"]:
            bound += 1
            sites["bound"] += row["sites"]
        else:
            unbound.append(row)
            sites["unbound"] += row["sites"]
    unbound.sort_custom(func(a, b): return a["sites"] > b["sites"] or (a["sites"] == b["sites"] and a["class"] + a["function"] < b["class"] + b["function"]))
    print("scripts scanned: %d" % classes.size())
    print("natives called: %d (bound %d, ignored %d, unbound %d)" % [rows.size(), bound, ignored, unbound.size()])
    print("call sites: %d (bound %d, ignored %d, unbound %d)" % [
        sites["bound"] + sites["ignored"] + sites["unbound"], sites["bound"], sites["ignored"], sites["unbound"]])
    var top := int(args.get("--top", "40"))
    print("\nunbound, most call sites first:\n\n| Native | Call sites |\n| --- | ---: |")
    for row in unbound.slice(0, top):
        print("| `%s.%s` | %d |" % [row["class"], row["function"], row["sites"]])
    if args.get("--all", "off") == "on":
        print("\nall natives:\n\n| Native | Call sites | State |\n| --- | ---: | --- |")
        for row in rows:
            var state := "ignored" if row["ignored"] else ("bound" if row["bound"] else "unbound")
            print("| `%s.%s` | %d | %s |" % [row["class"], row["function"], row["sites"], state])
    quit(0)
