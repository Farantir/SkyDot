# SPDX-License-Identifier: GPL-3.0-or-later
#
# Mounts the pack from `bethconv-testpack` and queries it. Expected numbers
# come from that tool (converter/tools/testpack/main.cpp: 44 forms, 36 distinct
# assets, 40 index entries) and change with it.
extends SceneTree

var failures := 0

func _init() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_pack: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_pack: SKIP (SKYDOT_TESTPACK unset; build bethconv-testpack and point it here)")
        quit(77)
        return
    _run(pack_dir)
    print("smoke_pack: failures=", failures)
    quit(failures)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func _run(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    if not pack.is_open():
        return

    expect(pack.get_asset_count() == 36, "36 distinct assets, got %d" % pack.get_asset_count())
    expect(pack.get_index_count() == 40, "40 index entries, got %d" % pack.get_index_count())
    expect(pack.get_unknown_kind_count() == 0, "every kind is one this engine knows")

    expect(pack.get_store_layout() == "blob", "the test pack keeps its assets in a blob")

    # Every line has bytes of its kind: a GLB, a DDS, a decoded script.
    var expected := {
        "meshes/testpack/cube_se.nif": ["mesh", "glTF"],
        "meshes/testpack/cube_le.nif": ["mesh", "glTF"],
        "meshes/testpack/cube_se_copy.nif": ["mesh", "glTF"],
        "meshes/testpack/cube with space.nif": ["mesh", "glTF"],
        "textures/testpack/cube.dds": ["texture", "DDS "],
        "textures/testpack/cube_n.dds": ["texture", "DDS "],
        "textures/testpack/sky.dds": ["texture", "DDS "],
        "scripts/testpack/fixture.pex": ["script", ""],
        "scripts/testpackleverscript.pex": ["script", ""],
    }
    for vpath in expected:
        expect(pack.has(vpath), "has " + vpath)
        var bytes := pack.get_bytes(vpath)
        expect(bytes.size() > 0, "bytes for " + vpath)
        var magic: String = expected[vpath][1]
        if magic != "":
            expect(bytes.slice(0, 4).get_string_from_ascii() == magic, "magic of " + vpath)
        expect(pack.get_kind(vpath) == expected[vpath][0], "kind of " + vpath)
        expect(pack.get_hash(vpath).length() == 64, "hash of " + vpath)
        expect(pack.get_source(vpath) != "", "winning source of " + vpath)

    # Dedupe: two virtual paths, one hash, the same bytes.
    expect(pack.get_hash("meshes/testpack/cube_se.nif") == pack.get_hash("meshes/testpack/cube_se_copy.nif"),
           "the duplicate mesh shares its hash")
    expect(pack.get_bytes("meshes/testpack/cube_se.nif") == pack.get_bytes("meshes/testpack/cube_se_copy.nif"),
           "and its bytes")

    # Lookup in any spelling.
    expect(pack.get_bytes("Meshes\\TestPack\\Cube_SE.nif") == pack.get_bytes("meshes/testpack/cube_se.nif"),
           "lookup normalizes")
    expect(pack.has(SkydotPack.model_vpath("testpack\\cube_se.nif")), "a MODL field resolves through model_vpath")
    expect(pack.get_bytes("\\Meshes\\.\\TestPack//Cube_SE.nif\\") == pack.get_bytes("meshes/testpack/cube_se.nif"),
           "lookup drops a leading and a trailing separator, repeated ones and ./ segments")
    expect(SkydotPack.model_vpath("Meshes\\Ä\\Pot.nif") == "meshes/Ä/pot.nif",
           "a MODL field with a letter above ASCII keeps its case")

    # Missing paths give nothing, not a crash or a guess.
    expect(not pack.has("meshes/testpack/nope.nif") and pack.get_bytes("meshes/testpack/nope.nif").is_empty(),
           "an absent path has no bytes")
    expect(pack.get_kind("meshes/testpack/nope.nif") == "", "and has no kind")
    expect(pack.load_scene("meshes/testpack/nope.nif") == null, "and no scene")

    _check_world(pack)

    pack.close()
    expect(not pack.is_open() and not pack.has("meshes/testpack/cube_se.nif"), "closed is closed")

func _check_world(pack: SkydotPack) -> void:
    expect(pack.has_world(), "the test pack carries world.fb")
    var world := pack.open_world()
    expect(world != null and world.is_open(), "world.fb opens: " + pack.get_error())
    if world == null:
        return
    expect(world.get_cell_count() == 4, "4 cells, got %d" % world.get_cell_count())
    expect(world.get_base_count() == 7, "7 bases, got %d" % world.get_base_count())

    var id := world.find_cell("testpackinterior")
    expect(id != 0, "the interior is found by editor id, any case")
    var cell := world.get_cell(id)
    expect(cell["interior"] == true, "it is an interior")
    expect(cell["lighting"] != null, "with decoded lighting")
    if cell["lighting"] != null:
        var ambient: Color = cell["lighting"]["ambient"]
        expect(ambient.is_equal_approx(Color8(0x48, 0x38, 0x30)), "ambient color: %s" % ambient)

    var refs := world.get_refs(id)
    expect(refs.size() == 5, "5 references, got %d" % refs.size())
    var cube: Dictionary = world.get_base(refs[0]["base"])
    expect(cube.get("model", "") == "meshes/testpack/cube_se.nif", "the cube's model: %s" % cube)
    var torch: Dictionary = world.get_base(refs[1]["base"])
    expect(torch.get("light") != null, "the torch is a light")

    # Models load from the pack itself: cube, door and lever, and the light.
    var root := world.build_cell(id)
    var stats: Dictionary = root.get_meta("skydot_stats")
    expect(stats["lights"] == 1, "one light built: %s" % stats)
    expect(stats["placed"] == 3 and stats["missing"].is_empty(), "three models placed: %s" % stats)
    root.free()

    _check_doors(world, id)

## The interior's door leads to the origin cell's, and back; the lever has a
## script on its base and one on its reference.
func _check_doors(world: SkydotWorld, interior: int) -> void:
    const S := 0.0142875
    var inside := world.get_door(0x303)
    expect(inside.get("destination", 0) == 0x213, "the interior door leads outside: %s" % inside)
    if not inside.is_empty():
        expect(inside["cell"] == interior, "and stands in the interior")
        var outside_cell := world.get_exterior_cell(world.find_world("TestpackWorld"), 0, 0)
        expect(inside["destination_cell"] == outside_cell and not inside["destination_interior"],
               "into the origin cell: %s" % inside)
        expect(inside["destination_world"] == world.find_world("TestpackWorld"), "of the test worldspace")
        var arrival: Transform3D = inside["arrival"]
        expect(arrival.origin.is_equal_approx(SkydotWorld.skyrim_position(Vector3(1000, 900, 0))),
               "arriving south of the outside door: %s" % arrival.origin)
        # Turned half a circle: facing south (Godot +Z), away from the door.
        expect((-arrival.basis.z).is_equal_approx(Vector3(0, 0, 1)),
               "facing away from it: %s" % (-arrival.basis.z))
    var outside := world.get_door(0x213)
    expect(outside.get("destination", 0) == 0x303 and outside.get("destination_interior", false),
           "the outside door leads back in: %s" % outside)
    expect(world.get_door(0x301).is_empty(), "the cube is not a door")

    var info := world.get_ref_info(world.get_exterior_cell(world.find_world("TestpackWorld"), 0, 0), 0x213)
    expect(info.get("type", "") == "DOOR" and info["activatable"], "the outside door is an activatable DOOR: %s" % info)
    if not info.is_empty():
        expect(info["lock"] != null and info["lock"]["level"] == 25, "locked at level 25: %s" % info["lock"])
        expect(info["door"] != null and info["door"]["destination"] == 0x303, "with its door link")

    var lever := world.get_ref_info(interior, 0x304)
    expect(lever.get("editor_id", "") == "TestpackLever" and lever["activatable"], "the lever: %s" % lever)
    if not lever.is_empty():
        expect(lever["lock"] == null and lever["door"] == null, "neither locked nor a door")
        expect(lever["links"].size() == 1 and lever["links"][0]["target"] == 0x301, "linked to the cube")
        var scripts: Array = lever["scripts"]
        expect(scripts.size() == 2, "a base script and a reference script: %s" % [scripts])
        if scripts.size() == 2:
            expect(scripts[0]["name"] == "TestpackLeverScript" and not scripts[0]["from_ref"],
                   "the base's first")
            expect(scripts[0]["properties"]["Target"] == 0, "the base leaves Target empty")
            expect(scripts[1]["from_ref"] and scripts[1]["properties"]["Target"] == 0x301,
                   "the reference sets it to the cube: %s" % scripts[1])
    var cube := world.get_ref_info(interior, 0x301)
    expect(not cube.get("activatable", true), "a plain static is not activatable")
    expect(world.get_ref_info(interior, 0x999).is_empty(), "an unknown reference gives nothing")
    expect(world.get_base(0x104)["scripts"].size() == 1, "get_base lists scripts")
