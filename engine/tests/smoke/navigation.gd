# SPDX-License-Identifier: GPL-3.0-or-later
#
# Navmeshes on the test pack (converter/tools/testpack): the origin cell's
# navmesh follows its slope and links across x = 4096 to the east cell's,
# which sits 30 units higher at the border; the interior's has a door
# triangle. A path crosses the border only while both cells are built.
extends SceneTree

const S := 0.0142875

var failures := 0

func _initialize() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_navigation: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_navigation: SKIP (SKYDOT_TESTPACK unset)")
        quit(77)
        return
    _run(pack_dir)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

## Wait until the navigation map has taken in the regions added so far.
func sync() -> void:
    for i in 10:
        await physics_frame

func build(world: SkydotWorld, world_id: int, x: int) -> Node3D:
    while world.request_exterior(world_id, x, 0) != 0:
        OS.delay_msec(1)
    var cell := world.build_exterior(world_id, x, 0)
    root.add_child(cell)
    return cell

func path(from: Vector3, to: Vector3) -> PackedVector3Array:
    return NavigationServer3D.map_get_path(root.world_3d.navigation_map,
        SkydotWorld.skyrim_position(from), SkydotWorld.skyrim_position(to), true)

func _run(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    var world := pack.open_world()
    if world == null:
        expect(false, "world.fb opens")
        _finish()
        return
    var world_id := world.find_world("TestpackWorld")
    var origin_id := world.get_exterior_cell(world_id, 0, 0)
    var east_id := world.get_exterior_cell(world_id, 1, 0)

    var navmeshes := world.get_navmeshes(origin_id)
    expect(navmeshes.size() == 1, "the origin cell has one navmesh: %s" % [navmeshes])
    if navmeshes.size() == 1:
        var nav: Dictionary = navmeshes[0]
        expect(nav["id"] == 0x216 and nav["triangles"] == 2 and nav["vertices"] == 4,
               "the origin navmesh: %s" % nav)
        var links: Array = nav["links"]
        expect(links.size() == 1, "one link: %s" % [links])
        if links.size() == 1:
            var link: Dictionary = links[0]
            expect(link["type"] == 0 and link["navmesh"] == 0x223 and link["cell"] == east_id,
                   "the link leads to the east cell's navmesh: %s" % link)
            # From the middle of the border edge to the east triangle's centre.
            var from: Vector3 = SkydotWorld.godot_to_skyrim(link["from"])
            expect(from.is_equal_approx(Vector3(4096, 2048, 256)), "the link leaves the border: %s" % from)
    expect(world.get_navmesh(0x223).get("cell") == east_id, "navmeshes are found by id")
    expect(world.get_navmesh(0x999).is_empty(), "an unknown navmesh is empty")
    var inside: Array = world.get_navmeshes(world.find_cell("TestpackInterior"))
    expect(inside.size() == 1 and inside[0]["doors"].size() == 1 and inside[0]["doors"][0]["door"] == 0x303,
           "the interior navmesh knows its door: %s" % [inside])

    var origin := build(world, world_id, 0)
    var regions := origin.find_child("Navmesh", false, false)
    expect(regions != null and regions.get_child_count() == 1 and regions.get_child(0) is NavigationRegion3D,
           "the cell has a navigation region")
    await sync()

    # Within the origin cell, the path follows the slope (z = x / 16).
    var within := path(Vector3(500, 3000, 500.0 / 16), Vector3(3500, 1000, 3500.0 / 16))
    expect(within.size() >= 2, "a path within the cell: %s" % within)
    if within.size() >= 2:
        var end := SkydotWorld.godot_to_skyrim(within[within.size() - 1])
        expect(absf(end.x - 3500) < 1 and absf(end.z - 3500.0 / 16) < 1, "it ends on the slope: %s" % end)

    # Without the east cell, a path east stops at the border.
    var blocked := path(Vector3(1000, 2000, 62.5), Vector3(6000, 2000, 286))
    if blocked.size() > 0:
        var end := SkydotWorld.godot_to_skyrim(blocked[blocked.size() - 1])
        expect(end.x <= 4097, "without the east cell the path stops at the border: %s" % end)

    var east := build(world, world_id, 1)
    await sync()
    var across := path(Vector3(1000, 2000, 62.5), Vector3(6000, 2000, 286))
    expect(across.size() >= 2, "a path across the border: %s" % across)
    if across.size() >= 2:
        var end := SkydotWorld.godot_to_skyrim(across[across.size() - 1])
        expect(absf(end.x - 6000) < 1 and absf(end.y - 2000) < 1,
               "the path reaches the east cell: %s" % end)

    east.free()
    origin.free()
    world.navigation = false
    var bare := world.build_exterior(world_id, 0, 0)
    expect(bare.find_child("Navmesh", false, false) == null, "navigation off builds no regions")
    bare.free()
    _finish()

func _finish() -> void:
    print("smoke_navigation: failures=", failures)
    quit(failures)
