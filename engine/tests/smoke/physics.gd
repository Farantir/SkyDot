# SPDX-License-Identifier: GPL-3.0-or-later
#
# Physics on the test pack (converter/tools/testpack): the origin cell's
# terrain (rising 8 units per vertex eastward) and its cubes' static boxes
# (24 units across) collide; the LE cube, whose box is clutter, falls once
# woken. SkydotPlayer lands on the slope, walks up it, climbs a step, stops
# at a wall, and pushes clutter. Needs the test pack.
extends SceneTree

const S := 0.0142875

var failures := 0

func _initialize() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_physics: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_physics: SKIP (SKYDOT_TESTPACK unset)")
        quit(77)
        return
    _run(pack_dir)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func ticks(count: int) -> void:
    for i in count:
        await physics_frame

## The first hit straight down from `from` (Godot space), or {}.
func ray_down(from: Vector3, mask := 0xFFFFFFFF) -> Dictionary:
    var query := PhysicsRayQueryParameters3D.create(from, from - Vector3(0, 100, 0), mask)
    return root.world_3d.direct_space_state.intersect_ray(query)

func _run(pack_dir: String) -> void:
    expect(ProjectSettings.get_setting("physics/3d/physics_engine") == "Jolt Physics",
           "the project runs on Jolt")
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    var world := pack.open_world()
    if world == null:
        expect(false, "world.fb opens")
        _finish()
        return
    var world_id := world.find_world("TestpackWorld")
    while world.request_exterior(world_id, 0, 0) != 0:
        OS.delay_msec(1)
    var cell := world.build_exterior(world_id, 0, 0)
    var stats: Dictionary = cell.get_meta("skydot_stats")
    # The cube and the door use the cube with a box; the spaced cube has none.
    expect(stats["bodies"] == 2, "two references get a body: %s" % stats)
    root.add_child(cell)
    await ticks(2)

    # Terrain: the centre column is 16 vertices east, 128 units up.
    var ground := ray_down(SkydotWorld.skyrim_position(Vector3(2048, 2048, 1000)))
    expect(not ground.is_empty() and is_equal_approx(snappedf(ground["position"].y, 0.001), snappedf(128 * S, 0.001)),
           "the terrain is solid where it is drawn: %s" % ground)
    # The cube at (12, -34, 56): its top is 12 units above its centre.
    var top := ray_down(SkydotWorld.skyrim_position(Vector3(12, -34, 500)))
    expect(not top.is_empty() and absf(top["position"].y - 68 * S) < 0.002,
           "the cube's box is where the cube is: %s" % top)
    var cube := top.get("collider") as Node
    expect(cube is StaticBody3D and cube.collision_layer == 1, "a static body on the world layer")

    await _walk_the_slope()
    await _steps_and_walls(pack)
    cell.free()
    _finish()

func _finish() -> void:
    print("smoke_physics: failures=", failures)
    quit(failures)

## Drop the player onto the slope, then run east, uphill.
func _walk_the_slope() -> void:
    var player := SkydotPlayer.new()
    root.add_child(player)
    player.teleport(SkydotWorld.skyrim_position(Vector3(1024, 2048, 600)))
    await ticks(120)
    var ground := 64 * S  # 8 vertices east
    expect(player.is_on_floor(), "the player lands")
    expect(absf(player.global_position.y - ground) < 0.05,
           "on the terrain: %.3f against %.3f" % [player.global_position.y, ground])
    var start := player.global_position
    player.set_look(-PI / 2, 0)  # facing east
    player.set_input(Vector2(0, 1), 0, SkydotPlayer.RUN)
    await ticks(60)
    var moved := player.global_position - start
    expect(moved.x > 3.5 and absf(moved.z) < 0.1, "one second runs about 5 m east: %s" % moved)
    expect(player.is_on_floor() and moved.y > 0.15, "uphill, on the ground: %s" % moved)
    player.set_input(Vector2.ZERO, 0, SkydotPlayer.RUN)
    player.jump()
    await ticks(10)
    expect(not player.is_on_floor() and player.global_position.y > start.y + moved.y + 0.3,
           "a jump leaves the ground")
    await ticks(90)
    expect(player.is_on_floor(), "and comes back down")
    player.fly = true
    player.set_input(Vector2.ZERO, 1, SkydotPlayer.RUN)
    var flying := player.global_position.y
    await ticks(30)
    expect(player.global_position.y > flying + 2, "flying ignores gravity and goes up")
    player.free()

## A floor, a 0.3 m step and a wall built here, away from the cell; then the
## LE cube as clutter.
func _steps_and_walls(pack: SkydotPack) -> void:
    var base := Vector3(0, -100, 500)
    var floor := _box(base + Vector3(0, -0.5, 0), Vector3(40, 1, 40))
    var step := _box(base + Vector3(3, 0.15, 0), Vector3(2, 0.3, 4))
    var wall := _box(base + Vector3(8, 1, 0), Vector3(0.5, 2, 4))
    await ticks(2)

    var player := SkydotPlayer.new()
    root.add_child(player)
    player.teleport(base + Vector3(0, 0.1, 0))
    await ticks(30)
    expect(player.is_on_floor(), "stands on the floor")
    player.set_look(-PI / 2, 0)
    player.set_input(Vector2(0, 1), 0, SkydotPlayer.WALK)
    await ticks(150)  # 2.5 s at walking pace: onto the step
    expect(absf(player.global_position.y - (base.y + 0.3)) < 0.03 and player.global_position.x > base.x + 2.2,
           "climbs the 0.3 m step: %s" % (player.global_position - base))
    player.set_input(Vector2(0, 1), 0, SkydotPlayer.SPRINT)
    await ticks(120)
    expect(player.global_position.x < base.x + 7.75 - 0.29, "stops at the wall: %s" % (player.global_position - base))
    expect(player.global_position.x > base.x + 7.0, "after walking up to it: %s" % (player.global_position - base))

    # Clutter: frozen as placed, falls when woken.
    var model: SkydotModel = pack.load_scene("meshes/testpack/cube_le.nif")
    expect(model != null and model.get_body_count() == 1, "the LE cube has one body")
    if model != null:
        var crate := model.instantiate() as Node3D
        crate.position = base + Vector3(-3, 1.0, 0)
        expect(model.attach_collision(crate) == 1, "attached to an instance")
        root.add_child(crate)
        var bodies := crate.find_children("*", "SkydotDynamicBody", true, false)
        expect(bodies.size() == 1, "as a dynamic body")
        await ticks(30)
        expect(absf(crate.position.y - (base.y + 1.0)) < 0.001, "it stays put until woken")
        if bodies.size() == 1:
            var body: RigidBody3D = bodies[0]
            expect(body.collision_layer == 2 and is_equal_approx(body.mass, 5.0), "clutter weighing 5 kg")
            body.wake()
            await ticks(120)
            expect(absf(crate.position.y - (base.y + 12 * S)) < 0.03,
                   "and lands on the floor when woken: %.3f" % (crate.position.y - base.y))
            # Walk into it from the east: it gets pushed west.
            var at := crate.position.x
            player.teleport(Vector3(crate.position.x + 1.5, base.y, base.z))
            player.set_look(PI / 2, 0)  # facing west
            player.set_input(Vector2(0, 1), 0, SkydotPlayer.RUN)
            await ticks(60)
            expect(crate.position.x < at - 0.3, "the player pushes it: %.2f m" % (at - crate.position.x))
    player.free()
    for node in [floor, step, wall]:
        node.free()

func _box(centre: Vector3, size: Vector3) -> StaticBody3D:
    var body := StaticBody3D.new()
    var shape := CollisionShape3D.new()
    var box := BoxShape3D.new()
    box.size = size
    shape.shape = box
    body.add_child(shape)
    body.position = centre
    root.add_child(body)
    return body
