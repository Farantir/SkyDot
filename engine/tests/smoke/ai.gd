# SPDX-License-Identifier: GPL-3.0-or-later
#
# AI packages on the test pack (converter/tools/testpack): its NPC (placed
# as 0x307 in the interior, persistent) is home near where it was placed
# from 20:00 and, from 08:00, by the scaled cube (0x212) in the origin cell
# outside, unlocking the doors there. Checks which package runs when, where
# that puts the actor before anything is built, and, on a floor in the
# interior, that at 08:00 it walks to the load door (0x303), goes through,
# and unlocks the outside door (0x213). Needs the test pack.
extends SceneTree

const ACTOR := 0x307
const INTERIOR := 0x300
const WORLD := 0x1
const CUBE := 0x212
const INSIDE_DOOR := 0x303
const OUTSIDE_DOOR := 0x213
const HOME := 0x171
const WORK := 0x172

var failures := 0
var left_through := 0

func _initialize() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_ai: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_ai: SKIP (SKYDOT_TESTPACK unset)")
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

func _run(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    var world := pack.open_world()
    if world == null:
        expect(false, "world.fb opens")
        _finish()
        return

    var ai := SkydotAi.new()
    expect(ai.setup(world, null) == OK, "the AI sets up on the world")

    # Which package, and where it wants the actor.
    ai.days = 22.0 / 24.0
    var night := ai.get_actor_state(ACTOR)
    expect(night.get("package", 0) == HOME, "at 22:00 the home package runs: %s" % night)
    expect(night.get("template", "") == "TestpackSandboxTemplate", "from its template's tree: %s" % night)
    expect(night.get("steps", []) == ["sandbox"], "home: just a sandbox (no unlock on arrival): %s" % night)
    var dest := ai.get_destination(ACTOR)
    expect(dest.get("space", 0) == INTERIOR, "home is the interior: %s" % dest)
    ai.days = 1.0 + 12.0 / 24.0  # the next day
    var noon := ai.get_actor_state(ACTOR)
    expect(noon.get("package", 0) == WORK, "at 12:00 the work package runs: %s" % noon)
    expect(noon.get("steps", []) == ["travel", "unlock_doors", "sandbox"],
           "work travels, unlocks on arrival, sandboxes: %s" % noon)
    dest = ai.get_destination(ACTOR)
    expect(dest.get("space", 0) == WORLD, "work is outside: %s" % dest)
    var cube := Vector3(300, 0, 0)
    expect(dest.get("position", Vector3.ZERO).distance_to(cube) < 1.0 and dest.get("radius", 0.0) == 300.0,
           "by the cube, within 300 units: %s" % dest)
    var listed := ai.get_packages(ACTOR)
    expect(listed.size() == 2 and listed[0]["id"] == WORK and listed[0]["chosen"] and not listed[1]["scheduled"],
           "the NPC's two packages, work chosen: %s" % listed)

    # Not built: placed where the package says, and cells build it there.
    ai.set_space(WORLD)
    expect(ai.place_actors() >= 1, "placing moves the actor")
    var place := world.get_actor_place(ACTOR)
    expect(place.get("moved", false) and place.get("space", 0) == WORLD, "it is outside now: %s" % place)
    expect(Vector2(place["position"].x - 300, place["position"].y).length() <= 300.0,
           "in the sandbox around the cube: %s" % place)
    var inside := world.build_cell(INTERIOR)
    expect(inside.find_child("0x00000307*", true, false) == null, "the interior builds without it")
    inside.free()
    var outside := world.build_exterior(WORLD, 0, 0)
    expect(outside.find_child("0x00000307*", true, false) is SkydotActor, "the origin cell builds it")
    outside.free()
    world.clear_actor_places()
    expect(not world.get_actor_place(ACTOR)["moved"], "places clear")

    # Built, at 08:00: through the load door, unlocking the outside door.
    var papyrus := SkydotPapyrus.new()
    papyrus.setup(pack, world)
    expect(papyrus.get_lock_level(OUTSIDE_DOOR) == 25, "the outside door starts locked")
    ai = SkydotAi.new()
    expect(ai.setup(world, papyrus) == OK, "the AI sets up with scripts")
    ai.actor_left.connect(func(ref: int, door: int) -> void:
        if ref == ACTOR:
            left_through = door)
    ai.days = 7.99 / 24.0
    ai.time_scale = 600.0
    ai.set_space(INTERIOR)
    ai.place_actors()
    expect(not world.get_actor_place(ACTOR)["moved"], "at 07:59 it is home")
    var floor := StaticBody3D.new()
    floor.collision_layer = SkydotPlayer.LAYER_WORLD
    var shape := CollisionShape3D.new()
    var box := BoxShape3D.new()
    box.size = Vector3(30, 1, 30)
    shape.shape = box
    shape.position = Vector3(0, -0.5, 0)
    floor.add_child(shape)
    root.add_child(floor)
    var cell := world.build_cell(INTERIOR)
    root.add_child(cell)
    expect(ai.attach_built(cell) == 1, "the AI takes the actor over")
    for i in 900:
        await physics_frame
        ai.update(1.0 / Engine.physics_ticks_per_second)
        if left_through != 0:
            break
    expect(left_through == INSIDE_DOOR, "it leaves through the load door: 0x%X at %05.2f" % [left_through, ai.get_hour()])
    await ticks(2)
    expect(cell.find_child("0x00000307*", true, false) == null, "and is gone from the interior")
    place = world.get_actor_place(ACTOR)
    expect(place.get("space", 0) == WORLD, "it is outside: %s" % place)
    expect(papyrus.get_lock_level(OUTSIDE_DOOR) == -1, "the doors at work are unlocked")

    _finish()

func _finish() -> void:
    print("smoke_ai: failures=", failures)
    quit(failures)
