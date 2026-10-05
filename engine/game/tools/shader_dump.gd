# SPDX-License-Identifier: GPL-3.0-or-later
#
# The shader oracle: prints the final code of every shader the engine can
# produce (SkydotMaterials.shader_sources: material variants, terrain, water,
# LOD, weather, particles, image space), one line per shader, sorted by name:
# name, SHA-256 of the code, its length in characters. Two dumps differ only
# if some shader's code differs, so moving shader source around proves itself
# by diffing before and after. Needs no pack.
#
#   godot4.7 --headless --path game --script res://tools/shader_dump.gd -- \
#       [--out FILE] [--texts DIR]
#
# --texts also writes each shader's code to DIR/<name>.txt, to see what a
# differing hash changed.
extends SceneTree


func _initialize() -> void:
    var argv := OS.get_cmdline_user_args()
    var args := {}
    for i in range(0, argv.size() - 1):
        args[argv[i]] = argv[i + 1]
    var sources := SkydotMaterials.shader_sources()
    var names: Array = sources.keys()
    names.sort()
    var lines := PackedStringArray()
    for shader_name: String in names:
        var code: String = sources[shader_name]
        lines.append("%s %s %d" % [shader_name, code.sha256_text(), code.length()])
        if args.has("--texts"):
            DirAccess.make_dir_recursive_absolute(args["--texts"])
            var file := FileAccess.open("%s/%s.txt" % [args["--texts"], shader_name], FileAccess.WRITE)
            file.store_string(code)
    var text := "\n".join(lines) + "\n"
    if args.has("--out"):
        FileAccess.open(args["--out"], FileAccess.WRITE).store_string(text)
    else:
        print(text)
    quit(0)
