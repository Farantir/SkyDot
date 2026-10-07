# SPDX-License-Identifier: GPL-3.0-or-later
#
# The extension loads, registers its classes and refuses bad packs. Needs no
# pack; runs anywhere.
extends SceneTree

const k_index_header := "# bethconv vpath index v6\n"
const k_index_columns := "# virtual path\tcontent hash\tkind\twinning source\n"

var failures := 0

func _init() -> void:
    _check_registered()
    _check_path_arithmetic()
    _check_coordinates()
    _check_refusals()

# Billboards need the scene tree, which is only live once frames run.
func _process(_delta: float) -> bool:
    _check_billboards()
    _check_effects()
    _check_flicker()
    print("smoke_extension: failures=", failures)
    quit(failures)
    return true

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func _check_registered() -> void:
    expect(ClassDB.class_exists("SkydotPack"), "SkydotPack is registered")
    expect(ClassDB.can_instantiate("SkydotPack"), "SkydotPack can be instantiated")
    var pack := SkydotPack.new()
    expect(pack.PACK_FORMAT_VERSION == 6, "the engine reads pack format v6")
    expect(ClassDB.class_exists("SkydotWorld"), "SkydotWorld is registered")
    expect(ClassDB.class_exists("SkydotMaterials"), "SkydotMaterials is registered")
    expect(ClassDB.class_exists("SkydotBillboard"), "SkydotBillboard is registered")
    for name in ["SkydotAnimator", "SkydotParticles", "SkydotFlicker", "SkydotEmittance"]:
        expect(ClassDB.class_exists(name), name + " is registered")
    expect(not pack.is_open(), "a new pack is closed")
    expect(not pack.has("meshes/anything.nif") and pack.get_bytes("meshes/anything.nif").is_empty(),
           "a closed pack has nothing")
    expect(pack.load_scene("meshes/anything.nif") == null, "and loads nothing")

func _check_path_arithmetic() -> void:
    expect(SkydotPack.normalize_vpath("Meshes\\Clutter\\Apple01.NIF") == "meshes/clutter/apple01.nif",
           "normalize: backslashes and case")
    expect(SkydotPack.normalize_vpath("/textures/a b.dds") == "textures/a b.dds",
           "normalize: leading slash dropped, space kept")
    expect(SkydotPack.model_vpath("Clutter\\Apple01.nif") == "meshes/clutter/apple01.nif",
           "a MODL field is relative to meshes/")
    expect(SkydotPack.model_vpath("meshes\\Clutter\\Apple01.nif") == "meshes/clutter/apple01.nif",
           "a MODL field that already says meshes/ is left alone")
    expect(SkydotPack.model_vpath("") == "", "an empty MODL is empty")
    # The converter's rules (formats/include/skydot_formats/vpath.hpp), byte for byte.
    expect(SkydotPack.model_vpath("Meshes\\Ä\\Pot.nif") == "meshes/Ä/pot.nif",
           "letters above ASCII keep their case, ASCII ones do not")
    expect(SkydotPack.normalize_vpath("\\.\\Meshes//.//Clutter\\\\Apple01.NIF\\") == "meshes/clutter/apple01.nif",
           "normalize: duplicate separators, ./ segments, a leading and a trailing separator")
    expect(SkydotPack.normalize_vpath("meshes/../a/./b/.") == "meshes/../a/b",
           "normalize: .. is kept, a trailing . goes")
    expect(SkydotPack.normalize_vpath("meshes/.hidden/..x/a.b") == "meshes/.hidden/..x/a.b",
           "normalize: dots inside a name stay")
    expect(SkydotPack.normalize_vpath("İSTANBUL\\Iı.nif") == "İstanbul/iı.nif",
           "normalize: Turkish dotless i and dotted capital I are not Unicode-lowered")
    expect(SkydotPack.normalize_vpath("") == "" and SkydotPack.normalize_vpath("/\\./") == "",
           "normalize: nothing in, nothing out")

## Billboard nodes, marked the way the importer keeps glTF node extras.
func _check_billboards() -> void:
    var scene := Node3D.new()
    var camera := Camera3D.new()
    scene.add_child(camera)
    var nodes := {}
    for mode in [0, 1, 3]:
        var node := Node3D.new()
        node.set_meta("extras", {"bethconv": {"billboard": mode}})
        # Authored like the game's: local +Y is the parent's up.
        node.rotation = Vector3(PI / 2, 0, 0)
        node.position = Vector3(mode, 0, 0)
        scene.add_child(node)
        nodes[mode] = node
    scene.add_child(Node3D.new())
    expect(SkydotBillboard.attach(scene) == 3, "one billboard per marked node")
    expect(SkydotBillboard.attach(null) == 0, "attach(null) is harmless")

    get_root().add_child(scene)
    camera.look_at_from_position(Vector3(4, 3, 6), Vector3.ZERO)
    camera.make_current()
    for node in nodes.values():
        var billboard: SkydotBillboard = node.get_node("SkydotBillboard")
        billboard.face_camera()

    var eye := camera.global_position
    var face: Basis = nodes[0].global_basis
    expect(face.z.is_equal_approx(camera.global_basis.z), "mode 0 takes the camera's orientation")
    var center: Node3D = nodes[3]
    expect(center.global_basis.z.normalized().is_equal_approx((eye - center.global_position).normalized()),
           "mode 3 points +Z at the camera")
    var about_up: Node3D = nodes[1]
    expect(about_up.global_basis.y.is_equal_approx(Vector3(0, 0, 1)),
           "mode 1 keeps its authored up axis: %s" % about_up.global_basis.y)
    var flat := eye - about_up.global_position
    flat.z = 0
    expect(about_up.global_basis.z.normalized().is_equal_approx(flat.normalized()),
           "and turns +Z towards the camera about it")
    scene.free()

## Every refusal case, triggered with files this script writes (no game data,
## no converter output).
func _check_coordinates() -> void:
    const S := 0.0142875
    expect(is_equal_approx(SkydotWorld.unit_scale(), S), "scripts read the unit scale from the extension")
    expect(SkydotWorld.CELL_UNITS == 4096, "and the cell side")
    var p := SkydotWorld.skyrim_position(Vector3(100, 200, 300))
    expect(p.is_equal_approx(Vector3(100, 300, -200) * S), "Z-up game units to Y-up metres: %s" % p)
    # +90 degrees about Skyrim Z turns clockwise seen from above: local X
    # (east) ends up pointing south, Godot +Z.
    var t := SkydotWorld.skyrim_transform(Vector3.ZERO, Vector3(0, 0, PI / 2), 2.0)
    expect(t.basis.x.normalized().is_equal_approx(Vector3(0, 0, 1)), "rotation about Z: %s" % t.basis.x)
    expect(is_equal_approx(t.basis.get_scale().x, 2.0), "scale is applied")

func _check_refusals() -> void:
    var scratch := OS.get_environment("SKYDOT_SCRATCH")
    if scratch == "":
        scratch = OS.get_user_data_dir().path_join("smoke")
    var root := scratch.path_join("refusals")
    DirAccess.make_dir_recursive_absolute(root)

    var pack := SkydotPack.new()
    expect(pack.open(root.path_join("does-not-exist")) == ERR_FILE_NOT_FOUND, "missing directory")
    expect(pack.get_error().contains("no directory"), "missing directory says so")

    var empty := root.path_join("empty")
    DirAccess.make_dir_recursive_absolute(empty)
    expect(pack.open(empty) == ERR_FILE_NOT_FOUND, "a directory with no manifest is not a pack")
    expect(pack.get_error().contains("manifest.json"), "and the message names the file")

    # A v4 manifest (a pack that still needs a bake): refused with both
    # numbers in the sentence.
    var v4 := root.path_join("v4")
    _write_pack(v4, 4, k_index_header + k_index_columns)
    expect(pack.open(v4) == ERR_UNAVAILABLE, "a v4 pack is refused")
    expect(pack.get_error().contains("version 4") and pack.get_error().contains("v6"),
           "the refusal names both versions: " + pack.get_error())
    expect(not pack.is_open(), "a refused pack is closed")

    # A v6 manifest whose index header is from another version.
    var idx1 := root.path_join("idx1")
    _write_pack(idx1, 6, "# bethconv vpath index v1\n")
    expect(pack.open(idx1) == ERR_FILE_UNRECOGNIZED, "an index header this engine does not know")

    # A v6 manifest whose index has more lines than the manifest counted.
    var extra := root.path_join("extra")
    _write_pack(extra, 6, k_index_header + k_index_columns
        + "meshes/a.nif\t" + "0".repeat(64) + "\tmesh\tsrc\n")
    expect(pack.open(extra) == ERR_FILE_CORRUPT, "an index the manifest did not count")
    expect(pack.get_error().contains("1 entries but manifest.json says 0"),
           "and the two counts are in the message: " + pack.get_error())

    # An empty but valid pack: manifest and index both count zero.
    var ok := root.path_join("ok")
    _write_pack(ok, 6, k_index_header + k_index_columns)
    expect(pack.open(ok) == OK, "an empty pack opens: " + pack.get_error())
    expect(pack.is_open() and pack.get_index_count() == 0 and not pack.has_world(),
           "and reports itself empty")
    expect(pack.get_error() == "", "a successful open clears the error")

    # Packs from earlier converters name a records.fb in a `records` key. Nothing
    # reads it any more: the key and the file are ignored, even a missing or
    # damaged file.
    var norec := root.path_join("norec")
    _write_pack(norec, 6, k_index_header + k_index_columns, true)
    expect(pack.open(norec) == OK, "a records key whose file is missing is ignored: " + pack.get_error())
    expect(pack.is_open() and pack.get_error() == "", "and the pack is open")

    var badrec := root.path_join("badrec")
    _write_pack(badrec, 6, k_index_header + k_index_columns, true)
    var f := FileAccess.open(badrec.path_join("records.fb"), FileAccess.WRITE)
    f.store_string("NOTASNAP".rpad(64, " "))
    f.close()
    expect(pack.open(badrec) == OK, "a records.fb without its magic is ignored: " + pack.get_error())

    var rec2 := root.path_join("rec2")
    _write_pack(rec2, 6, k_index_header + k_index_columns, true)
    var header := PackedByteArray()
    header.resize(64)
    for i in 8:
        header[i] = "BETHSNAP".unicode_at(i)
    header.encode_u32(8, 2)
    f = FileAccess.open(rec2.path_join("records.fb"), FileAccess.WRITE)
    f.store_buffer(header)
    f.close()
    expect(pack.open(rec2) == OK, "so is one of a version this engine never read: " + pack.get_error())
    expect(not pack.has_world(), "and a records key does not make a world")

    # A world key whose file is missing.
    var noworld := root.path_join("noworld")
    _write_pack(noworld, 6, k_index_header + k_index_columns, false, true)
    expect(pack.open(noworld) == ERR_FILE_NOT_FOUND, "a missing world.fb is refused")

    # A damaged world.fb is refused by SkydotWorld.
    var junk := root.path_join("junk.fb")
    f = FileAccess.open(junk, FileAccess.WRITE)
    f.store_string("not a flatbuffer at all".rpad(64, "x"))
    f.close()
    var world := SkydotWorld.new()
    expect(world.open(junk) == ERR_FILE_CORRUPT, "a damaged world.fb is refused")
    expect(not world.is_open(), "and stays closed")
    var empty_fb := root.path_join("empty.fb")
    FileAccess.open(empty_fb, FileAccess.WRITE).close()
    expect(world.open(empty_fb) == ERR_FILE_CORRUPT, "an empty world.fb is refused")
    expect(world.get_error().contains("empty.fb"), "and the error names it")

    # The blob layout: its index must be there and well formed.
    var noidx := root.path_join("noidx")
    _write_pack(noidx, 6, k_index_header + k_index_columns, false, false, "blob")
    expect(pack.open(noidx) == ERR_FILE_NOT_FOUND, "a blob pack without assets.idx is refused")
    var badidx := root.path_join("badidx")
    _write_pack(badidx, 6, k_index_header + k_index_columns, false, false, "blob")
    f = FileAccess.open(badidx.path_join("assets.idx"), FileAccess.WRITE)
    f.store_string("BCAI")
    f.close()
    FileAccess.open(badidx.path_join("assets-0001.blob"), FileAccess.WRITE).close()
    expect(pack.open(badidx) == ERR_FILE_CORRUPT, "a truncated assets.idx is refused")
    var blob := root.path_join("blob")
    _write_pack(blob, 6, k_index_header + k_index_columns, false, false, "blob")
    var index := PackedByteArray()
    index.resize(24)
    for i in 4:
        index[i] = "BCAI".unicode_at(i)
    index.encode_u32(4, 1)
    index.encode_u32(8, 1)
    f = FileAccess.open(blob.path_join("assets.idx"), FileAccess.WRITE)
    f.store_buffer(index)
    f.close()
    FileAccess.open(blob.path_join("assets-0001.blob"), FileAccess.WRITE).close()
    expect(pack.open(blob) == OK, "an empty blob pack opens: " + pack.get_error())
    expect(pack.get_store_layout() == "blob", "and says it is one")
    var odd := root.path_join("odd")
    _write_pack(odd, 6, k_index_header + k_index_columns, false, false, "zip")
    expect(pack.open(odd) == ERR_FILE_UNRECOGNIZED, "an unknown store layout is refused")

func _write_pack(dir: String, version: int, index_text: String, with_records := false,
        with_world := false, layout := "loose") -> void:
    DirAccess.make_dir_recursive_absolute(dir)
    DirAccess.make_dir_recursive_absolute(dir.path_join("assets"))
    var manifest := {
        "pack_format_version": version,
        "converter": "smoke test",
        "assets": {"distinct": 0, "index_entries": 0},
        "store": {"layout": layout, "index": "assets.idx", "blob": "assets-0001.blob"},
    }
    if with_records:
        manifest["records"] = {"file": "records.fb", "forms": 0}
    if with_world:
        manifest["world"] = {"file": "world.fb", "cells": 0}
    var f := FileAccess.open(dir.path_join("manifest.json"), FileAccess.WRITE)
    f.store_string(JSON.stringify(manifest, "  ") + "\n")
    f.close()
    f = FileAccess.open(dir.path_join("vpath.idx"), FileAccess.WRITE)
    f.store_string(index_text)
    f.close()

## A model with the converter's effect extras: a clock-driven UV scroll and
## flame visibility, a named door sequence, a hidden node and one particle
## system.
func _check_effects() -> void:
    var root := Node3D.new()
    var pivot := Node3D.new()
    root.add_child(pivot)
    var door := Node3D.new()
    door.set_meta("extras", {"bethconv": {"id": 1}})
    pivot.add_child(door)
    var flame := MeshInstance3D.new()
    flame.mesh = QuadMesh.new()
    var shader := Shader.new()
    shader.code = "shader_type spatial;\nuniform vec2 uv_offset;\nuniform vec4 falloff_params;\n"
    var material := ShaderMaterial.new()
    material.shader = shader
    flame.set_surface_override_material(0, material)
    flame.set_meta("extras", {"bethconv": {"id": 2, "hidden": true}})
    pivot.add_child(flame)
    var smoke := Node3D.new()
    smoke.set_meta("extras", {"bethconv": {"id": 3}})
    pivot.add_child(smoke)
    pivot.set_meta("extras", {"bethconv": {
        "source": "meshes/test/effects.nif",
        "animations": [
            {"name": "", "autoplay": true, "cycle": "loop", "frequency": 1.0, "phase": 0.0,
             "start": 0.0, "stop": 2.0, "channels": [
                {"node": 2, "property": "effect.u_offset", "interp": "linear", "components": 1,
                 "times": [0.0, 2.0], "values": [0.0, 1.0]},
                {"node": 2, "property": "visible", "interp": "step", "components": 1,
                 "times": [0.0, 1.0], "values": [1.0, 0.0]}]},
            {"name": "Open", "autoplay": false, "cycle": "clamp", "frequency": 1.0, "phase": 0.0,
             "start": 0.0, "stop": 1.0, "text_keys": [[1.0, "end"]], "channels": [
                {"node": 1, "property": "rotation_z", "interp": "linear", "components": 1,
                 "times": [0.0, 1.0], "values": [0.0, -1.5]}]}],
        "particles": [
            {"node": 3, "world_space": true, "max_particles": 50,
             "material": {"shader": "BSEffectShaderProperty", "alpha_flags": 4333},
             "emitters": [{"kind": "box", "size": [10, 10, 10], "speed": 60.0, "life_span": 2.0,
                           "life_span_variation": 0.4, "radius": 10.0, "birth_rate": 15.0,
                           "color": [1, 1, 1, 1]}],
             "gravity": [], "simple_color": {"fade_in": 0.1, "fade_out": 0.3,
                 "color1_end": 0.0, "color2_start": 0.1, "color2_end": 0.5, "color3_start": 1.0,
                 "colors": [[1, 1, 1, 0], [1, 1, 1, 1], [1, 1, 1, 0]]}}]}})

    var materials := SkydotMaterials.new()
    expect(SkydotAnimator.attach(root, materials) == 3, "two clips and one particle system attached")
    expect(not flame.visible, "a node the NIF hides starts hidden")
    get_root().add_child(root)

    var animator: SkydotAnimator = root.get_node_or_null("SkydotAnimator")
    expect(animator != null, "an animator sits under the model root")
    if animator == null:
        return
    expect(animator.get_clip_names() == PackedStringArray(["Open"]), "only sequences have names")
    animator.evaluate(0.5, 0.0)
    expect(flame.visible, "the clock shows the flame at 0.5 s")
    var own: ShaderMaterial = flame.get_surface_override_material(0)
    expect(is_equal_approx(own.get_shader_parameter("uv_offset").x, 0.25), "UV offset follows the clock")
    animator.evaluate(1.5, 0.0)
    expect(not flame.visible, "and hides it at 1.5 s")
    animator.evaluate(2.5, 0.0)
    expect(is_equal_approx(own.get_shader_parameter("uv_offset").x, 0.25), "the clock loops")

    var keys: Array = []
    animator.text_key.connect(func(clip, key): keys.append([clip, key]))
    expect(animator.play("Open"), "a named sequence plays")
    expect(not animator.play("Missing"), "a missing one does not")
    animator.play("Open")
    animator.evaluate(0.0, 0.5)
    expect(is_equal_approx(door.rotation.z, -0.75), "the door is half open after 0.5 s")
    animator.evaluate(0.0, 3.0)
    expect(is_equal_approx(door.rotation.z, -1.5), "and a clamped sequence holds its last pose")
    expect(keys == [["Open", "end"]], "its text key fires once")

    var particles: SkydotParticles = smoke.get_node_or_null("SkydotParticles")
    expect(particles != null, "a particle system sits under its node")
    if particles != null:
        expect(particles.get_emitter_count() == 1, "with one emitter")
        var gpu: GPUParticles3D = particles.get_node_or_null("Emitter")
        expect(gpu != null and gpu.amount == 33, "sized to rate x longest life (15 x 2.2)")
        expect(gpu != null and gpu.emitting, "and emitting")
        particles.set_active(false)
        expect(gpu != null and not gpu.emitting, "switched off by the emitter's active channel")
    root.queue_free()

func _check_flicker() -> void:
    var flicker := SkydotFlicker.new()
    flicker.configure(0x0008, 5.0, 0.3, 0.1)
    var lo := 10.0
    var hi := -10.0
    for i in 200:
        var f := flicker.factor_at(i * 0.037)
        lo = min(lo, f)
        hi = max(hi, f)
    expect(lo >= 0.7 - 1e-6 and hi <= 1.0 + 1e-6, "flicker dims within its amplitude, never brighter")
    expect(hi - lo > 0.2, "and actually varies")
    var far := 0.0
    for i in 200:
        far = max(far, flicker.offset_at(i * 0.037).length())
    expect(far <= 0.1 / 64.0 + 1e-6, "movement stays within a 64th of its amplitude")
    expect(far > 0.0, "and actually moves")
    var pulse := SkydotFlicker.new()
    pulse.configure(0x0080, 1.0, 0.5, 0.0)
    expect(is_equal_approx(pulse.factor_at(0.75) - pulse.factor_at(0.25), 0.5), "a pulse is a sine")
    expect(pulse.offset_at(0.3) == Vector3.ZERO, "and does not move")
    # A Whiterun street fire: 1/period 0.05 (20 s), amplitude 1 below fade 3.
    var fire := SkydotFlicker.new()
    fire.configure(0x0009, 0.05, 1.0, 0.0, 3.0)
    var fire_lo := 10.0
    var fire_step := 0.0
    for i in 600:
        fire_lo = min(fire_lo, fire.factor_at(i * 0.1))
        fire_step = max(fire_step, absf(fire.factor_at(i * 0.1 + 1.0 / 60.0) - fire.factor_at(i * 0.1)))
    expect(fire_lo >= 2.0 / 3.0 - 1e-6, "a street fire dims to two thirds at most")
    expect(fire_step < 0.01, "and drifts slowly rather than jumping each frame")
    flicker.free()
    pulse.free()
    # A light that follows a region's weather keeps its colour where no weather runs.
    var emittance := SkydotEmittance.new()
    emittance.configure(Color(0.9, 0.7, 0.4), PackedInt64Array([0x8282A]), PackedInt64Array([100]))
    expect(emittance.tint() == Color(1, 1, 1, 1), "an emittance without a weather does not tint")
    emittance.free()
