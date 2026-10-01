# SPDX-License-Identifier: GPL-3.0-or-later
#
# SkydotWeather on the test pack (converter/tools/testpack): the climate
# offers TestpackClear everywhere; a region over the cell at (1, 0) offers
# TestpackRain, with a cloud layer, rain and frequent thunder. Time runs at
# the time scale and the weather fades from one to the next.
extends SceneTree

const S := 0.0142875

var failures := 0

func _initialize() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_weather: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_weather: SKIP (SKYDOT_TESTPACK unset)")
        quit(77)
        return
    _run(pack_dir)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func frames(count: int) -> void:
    for i in count:
        await process_frame

func _run(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    var world := pack.open_world()
    if world == null:
        expect(false, "world.fb opens")
        _finish()
        return
    var world_id := world.find_world("TestpackWorld")
    var clear := world.find_weather("TestpackClear")
    var rain := world.find_weather("TestpackRain")
    expect(clear != 0 and rain != 0, "both weathers are in world.fb")

    var camera := Camera3D.new()
    root.add_child(camera)
    camera.position = SkydotWorld.skyrim_position(Vector3(2048, 2048, 500))
    var weather := SkydotWeather.new()
    root.add_child(weather)
    expect(weather.setup(world, world_id, camera) == OK, "set up in the test worldspace")
    var state: Dictionary = weather.get_state()
    expect(state["weather"] == clear and state["region"] == 0,
           "outside the region the climate's weather: %s" % state)

    # Time: noon plus one game hour after a real 1.8 s at 2000x.
    weather.hour = 12.0
    weather.time_scale = 2000.0
    var started := Time.get_ticks_msec()
    while Time.get_ticks_msec() - started < 1800:
        await process_frame
    expect(absf(weather.hour - 13.0) < 0.2, "time runs at the time scale: %.2f" % weather.hour)
    weather.time_scale = 0.0
    weather.hour = 23.9
    weather.time_scale = 3600.0
    while weather.day == 0 and Time.get_ticks_msec() - started < 5000:
        await process_frame
    expect(weather.day == 1, "midnight starts the next day")
    weather.time_scale = 0.0

    # In the region: its weather, faded in; rain follows, thunder flashes.
    var flashes := []
    weather.lightning.connect(func() -> void: flashes.append(true))
    camera.position = SkydotWorld.skyrim_position(Vector3(6000, 2048, 500))
    expect(weather.next_weather() == rain, "the region offers the rain")
    state = weather.get_state()
    expect(state["region"] == 0x134 and state["previous"] == clear and state["transition"] < 0.5,
           "fading from the clear weather: %s" % state)
    started = Time.get_ticks_msec()
    while weather.get_state()["transition"] < 1.0 and Time.get_ticks_msec() - started < 60000:
        await process_frame
    state = weather.get_state()
    expect(state["transition"] == 1.0 and state["precipitation"] == 1.0,
           "after the fade it rains: %s" % state)
    var drops := weather.find_child("Precipitation", true, false) as GPUParticles3D
    expect(drops != null and drops.emitting and drops.amount > 0, "rain particles fall")
    expect(not flashes.is_empty(), "thunder flashed during the storm")

    weather.set_weather(clear, 0.0)
    await frames(2)
    state = weather.get_state()
    expect(state["weather"] == clear and state["precipitation"] == 0.0, "back to clear at once: %s" % state)
    weather.free()
    camera.free()
    _finish()

func _finish() -> void:
    print("smoke_weather: failures=", failures)
    quit(failures)
