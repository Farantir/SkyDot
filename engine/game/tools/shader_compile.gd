# SPDX-License-Identifier: GPL-3.0-or-later
#
# The compile check: gives every shader the engine can produce (SkydotMaterials.
# shader_sources: material variants, terrain, water, LOD, weather, particles) to
# Godot, draws each once, and counts the errors Godot logs for them. Godot's
# preprocessor expands the `#include`s and `#ifdef`s of the shader files when a
# Shader gets its code, so this is what shows a typo in one of them, a variant
# whose defines select nothing that compiles, or an include that cannot be found.
#
#   godot4.7 --path game --script res://tools/shader_compile.gd -- [--frames 20] [--sheet FILE]
#
# Run it windowed (a GPU): then the shaders compile to GLSL and SPIR-V too, as
# in the game. With --headless the dummy renderer still parses and checks every
# shader (the same SHADER ERROR lines), but compiles none. Exits with the number
# of shaders that logged an error. The compute shaders of the image space are
# not Godot shaders and are skipped.
#
# --sheet FILE also saves the picture: every shader on a sphere (or, the
# particles, a few quads) in a grid, in names' order, lit, fogged and with the
# game's ambient set to fixed values. With `--fixed-fps 60` (a Godot option, before
# --script) two runs give the same pixels, so a refactor that is meant to leave
# the shaders as they are compares sheets from before and after.
extends SceneTree


## Collects what Godot logs as a shader error, or as a failed shader compile.
class ErrorLog extends Logger:
    var messages: PackedStringArray = []

    func _log_error(_function: String, _file: String, _line: int, code: String, rationale: String,
            _editor_notify: bool, error_type: int, _script_backtraces: Array[ScriptBacktrace]) -> void:
        if error_type == ERROR_TYPE_SHADER or rationale.contains("ompilation failed"):
            messages.append((rationale if not rationale.is_empty() else code).strip_edges())


var _log := ErrorLog.new()
var _frames := 20
var _sheet := ""
var _failed := {}


func _initialize() -> void:
    var argv := OS.get_cmdline_user_args()
    for i in range(0, argv.size() - 1):
        if argv[i] == "--frames":
            _frames = int(argv[i + 1])
        elif argv[i] == "--sheet":
            _sheet = argv[i + 1]
    OS.add_logger(_log)
    var sources := SkydotMaterials.shader_sources()
    var names: Array = sources.keys()
    names.sort()
    var count := 0
    var spacing := 2.5
    for shader_name: String in names:
        var code: String = sources[shader_name]
        if code.begins_with("#version"):
            continue
        var before := _log.messages.size()
        var shader := Shader.new()
        shader.code = code
        shader.get_shader_uniform_list() # the rendering server compiles a shader when asked about it
        _use(shader, count, spacing)
        if _log.messages.size() > before:
            _failed[shader_name] = _log.messages.slice(before)
        count += 1
    var camera := Camera3D.new()
    root.add_child(camera)
    var rows := ceili(count / 10.0)
    camera.position = Vector3(spacing * 4.5, -spacing * (rows - 1) * 0.5, 15.0)
    camera.far = 200.0
    if not _sheet.is_empty():
        _set_up_sheet()
    print("shader_compile: %d shaders given to Godot, drawing %d frames" % [count, _frames])


func _process(_delta: float) -> bool:
    _frames -= 1
    if _frames > 0:
        return false
    # Errors of draws that came after the shaders were created are not
    # attributed to one of them, only counted.
    var late := _log.messages.size()
    for shader_name: String in _failed:
        late -= _failed[shader_name].size()
        printerr("FAIL: ", shader_name, ": ", _failed[shader_name][0])
    if late > 0:
        printerr("FAIL: %d more shader errors while drawing" % late)
    if not _sheet.is_empty():
        root.get_texture().get_image().save_png(_sheet)
        print("shader_compile: sheet ", _sheet)
    print("shader_compile: %d shaders with errors, %d more errors" % [_failed.size(), late])
    quit(_failed.size() + (1 if late > 0 else 0))
    return true


## The light, background and global shader parameters the sheet is drawn with.
func _set_up_sheet() -> void:
    var environment := Environment.new()
    environment.background_mode = Environment.BG_COLOR
    environment.background_color = Color(0.2, 0.25, 0.3)
    var world := WorldEnvironment.new()
    world.environment = environment
    root.add_child(world)
    var sun := DirectionalLight3D.new()
    root.add_child(sun)
    sun.rotation = Vector3(-0.8, 0.6, 0.0)
    var server := RenderingServer
    server.global_shader_parameter_set("skydot_fog", Vector4(5.0, 25.0, 1.0, 0.8))
    server.global_shader_parameter_set("skydot_fog_near_color", Vector3(0.6, 0.5, 0.4))
    server.global_shader_parameter_set("skydot_fog_far_color", Vector3(0.4, 0.5, 0.7))
    server.global_shader_parameter_set("skydot_ambient_r", Vector4(0.2, -0.1, 0.1, 0.3))
    server.global_shader_parameter_set("skydot_ambient_g", Vector4(0.1, 0.2, -0.1, 0.3))
    server.global_shader_parameter_set("skydot_ambient_b", Vector4(-0.1, 0.1, 0.3, 0.3))
    server.global_shader_parameter_set("skydot_sun_direction", Vector3(0.3, 0.8, 0.5).normalized())
    server.global_shader_parameter_set("skydot_sun_color", Vector3(1.0, 0.9, 0.7))
    server.global_shader_parameter_set("skydot_sky_upper", Vector3(0.3, 0.5, 0.9))
    server.global_shader_parameter_set("skydot_sky_horizon", Vector3(0.7, 0.8, 0.9))


## Shows `shader` on the `index`th object of a grid, as the kind of resource
## its shader_type needs.
func _use(shader: Shader, index: int, spacing: float) -> void:
    var material := ShaderMaterial.new()
    material.shader = shader
    var position := Vector3((index % 10) * spacing, floori(index / 10.0) * -spacing, 0.0)
    match shader.get_mode():
        Shader.MODE_SPATIAL:
            var ball := MeshInstance3D.new()
            var sphere := SphereMesh.new()
            sphere.radial_segments = 24
            sphere.rings = 12
            ball.mesh = sphere
            ball.material_override = material
            root.add_child(ball)
            ball.position = position
        Shader.MODE_PARTICLES:
            var particles := GPUParticles3D.new()
            particles.process_material = material
            particles.draw_pass_1 = QuadMesh.new()
            particles.amount = 4
            particles.use_fixed_seed = true
            particles.seed = 1
            root.add_child(particles)
            particles.position = position
        Shader.MODE_SKY:
            if not _sheet.is_empty():
                return # it would replace the background
            var sky := Sky.new()
            sky.sky_material = material
            var environment := Environment.new()
            environment.background_mode = Environment.BG_SKY
            environment.sky = sky
            var world := WorldEnvironment.new()
            world.environment = environment
            root.add_child(world)
