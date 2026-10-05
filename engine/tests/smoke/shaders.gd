# SPDX-License-Identifier: GPL-3.0-or-later
#
# The shader files (game/shaders/): every shader the engine can produce has
# code, which means every file the C++ asks for was found (a missing one
# reports an error and gives an empty shader), the files' header comments are
# cut off, and no %PLACEHOLDER% line is left unfilled. Needs no pack.
extends SceneTree

var failures := 0

func _initialize() -> void:
    _run()
    print("smoke_shaders: failures=", failures)
    quit(failures)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func _run() -> void:
    var sources := SkydotMaterials.shader_sources()
    for name in ["lighting_single_none", "effect_double_add_particles_lit", "refraction_double",
            "particles_process", "terrain_1", "terrain_7", "water", "lod_terrain", "lod_tree", "sky",
            "clouds", "sprite_add", "precipitation", "image_space_reduce", "image_space_grade"]:
        expect(sources.has(name), "the shader " + name + " is listed")
    var placeholder := RegEx.create_from_string("%[A-Z_]+%")
    for name: String in sources:
        var code: String = sources[name]
        var start := code.strip_edges(true, false)
        expect(start.begins_with("shader_type ") or start.begins_with("#version "),
                name + " starts with its shader_type or #version, not empty or a header: " + code.left(40))
        expect(placeholder.search(code) == null, name + " has a placeholder left")
    for name in ["lighting_double_blend", "effect_single_none_unlit", "water", "lod_object", "terrain_3"]:
        expect((sources[name] as String).contains("skydot_game_fog(VERTEX)"), name + " has the game's fog")
    for layers in range(1, 8):
        var code: String = sources["terrain_%d" % layers]
        expect(code.count("uniform sampler2D albedo_") == layers, "terrain_%d has %d layers" % [layers, layers])

    # Every shader file starts with a header comment the loader cuts.
    var count := 0
    for file in DirAccess.get_files_at("res://shaders"):
        if file.get_extension() not in ["gdshader", "gdshaderinc", "comp"]:
            continue
        count += 1
        var text := FileAccess.get_file_as_string("res://shaders/" + file)
        expect(text.begins_with("/*") and text.contains("*/\n"), file + " starts with a header comment")
    expect(count >= 20, "the shader files are there: %d" % count)
