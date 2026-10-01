# SPDX-License-Identifier: GPL-3.0-or-later
#
# Loads the test pack's models and textures straight from its asset store, as
# the engine does: a virtual path must give a scene whose surfaces end up with
# Skyrim materials and their textures, and the test cells must build.
extends SceneTree

var failures := 0

func _init() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_assets: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_assets: SKIP (SKYDOT_TESTPACK unset; build bethconv-testpack and point it here)")
        quit(77)
        return
    _run(pack_dir)
    print("smoke_assets: failures=", failures)
    quit(failures)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func _run(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    if failures > 0:
        return

    var albedo_tex := pack.load_texture("textures/testpack/cube.dds")
    expect(albedo_tex is ImageTexture and albedo_tex.get_width() == 16,
           "a DDS loads as a 16x16 texture: %s" % albedo_tex)
    if albedo_tex is ImageTexture:
        var image: Image = albedo_tex.get_image()
        expect(image.is_compressed() and image.has_mipmaps(), "block-compressed, with its mips")
    var sky := pack.load_texture("textures/testpack/sky.dds")
    expect(sky is Cubemap and sky.get_layers() == 6, "a cube map loads as a Cubemap: %s" % sky)
    expect(pack.load_texture("textures/testpack/cube.dds") == albedo_tex, "textures are cached")

    var meshes := ["meshes/testpack/cube_se.nif", "meshes/testpack/cube_le.nif",
                   "meshes/testpack/cube_se_copy.nif", "meshes/testpack/cube with space.nif"]
    var scenes := 0
    var surfaces := 0
    var albedo := 0
    var normals := 0
    for vpath in meshes:
        expect(pack.get_kind(vpath) == "mesh", "the pack knows " + vpath)
        var scene := pack.load_scene(vpath)
        if scene == null:
            expect(false, "loads: " + vpath)
            continue
        scenes += 1
        var root := scene.instantiate()
        expect(root.scene_file_path == SkydotPack.normalize_vpath(vpath),
               "an instance names its model: " + root.scene_file_path)
        for mi in _find_meshes(root):
            var mesh: Mesh = mi.mesh
            for s in mesh.get_surface_count():
                surfaces += 1
                # Pack meshes carry no images; the extras name the textures.
                var base := mi.get_active_material(s) as BaseMaterial3D
                expect(base != null and base.albedo_texture == null and base.has_meta("extras"),
                       "the glTF material has extras and no image")
        root.free()
    expect(pack.load_scene(meshes[0]) == pack.load_scene(meshes[0]), "scenes are cached")

    print("smoke_assets: scenes=", scenes, " surfaces=", surfaces)
    expect(scenes == 4, "4 scenes")
    expect(surfaces == 4, "4 surfaces")

    # The interior cell builds with its cube, door and lever in place, their
    # Skyrim materials textured from the extras.
    var world := pack.open_world()
    expect(world != null, "world.fb opens: " + pack.get_error())
    if world != null:
        var cell := world.build_cell(world.find_cell("TestpackInterior"))
        var stats: Dictionary = cell.get_meta("skydot_stats")
        expect(stats["placed"] == 3 and stats["lights"] == 1 and stats["missing"].is_empty(),
               "the interior builds completely: %s" % stats)
        expect(stats["materials"] == 3, "each surface gets a Skyrim material: %s" % stats)
        var converted := 0
        for mi in _find_meshes(cell):
            var m := mi.get_surface_override_material(0) as ShaderMaterial
            if m == null:
                continue
            converted += 1
            if m.get_shader_parameter("albedo_tex") == albedo_tex:
                albedo += 1
            if m.get_shader_parameter("normal_tex") is Texture2D:
                normals += 1
        expect(converted == 3, "as a surface override")
        expect(albedo == 3 and normals == 3, "each with its albedo and normal map: %d, %d" % [albedo, normals])
        _check_pick(world, cell)
        cell.free()
        _check_exterior(world)

## The door stands 200 units north of the cube (Godot -Z), the lever 100 east.
func _check_pick(world: SkydotWorld, cell: Node3D) -> void:
    const S := 0.0142875
    var holder := Node3D.new()
    holder.add_child(cell)
    var door := world.pick_ref(holder, Vector3(0, 0, -100 * S), Vector3(0, 0, -300 * S))
    expect(door.get("ref", 0) == 0x303, "looking north picks the door: %s" % door)
    if not door.is_empty():
        # The cube model is 24 units wide, so the door's south face is 12 units
        # short of its position.
        expect(is_equal_approx(door["distance"], 88 * S), "at the door's face: %s" % door["distance"])
        expect(door["node"].get_meta("skydot_cell") == 0x300, "tagged with its cell")
    var lever := world.pick_ref(holder, Vector3(0, 0, 0), Vector3(300 * S, 0, 0))
    expect(lever.get("ref", 0) == 0x304, "looking east from inside the cube picks the lever: %s" % lever)
    var nothing := world.pick_ref(holder, Vector3(0, 0, 100 * S), Vector3(0, 0, 300 * S))
    expect(nothing.is_empty(), "looking south picks nothing: %s" % nothing)
    # The plain cube has no script and is a STAT, so it is not activatable.
    var cube := world.pick_ref(holder, Vector3(-300 * S, 0, 0), Vector3(-10 * S, 0, 0))
    expect(cube.is_empty(), "a static is never picked: %s" % cube)
    holder.remove_child(cell)
    holder.free()

## The origin cell of the test worldspace: two cubes, the worldspace's
## persistent light, and a terrain slope whose quadrant 0 has a second layer.
func _check_exterior(world: SkydotWorld) -> void:
    var world_id := world.find_world("TestpackWorld")
    expect(world_id != 0, "the test worldspace is found by editor id")
    expect(world.get_exterior_cell(world_id, 0, 0) != 0, "and has a cell at (0, 0)")
    expect(world.build_exterior(world_id, 5, 5) == null, "nothing where there is no cell or land")

    expect(world.warm_up() == 29, "every shader variant is created up front, particles included")

    # The test climate: one weather, sunrise 6-8, sunset 18-20, sky upper blue
    # by day and black at night.
    var noon := world.get_sky(world_id, 12.0)
    expect(noon.get("weather", "") == "TestpackClear", "the climate's weather is chosen: %s" % noon)
    if not noon.is_empty():
        expect(noon["sky_upper"].is_equal_approx(Color(0, 0, 1)) and noon["daylight"] == 1.0,
               "noon is day: %s" % noon["sky_upper"])
        expect(noon["sun_direction"].y > 0.9, "the sun is high at noon")
        expect(is_equal_approx(noon["fog_far"], 50000.0 * 0.0142875), "day fog distance")
        var midnight := world.get_sky(world_id, 0.0)
        expect(midnight["sky_upper"].is_equal_approx(Color(0, 0, 0)) and midnight["daylight"] == 0.0,
               "midnight is night")
        var dawn := world.get_sky(world_id, 7.0)
        expect(dawn["sun_direction"].x > 0.5, "the sun rises in the east")
    var resources := world.get_exterior_resources(world_id, 0, 0)
    expect(resources.has("meshes/testpack/cube_se.nif")
           and resources.has("textures/testpack/cube.dds"),
           "the origin needs its cube and its land texture: %s" % resources)
    var polls := 0
    while world.request_exterior(world_id, 0, 0) != 0 and polls < 2000:
        OS.delay_msec(1)
        polls += 1
    expect(world.request_exterior(world_id, 0, 0) == 0, "its resources finish loading")
    expect(world.get_pending_resource_count() == 0 and world.get_cached_resource_count() >= 4,
           "and end up cached, none pending")

    var origin := world.build_exterior(world_id, 0, 0)
    if origin == null:
        expect(false, "the origin cell builds")
        return
    var stats: Dictionary = origin.get_meta("skydot_stats")
    expect(stats["placed"] == 3 and stats["lights"] == 1 and stats["terrain"],
           "two cubes, a door, the persistent light and terrain: %s" % stats)
    var terrain := origin.find_child("Terrain", false, false)
    var quadrants := terrain.find_children("Quadrant*", "MeshInstance3D", false, false)
    expect(quadrants.size() == 4, "one mesh per quadrant")
    expect(terrain.get_node_or_null("Collision") is StaticBody3D, "and a body to walk on")
    var textured := 0
    for q in quadrants:
        var material := (q as MeshInstance3D).mesh.surface_get_material(0) as ShaderMaterial
        if material != null and material.get_shader_parameter("albedo_0") != null:
            textured += 1
    expect(textured == 4, "every quadrant has its base texture")
    var q0 := (quadrants[0] as MeshInstance3D).mesh.surface_get_material(0) as ShaderMaterial
    # Its second layer is the default texture, which the test pack lacks, so
    # check the shader rather than the texture.
    expect(q0.shader.code.contains("uniform sampler2D albedo_1"), "quadrant 0 has a second layer")
    var q1 := (quadrants[1] as MeshInstance3D).mesh.surface_get_material(0) as ShaderMaterial
    expect(not q1.shader.code.contains("albedo_1"), "quadrant 1 has only its base")
    # The slope rises 8 units per vertex eastward: the east half spans
    # 128 to 256 units.
    var east := (quadrants[1] as MeshInstance3D).get_aabb()
    const S := 0.0142875
    expect(is_equal_approx(east.position.y, 128 * S) and is_equal_approx(east.end.y, 256 * S),
           "heights decode: %s" % east)
    origin.free()

    # The same cell in steps: with no budget each call places one reference,
    # and the result matches the whole build.
    var stepped := world.begin_exterior(world_id, 0, 0)
    expect(stepped != null and stepped.find_child("Terrain", false, false) != null,
           "a stepped build starts with its terrain")
    var steps := 1
    while not world.continue_build(stepped, 0):
        steps += 1
        if steps > 100:
            break
    expect(steps > 1, "and takes more than one step (%d)" % steps)
    expect(stepped.get_meta("skydot_stats") == stats, "ending as the whole build did")
    expect(world.continue_build(stepped, 0), "a finished build has nothing left")
    stepped.free()

    var east_cell := world.build_exterior(world_id, 1, 0)
    var east_stats: Dictionary = east_cell.get_meta("skydot_stats")
    expect(not east_stats["terrain"] and east_stats["placed"] == 2,
           "the cell to the east has no LAND: %s" % east_stats)
    east_cell.free()

func _find_meshes(node: Node) -> Array:
    var found := []
    if node is MeshInstance3D and node.mesh != null:
        found.append(node)
    for child in node.get_children():
        found.append_array(_find_meshes(child))
    return found
