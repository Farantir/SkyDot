# SPDX-License-Identifier: GPL-3.0-or-later
#
# Large references (pack format 11): the query, and SkydotLargeRefs under
# SkydotLod.
#
# The test pack (converter/tools/testpack) is of format 10 and has none, so
# with it this checks that a pack without large references costs nothing and
# fails nothing: no layer, no stats, empty queries. The layer itself needs a
# pack with them: SKYDOT_LARGE_REF_PACK names one (a Skyrim SE conversion of
# format 11, worldspace Tamriel, with its LOD); without it that half skips.
extends SceneTree

const CELL := 4096.0

var failures := 0

func _init() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    var big_dir := OS.get_environment("SKYDOT_LARGE_REF_PACK")
    if pack_dir == "" and big_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_large_refs: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_large_refs: SKIP (SKYDOT_TESTPACK unset)")
        quit(77)
        return
    if pack_dir != "":
        _without(pack_dir)
    if big_dir != "":
        _with(big_dir)
    else:
        print("smoke_large_refs: the layer is not run (SKYDOT_LARGE_REF_PACK unset)")
    print("smoke_large_refs: failures=", failures)
    quit(failures)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

## Update until nothing is pending (loads run on threads).
func settle(lod: SkydotLod, at: Vector3) -> Dictionary:
    var camera := SkydotWorld.skyrim_position(at)
    for i in 2000:
        if lod.update(camera, 1000000) == 0:
            break
        OS.delay_msec(5)
    return lod.get_stats()

func _without(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    var world := pack.open_world()
    if world == null or failures > 0:
        expect(false, "world.fb opens")
        return
    var world_id := world.find_world("TestpackWorld")
    expect(world.get_large_ref_count(world_id) == 0, "no large references in the test pack")
    expect(world.get_large_refs(world_id, 0, 0, 5).is_empty(), "so none around a cell")
    expect(world.get_large_refs(0, 0, 0, 5).is_empty() and world.get_large_refs(world_id, 0, 0, -1).is_empty(),
            "none for no worldspace or a negative radius")
    var lod := SkydotLod.new()
    expect(lod.large_refs and lod.large_ref_radius == 5, "on to 5 cells by default")
    expect(lod.setup(pack, world, world_id) == OK, "LOD sets up: " + lod.get_error())
    expect(lod.get_large_ref_layer() == null, "without large references there is no layer")
    root.add_child(lod)
    var stats := settle(lod, Vector3(1.5 * CELL, 1.5 * CELL, 0))
    expect(not stats.has("large_refs"), "and no stats for it: %s" % stats)
    lod.set_large_refs(false)
    lod.set_large_refs(true)
    lod.set_large_ref_radius(100)
    expect(lod.large_ref_radius == 32, "the radius is limited: %d" % lod.large_ref_radius)
    lod.set_large_ref_radius(-3)
    expect(lod.large_ref_radius == 0, "and not below 0")
    lod.set_cell_large_refs(0, 0, true)  # marking a cell nobody draws in is harmless
    lod.set_cell_large_refs(0, 0, false)
    lod.set_cell_loaded(0, 0, true)
    expect(lod.get_stats()["loaded_cells"] == 1, "loaded cells still count")
    lod.queue_free()

func _with(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the large reference pack opens: " + pack.get_error())
    var world := pack.open_world()
    if world == null:
        expect(false, "its world.fb opens")
        return
    var tamriel := world.find_world("Tamriel")
    var count := world.get_large_ref_count(tamriel)
    expect(count > 1000, "Tamriel has large references: %d" % count)

    # The query: the union around a square, each once, with its placement.
    var cx := floori(-2767.13 / CELL)
    var cy := floori(-5182.99 / CELL)
    var refs := world.get_large_refs(tamriel, cx, cy, 5)
    expect(refs.size() > 100 and refs.size() < 3000, "around Whiterun's plains: %d" % refs.size())
    var seen := {}
    var in_cell := 0
    for r in refs:
        expect(not seen.has(r["ref"]), "each once: %x" % r["ref"])
        seen[r["ref"]] = true
        expect(absi(r["cell"].x - cx) <= 5 + 8 and absi(r["cell"].y - cy) <= 5 + 8, "near the window")
        if r["in_cell"] != 0:
            in_cell += 1
    # Large references are normal references of their cell too, which is how
    # the full-detail cells draw them.
    expect(in_cell == refs.size(), "every one is also a reference in a cell: %d of %d" % [in_cell, refs.size()])
    var first: Dictionary = refs[0]
    var found := false
    for ref in world.get_refs(first["in_cell"]):
        found = found or ref["id"] == first["ref"]
    expect(found, "the first is among its cell's references")
    expect(world.get_large_refs(tamriel, cx, cy, 0).size() < refs.size(), "a smaller window has fewer")

    # The layer.
    var lod := SkydotLod.new()
    expect(lod.setup(pack, world, tamriel) == OK, "Tamriel's LOD sets up: " + lod.get_error())
    var layer := lod.get_large_ref_layer()
    expect(layer != null, "with large references there is a layer")
    if layer == null:
        lod.free()
        return
    root.add_child(lod)
    var at := Vector3(cx + 0.5, cy + 0.5, 0) * CELL
    at.z = -5331
    var stats := settle(lod, at)
    var lr: Dictionary = stats["large_refs"]
    expect(lr["wanted"] > 100 and lr["shown"] == lr["wanted"] and lr["pending"] == 0, "everything wanted is built: %s" % lr)
    expect(layer.get_child_count() == lr["shown"], "as one node each")
    expect(lr["marked_cells"] == 121, "an 11 x 11 window hides its LargeRef shapes: %s" % lr)
    for node in layer.get_children():
        expect(node is Node3D, "models are Node3Ds")
        break
    var no_collision := layer.find_children("*", "CollisionObject3D", true, false).is_empty()
    expect(no_collision, "visual only: no collision bodies")
    expect(layer.find_children("*", "Light3D", true, false).is_empty(), "and no lights")
    var shadows := 0
    for mi in layer.find_children("*", "MeshInstance3D", true, false):
        if (mi as MeshInstance3D).cast_shadow != GeometryInstance3D.SHADOW_CASTING_SETTING_OFF:
            shadows += 1
    expect(shadows == 0, "and casts no shadows (%d do)" % shadows)

    # A built cell draws its own: the layer lets go of what stands there.
    var shown_before: int = lr["shown"]
    var own := 0
    for r in refs:
        if r["cell"] == Vector2i(cx, cy):
            own += 1
    expect(own > 0, "a reference stands in the camera's square")
    lod.set_cell_loaded(cx, cy, true)
    stats = settle(lod, at)
    lr = stats["large_refs"]
    expect(lr["shown"] <= shown_before - own, "built square: %d shown after %d minus %d" % [lr["shown"], shown_before, own])
    expect(lr["marked_cells"] == 121, "the window is still hidden under: %s" % lr)
    lod.set_cell_loaded(cx, cy, false)
    stats = settle(lod, at)
    expect(stats["large_refs"]["shown"] == shown_before, "dropped square: they come back")

    # Radius and switching off.
    lod.set_large_ref_radius(2)
    stats = settle(lod, at)
    expect(stats["large_refs"]["shown"] < shown_before and stats["large_refs"]["marked_cells"] == 25,
            "radius 2: %s" % stats["large_refs"])
    lod.set_large_ref_radius(5)
    lod.set_large_refs(false)
    stats = settle(lod, at)
    expect(not stats.has("large_refs") and lod.get_large_ref_layer() == null, "off: no layer")
    lod.set_large_refs(true)
    stats = settle(lod, at)
    expect(stats.has("large_refs") and stats["large_refs"]["shown"] == shown_before, "and on again")

    # Far from any large reference: nothing is drawn.
    stats = settle(lod, Vector3(-95 * CELL, -95 * CELL, 0) )
    expect(stats["large_refs"]["shown"] < shown_before / 4, "far off the layer follows: %s" % stats["large_refs"])
    lod.queue_free()
