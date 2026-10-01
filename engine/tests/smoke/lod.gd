# SPDX-License-Identifier: GPL-3.0-or-later
#
# SkydotLod over the test pack's worldspace LOD (converter/tools/testpack:
# settings with levels 4 to 8 over 8 cells from (0, 0), terrain LOD for the
# level-8 quad and the level-4 quad at (0, 0), object LOD and two trees for
# the latter). Needs the test pack.
extends SceneTree

const CELL := 4096.0

var failures := 0

func _init() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_lod: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_lod: SKIP (SKYDOT_TESTPACK unset)")
        quit(77)
        return
    _run(pack_dir)
    print("smoke_lod: failures=", failures)
    quit(failures)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

## Update until nothing is pending (loads run on threads).
func settle(lod: SkydotLod, at: Vector3) -> Dictionary:
    var camera := SkydotWorld.skyrim_position(at)
    for i in 500:
        if lod.update(camera, 1000000) == 0:
            break
        OS.delay_msec(5)
    return lod.get_stats()

func _run(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    var world := pack.open_world()
    if world == null or failures > 0:
        expect(false, "world.fb opens")
        return
    var world_id := world.find_world("TestpackWorld")

    var lod := SkydotLod.new()
    expect(lod.setup(pack, world, world_id) == OK, "LOD sets up: " + lod.get_error())
    root.add_child(lod)
    var none := SkydotLod.new()
    expect(none.setup(pack, world, 0) != OK, "a worldspace without LOD settings is refused")
    none.free()

    # Far away: the one level-8 quad, no trees.
    var stats := settle(lod, Vector3(40 * CELL, 40 * CELL, 0))
    expect(stats["levels"] == {8: 1}, "far off, the level-8 quad: %s" % stats)
    expect(stats["trees"] == 0, "and no trees")
    expect(lod.get_node_or_null("quad_8_0_0") != null, "named after its level and corner")

    # Close: the level-8 quad splits into four level-4 quads, one of which
    # has terrain, objects and two trees; the level-8 quad goes.
    stats = settle(lod, Vector3(1.5 * CELL, 1.5 * CELL, 0))
    expect(stats["levels"] == {4: 4}, "close, four level-4 quads: %s" % stats)
    expect(stats["objects"] == 1, "one with objects")
    expect(stats["tree_quads"] == 1 and stats["trees"] == 2, "and two trees: %s" % stats)
    var coarse := lod.get_node_or_null("quad_8_0_0")
    expect(coarse == null or coarse.is_queued_for_deletion(), "the level-8 quad is dropped")

    var quad := lod.get_node_or_null("quad_4_0_0")
    expect(quad != null and quad.get_child_count() == 2, "terrain and objects under the quad")
    var shaded := 0
    for mi in quad.find_children("*", "MeshInstance3D", true, false):
        if (mi as MeshInstance3D).get_surface_override_material(0) is ShaderMaterial:
            shaded += 1
    expect(shaded == 2, "both meshes get the LOD shaders (%d)" % shaded)
    var trees := lod.get_node_or_null("trees_4_0_0")
    expect(trees != null and trees.get_child(0) is MultiMeshInstance3D, "trees are one MultiMesh")
    if trees != null:
        var mm: MultiMesh = trees.get_child(0).multimesh
        expect(mm.instance_count == 2, "of two billboards")
        # The tallest tree is type 1 (512 units) at scale 2. The headless
        # renderer keeps no instance data, so the size is checked through the
        # bounds, which are grown by it.
        var bounds: AABB = trees.get_child(0).custom_aabb
        expect(is_equal_approx(bounds.size.y, 2.0 * 1024.0 * 0.0142875),
               "billboards sized from the tree list and scale: %s" % bounds)

    # Loaded cells are counted into the mask.
    lod.set_cell_loaded(0, 0, true)
    lod.set_cell_loaded(1, 0, true)
    lod.set_cell_loaded(1, 0, true)
    lod.set_cell_loaded(100, 100, true)  # outside the grid: ignored
    expect(lod.get_stats()["loaded_cells"] == 2, "two cells masked")
    lod.set_cell_loaded(0, 0, false)
    expect(lod.get_stats()["loaded_cells"] == 1, "one again")
    lod.clear_loaded_cells()
    expect(lod.get_stats()["loaded_cells"] == 0, "none after clearing")

    # Far again: back to one quad.
    stats = settle(lod, Vector3(40 * CELL, 40 * CELL, 0))
    expect(stats["levels"] == {8: 1} and stats["trees"] == 0, "and back: %s" % stats)
    lod.queue_free()
