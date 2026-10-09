# SPDX-License-Identifier: GPL-3.0-or-later
#
# The shader files (game/shaders/): every shader the engine can produce has
# code (a stub or a whole shader file), every file it #includes exists, the
# files' header comments are there, the terrain stubs name their layer count,
# and Godot accepts every one of them: given the code, its preprocessor and the
# dummy renderer's shader compiler log no SHADER ERROR. (The GPU compile of
# the same shaders is game/tools/shader_compile.gd, which needs a window.)
# Needs no pack.
extends SceneTree

## Collects the errors Godot logs for shaders.
class ShaderErrors extends Logger:
    var messages: PackedStringArray = []

    func _log_error(_function: String, _file: String, _line: int, code: String, rationale: String,
            _editor_notify: bool, error_type: int, _script_backtraces: Array[ScriptBacktrace]) -> void:
        if error_type == ERROR_TYPE_SHADER:
            messages.append((rationale if not rationale.is_empty() else code).strip_edges())

var failures := 0
var errors := ShaderErrors.new()

func _initialize() -> void:
    OS.add_logger(errors)
    _run()
    OS.remove_logger(errors)
    print("smoke_shaders: failures=", failures)
    quit(failures)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

## `code` without its comments, which the preprocessor drops too (the header
## comments quote the includes).
func uncommented(code: String) -> String:
    var block := RegEx.create_from_string("(?s)/\\*.*?\\*/")
    var line := RegEx.create_from_string("(?<!:)//[^\n]*")
    return line.sub(block.sub(code, "", true), "", true)

## `code` with every `#include "res://shaders/FILE"` replaced by that file's
## text, as far as it exists.
func expanded(code: String) -> String:
    code = uncommented(code)
    var include := RegEx.create_from_string("#include \"res://shaders/([A-Za-z0-9_.]+)\"")
    var found := include.search(code)
    while found != null:
        var path := "res://shaders/" + found.get_string(1)
        expect(FileAccess.file_exists(path), "the include " + path + " exists")
        var text := FileAccess.get_file_as_string(path)
        code = code.substr(0, found.get_start()) + expanded(text) + code.substr(found.get_end())
        found = include.search(code)
    return code

func _run() -> void:
    var sources := SkydotMaterials.shader_sources()
    for name in ["lighting_single_none", "effect_double_add_particles_lit", "refraction_double",
            "particles_process", "terrain_1", "terrain_7", "water", "lod_terrain", "lod_tree", "sky",
            "clouds", "sprite_add", "precipitation", "image_space_reduce", "image_space_grade",
            "image_space_volumetric"]:
        expect(sources.has(name), "the shader " + name + " is listed")
    var placeholder := RegEx.create_from_string("%[A-Z_]+%")
    var comment := RegEx.create_from_string("(?s)^\\s*/\\*.*?\\*/")
    for name: String in sources:
        var code: String = sources[name]
        var start := code.strip_edges(true, false)
        # A whole shader file starts with its header comment, a stub and a
        # compute shader without.
        start = comment.sub(start, "").strip_edges(true, false)
        expect(start.begins_with("shader_type ") or start.begins_with("#version "),
                name + " starts with its shader_type or #version, not empty: " + code.left(40))
        expect(placeholder.search(code) == null, name + " has a placeholder left")
        expanded(code)

    # Fog: every shader that has it includes game_fog.gdshaderinc and ends fragment() with it
    # (the effect shader writes its own).
    for name in ["lighting_double_blend", "effect_single_none_unlit", "water", "lod_object", "terrain_3",
            "lod_tree", "lod_terrain", "lod_water"]:
        var text := expanded(sources[name])
        expect(text.contains("skydot_game_fog(VERTEX)"), name + " has the game's fog")
    for name in ["lighting_single_test", "terrain_1", "lod_water"]:
        expect(expanded(sources[name]).contains("ambient_light_disabled"), name + " is lit by the game's ambient")
    expect(not expanded(sources["refraction_single"]).contains("skydot_game_fog"), "refraction has no fog")
    for layers in range(1, 8):
        var code: String = sources["terrain_%d" % layers]
        expect(code.contains("#define SKYDOT_LAYERS %d\n" % layers), "terrain_%d is a stub for %d layers" % [layers, layers])

    # Every shader file starts with a header comment.
    var count := 0
    for file in DirAccess.get_files_at("res://shaders"):
        if file.get_extension() not in ["gdshader", "gdshaderinc", "comp"]:
            continue
        count += 1
        var text := FileAccess.get_file_as_string("res://shaders/" + file)
        expect(text.begins_with("/*") and text.contains("*/\n"), file + " starts with a header comment")
    expect(count >= 20, "the shader files are there: %d" % count)

    # Godot accepts them all. The globals the fog and sun shaders read are
    # registered by shader_sources above. A shader that is wrong makes Godot
    # log a SHADER ERROR (and the test fails on seeing it), so first show that
    # the check sees one.
    var broken := Shader.new()
    broken.code = "shader_type spatial;\n#include \"res://shaders/not_a_file.gdshaderinc\"\n"
    broken.get_shader_uniform_list() # the rendering server compiles a shader when asked about it
    expect(not errors.messages.is_empty(), "a shader with a missing include is reported")
    errors.messages.clear()
    var compiled := 0
    for name: String in sources:
        var code: String = sources[name]
        if code.contains("#version "):
            continue
        var shader := Shader.new()
        shader.code = code
        shader.get_shader_uniform_list()
        compiled += 1
        expect(errors.messages.is_empty(), name + " compiles: " + ", ".join(errors.messages))
        errors.messages.clear()
    expect(compiled >= 70, "the shaders were given to Godot: %d" % compiled)
