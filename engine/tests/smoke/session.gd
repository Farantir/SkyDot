# SPDX-License-Identifier: GPL-3.0-or-later
#
# SkydotClock and SkydotStreamer on the test pack (converter/tools/testpack):
# the time handed between the weather outside and the AI inside, a worldspace
# streamed around the camera and dropped behind it, and the place behind a
# load door built ahead and taken over on arrival.
extends SceneTree

var failures := 0
var _finished: Array[int] = []

func _initialize() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_session: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_session: SKIP (SKYDOT_TESTPACK unset)")
        quit(77)
        return
    _run(pack_dir)  # a coroutine: the scene tree runs a frame first

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func _near(a: float, b: float) -> bool:
    return absf(a - b) < 0.0001

func _run(pack_dir: String) -> void:
    await process_frame  # nodes added to root are in the tree from here on
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    var world := pack.open_world()
    if world == null:
        expect(false, "world.fb opens")
        _finish()
        return
    var world_id := world.find_world("TestpackWorld")
    var camera := Camera3D.new()
    root.add_child(camera)
    camera.position = SkydotWorld.skyrim_position(Vector3(0, 0, 300))
    _clock(world, world_id, camera)
    _streaming(pack, world, world_id, camera)
    await _preloading(pack, world, world_id, camera)
    _finish()

func _clock(world: SkydotWorld, world_id: int, camera: Camera3D) -> void:
    var clock := SkydotClock.new()
    clock.setup(9.5, 20.0, false)
    var ai := SkydotAi.new()
    expect(ai.setup(world, null) == OK, "the AI sets up")
    clock.attach_ai(ai)
    expect(_near(ai.days, 9.5 / 24.0) and ai.time_scale == 20.0, "the AI starts at the clock's time")

    var weather := SkydotWeather.new()
    root.add_child(weather)
    clock.configure(weather)
    expect(weather.hour == 9.5 and weather.day == 0 and weather.time_scale == 20.0, "a weather is configured")
    expect(weather.setup(world, world_id, camera) == OK, "the weather sets up")
    clock.bind_weather(weather)
    expect(clock.has_weather() and clock.weather == weather and clock.hour == 9.5, "the weather runs the time")

    weather.hour = 23.0
    weather.day = 2
    clock.sync(false)
    expect(ai.time_scale == 0.0 and _near(ai.days, 2.0 + 23.0 / 24.0), "the AI follows the weather, stopped")
    expect(clock.shift(1.0) == "00:00" and weather.hour == 0.0, "an hour later wraps past midnight")
    expect(clock.shift(-1.0) == "23:00", "and back")
    var state := clock.describe()
    expect(state["hour"] == weather.hour and state["day"] == 2 and state["time_scale"] == 20.0,
        "outside, a shot gets hour, day and speed: %s" % [state])

    clock.release_weather()
    weather.free()
    expect(not clock.has_weather() and clock.hour == 23.0 and clock.day == 2, "the weather's time is kept")
    clock.sync(false)
    expect(ai.time_scale == 20.0 and _near(ai.days, 2.0 + 23.0 / 24.0), "inside the AI's clock runs")
    ai.days = 2.5
    expect(clock.shift(6.0) == "18:00" and _near(ai.days, 2.75), "hours later by the AI's clock")
    clock.sync(true)
    expect(ai.time_scale == 0.0 and clock.day == 2, "stopped while benchmarking")
    expect(clock.describe() == {"hour": clock.hour, "day": 2}, "inside, a shot gets hour and day only")

    # A weather that is freed without being released no longer counts.
    var gone := SkydotWeather.new()
    root.add_child(gone)
    clock.bind_weather(gone)
    gone.free()
    expect(not clock.has_weather(), "a freed weather is not running the time")
    expect(SkydotClock.new().shift(1.0) == "", "nothing keeps the time without a weather or an AI")

func _streamer(pack: SkydotPack, world: SkydotWorld, host: Node3D, camera: Camera3D) -> SkydotStreamer:
    var streamer := SkydotStreamer.new()
    streamer.setup(world, pack, host, camera, null)
    streamer.radius = 1
    streamer.lod_enabled = false
    streamer.cell_finished.connect(func(_cell: Node3D, cell_id: int) -> void: _finished.append(cell_id))
    return streamer

func _stream_all(streamer: SkydotStreamer) -> void:
    for i in 400:
        streamer.update()
        if not streamer.streaming:
            return
        OS.delay_msec(5)
    expect(false, "the cells in range finish loading")

func _streaming(pack: SkydotPack, world: SkydotWorld, world_id: int, camera: Camera3D) -> void:
    var host := Node3D.new()
    root.add_child(host)
    var streamer := _streamer(pack, world, host, camera)
    expect(SkydotStreamer.cell_at(SkydotWorld.skyrim_position(Vector3(4096.5, -1, 0))) == Vector2i(1, -1),
        "a position's grid square")
    expect(streamer.camera_cell() == Vector2i(0, 0), "the camera's cell")
    expect(is_equal_approx(streamer.view_distance(), 2.0 * 4096.0 * SkydotWorld.unit_scale() * 1.5),
        "the distance seen without LOD")
    streamer.start(world_id)
    expect(streamer.world_id == world_id, "streaming the worldspace")
    _stream_all(streamer)
    expect(not streamer.streaming and not host.get_children().is_empty(), "cells in range are built")
    expect(not _finished.is_empty() and streamer.get_loaded_cells().size() == _finished.size(),
        "each cell built was announced: %d announced, %d loaded" % [_finished.size(), streamer.get_loaded_cells().size()])
    for cell in streamer.get_loaded_cells():
        expect(cell.visible and cell.get_parent() == host, "a finished cell is shown in the host")

    # Out of range cells are dropped one cell past the radius.
    camera.position = SkydotWorld.skyrim_position(Vector3(8 * 4096, 0, 300))
    streamer.update()
    expect(streamer.get_loaded_cell(Vector2i(0, 0)) == null, "the cell behind is dropped")
    expect(streamer.change_radius(1) == 2 and streamer.change_radius(10) == 8 and streamer.change_radius(-10) == 1,
        "the radius stays within 1 and 8")
    expect(streamer.scale_lod_split(100.0) == 16.0 and streamer.scale_lod_split(0.0001) == 0.5,
        "the LOD split stays within 0.5 and 16")
    streamer.clear()
    expect(streamer.world_id == 0 and streamer.get_loaded_cells().is_empty(), "cleared")
    camera.position = SkydotWorld.skyrim_position(Vector3(0, 0, 300))

func _preloading(pack: SkydotPack, world: SkydotWorld, world_id: int, camera: Camera3D) -> void:
    var host := Node3D.new()
    root.add_child(host)
    var streamer := _streamer(pack, world, host, camera)
    var interior := world.find_cell("TestpackInterior")
    var cell := world.build_cell(interior)
    host.add_child(cell)
    streamer.register_doors(cell)
    var doors := streamer.get_load_doors()
    expect(doors.has(0x303) and doors.size() == 1, "the interior's load door is found: %s" % [doors.keys()])
    var door := world.get_door(0x303)
    var feet: Vector3 = doors[0x303].global_position

    # Away from the door nothing is prepared; at it, the place behind it.
    streamer.preload_distance = 15.0
    streamer.preload_step(feet + Vector3(0, 0, 100))
    expect(streamer.get_preparation() == null, "no preparation far from a door")
    for i in 400:
        streamer.preload_step(feet)
        var preparing := streamer.get_preparation()
        if preparing != null and preparing.done:
            break
        await process_frame
    var prepared := streamer.get_preparation()
    expect(prepared != null and prepared.done and prepared.door == 0x303 and prepared.destination == 0x213
        and not prepared.interior, "the world behind the door is prepared")
    for held in host.get_children():
        if held != cell:
            expect(held.process_mode == Node.PROCESS_MODE_DISABLED and not held.visible,
                "a place built ahead is held: " + str(held.name))

    # Going through it hands the preparation over once; a door elsewhere gets nothing.
    expect(streamer.take_prepared(world.get_door(0x213)) == null and streamer.get_preparation() == null,
        "another door's preparation is dropped")
    for i in 400:
        streamer.preload_step(feet)
        if streamer.get_preparation() != null and streamer.get_preparation().done:
            break
        await process_frame
    prepared = streamer.take_prepared(door)
    expect(prepared != null and prepared.done and streamer.get_preparation() == null, "taken on arrival")

    # Arriving in the worldspace: the held cells finish as streamed cells do.
    camera.position = SkydotWorld.skyrim_position(Vector3(0, 0, 300))
    streamer.start(world_id)
    streamer.adopt(prepared)
    _finished.clear()
    _stream_all(streamer)
    expect(not _finished.is_empty(), "prepared cells are announced when finished")
    for loaded in streamer.get_loaded_cells():
        expect(loaded.process_mode == Node.PROCESS_MODE_INHERIT and loaded.visible, "and released")

    expect(not streamer.toggle_preload() and not streamer.preload_enabled, "preloading off")
    expect(streamer.toggle_preload(), "and on")
    streamer.clear()
    expect(streamer.get_load_doors().is_empty(), "leaving forgets the load doors")

func _finish() -> void:
    print("smoke_session: failures=", failures)
    quit(failures)
