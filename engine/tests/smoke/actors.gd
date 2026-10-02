# SPDX-License-Identifier: GPL-3.0-or-later
#
# Walking actors on the test pack (converter/tools/testpack): the race's
# behaviour project names an idle, a walk (70 units a second, 1 m/s) and a
# run (4 m/s); the interior's actor 0x307 is a SkydotActor with those clips.
# On a floor under the interior's navmesh it walks to a point, playing its
# walk, and idles when there. Needs the test pack.
extends SceneTree

const ACTOR := "meshes/testpack/actor/"
const S := 0.0142875

var failures := 0

func _initialize() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_actors: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_actors: SKIP (SKYDOT_TESTPACK unset)")
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

    var plan: Dictionary = world.get_actor_plan(0x307)
    expect(plan.get("behaviour", "") == ACTOR + "behaviour.hkx", "the plan names the behaviour project: %s" % plan)
    var loco: Dictionary = world.get_locomotion(plan.get("behaviour", ""))
    expect(loco.get("missing", "?") == "", "the project has locomotion: %s" % loco)
    expect(loco["idle"]["name"] == "MT_Idle" and loco["idle"]["file"] == ACTOR + "animations/mt_idle.hkx",
           "idle from the behaviour's clip generator: %s" % loco["idle"])
    expect(loco["walk"]["name"] == "MT_WalkForward" and loco["walk"]["file"] == ACTOR + "animations/mt_walkforward.hkx",
           "walk: %s" % loco["walk"])
    expect(is_equal_approx(loco["walk"]["speed"], 70.0), "walk speed from root motion: %s" % loco["walk"])
    expect(is_equal_approx(loco["run"]["speed"], 280.0), "run speed: %s" % loco["run"])
    expect(world.get_locomotion("meshes/nothing/here.hkx")["missing"] != "", "an unknown project has none")

    var cell := world.build_cell(world.find_cell("TestpackInterior"))
    var actor := cell.find_child("0x00000307*", false, false) as SkydotActor
    expect(actor != null, "the placed actor is a SkydotActor")
    if actor == null:
        cell.free()
        _finish()
        return
    var anim := actor.get_node("AnimationPlayer") as AnimationPlayer
    expect(anim.has_animation("idle") and anim.has_animation("walk") and anim.has_animation("run"),
           "idle, walk and run clips: %s" % anim.get_animation_list())
    expect(is_equal_approx(actor.walk_speed, 70.0 * S), "walks at the clip's speed: %f" % actor.walk_speed)
    actor.wander = false

    # The interior has a navmesh but no floor: give it one.
    var floor := StaticBody3D.new()
    floor.collision_layer = SkydotPlayer.LAYER_WORLD
    var shape := CollisionShape3D.new()
    var box := BoxShape3D.new()
    box.size = Vector3(30, 1, 30)
    shape.shape = box
    shape.position = Vector3(0, -0.5, 0)
    floor.add_child(shape)
    root.add_child(floor)
    root.add_child(cell)
    await ticks(30)
    expect(actor.get_state() == "idle", "stands on the floor: %s" % actor.get_state())
    expect(actor.collision_layer == SkydotPlayer.LAYER_NPC, "on the NPC layer")

    var target := SkydotWorld.skyrim_position(Vector3(150, -350, 0))
    var start := actor.global_position
    expect(actor.walk_to(target, false), "a path leads across the navmesh")
    await ticks(30)
    expect(actor.get_state() == "walk", "walking: %s" % actor.get_state())
    expect(anim.current_animation == "walk", "plays its walk: %s" % anim.current_animation)
    var speed := Vector2(actor.velocity.x, actor.velocity.z).length()
    expect(speed > 0.5 and speed < 1.2, "at about 1 m/s: %f" % speed)
    await ticks(600)
    var end := actor.global_position
    var left := Vector2(end.x - target.x, end.z - target.z).length()
    expect(left < 0.5, "arrives (%.2f m from the target, started %.2f m away)" % [left, start.distance_to(target)])
    expect(actor.get_state() == "idle" and anim.current_animation == "idle",
           "and idles: %s, %s" % [actor.get_state(), anim.current_animation])
    expect(absf(end.y) < 0.1, "on the floor: %f" % end.y)

    _finish()

func _finish() -> void:
    print("smoke_actors: failures=", failures)
    quit(failures)
