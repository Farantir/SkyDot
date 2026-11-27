# SPDX-License-Identifier: GPL-3.0-or-later
#
# SkydotPlayer in deep water (no pack): swimming forward looking 60 degrees
# down dives (the eye goes under, the breath runs down); without input it
# floats back up to the chest just under the surface and its breath is back.
extends SceneTree

var failures := 0

func _initialize() -> void:
    _run()

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func ticks(count: int) -> void:
    for i in count:
        await physics_frame

func _run() -> void:
    var hz := Engine.physics_ticks_per_second
    var player := SkydotPlayer.new()
    root.add_child(player)
    await process_frame
    player.water_height = 10.0
    player.teleport(Vector3(0, 10.0 - 1.35, 0))  # the chest at its rest
    player.set_look(0.0, deg_to_rad(-60.0))
    await ticks(2)
    expect(player.is_swimming(), "the player swims")
    expect(not player.is_underwater(), "the eye starts above the surface")

    player.set_input(Vector2(0, 1), 0, SkydotPlayer.RUN)
    await ticks(hz)
    var eye := player.global_position.y + player.eye_height
    expect(player.is_underwater() and eye < 10.0, "forward looking down dives: eye %.2f" % eye)
    expect(player.global_position.z < -0.5, "and moves forward: %s" % player.global_position)
    await ticks(hz)
    expect(player.get_breath() < 1.0, "the breath runs down: %.3f" % player.get_breath())

    # Moving level under water keeps the depth (once the dive has slowed down).
    player.set_look(0.0, 0.0)
    await ticks(hz)
    var deep := player.global_position.y
    await ticks(hz)
    expect(absf(player.global_position.y - deep) < 0.1, "level stays deep: %.2f from %.2f" % [player.global_position.y, deep])

    player.set_input(Vector2.ZERO, 0, SkydotPlayer.RUN)
    await ticks(hz * 10)
    var chest := player.global_position.y + 1.25
    expect(absf(chest - 9.9) < 0.05, "without input it floats back up: chest %.2f" % chest)
    expect(not player.is_underwater() and is_equal_approx(player.get_breath(), 1.0), "breath back above water")

    # Forward looking up stops at the surface rather than leaving the water.
    player.set_look(0.0, deg_to_rad(60.0))
    player.set_input(Vector2(0, 1), 0, SkydotPlayer.RUN)
    await ticks(hz * 2)
    expect(player.is_swimming() and player.global_position.y + 1.25 < 10.0, "looking up stays in the water")

    player.free()
    if failures == 0:
        print("smoke_swim: OK")
    quit(failures)
