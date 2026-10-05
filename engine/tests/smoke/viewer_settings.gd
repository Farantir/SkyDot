# SPDX-License-Identifier: GPL-3.0-or-later
#
# ViewerSettings: the viewer's options parsed once into typed fields, from
# the command line or from a shot's JSON (--from-shot, the command line
# wins), and the parts of SkydotClock that need no running world. Needs no pack.
extends SceneTree

var failures := 0

func _initialize() -> void:
    _defaults()
    _options()
    _captures()
    _errors()
    _from_shot()
    _clock()
    print("smoke_viewer_settings: failures=", failures)
    quit(failures)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func parse(args: Array) -> ViewerSettings:
    return ViewerSettings.from_arguments(PackedStringArray(args))

func _defaults() -> void:
    var s := parse(["--pack", "p", "--cell", "C"])
    expect(s.error == "", "plain arguments are fine: " + s.error)
    expect(s.pack == "p" and s.cell == "C" and s.world == "", "pack and cell")
    expect(s.time == 12.0 and s.time_scale == 20.0, "time 12, 20 game seconds per second")
    expect(s.radius == 2 and s.lod_split == 1.5 and s.build_budget_usec == 8000, "view defaults")
    expect(s.preload_doors and s.preload_distance == 15.0, "doors preload within 15 m")
    expect(s.materials and s.effects and s.grass and s.collision and s.navigation and s.actors, "builds everything")
    expect(s.ai and s.wander and s.quests and s.lod and s.shadows and s.image_space, "runs everything")
    expect(not s.pick_locks and not s.fly and not s.all_light_shadows, "locks hold, walks")
    expect(s.interactive and not s.captures, "interactive")
    expect(not s.has_at and not s.has_target and not s.has_look and not s.has_fov, "nothing placed")
    expect(s.msaa_index == 0 and s.shot_dir == "user://screenshots" and s.shot_notes, "shot and msaa defaults")

func _options() -> void:
    var s := parse(["--pack", "p", "--world", "W", "--at", "1,2,3", "--target", "4,5,6.5",
            "--look", "90,10", "--time", "22", "--time-scale", "0", "--radius", "3",
            "--lod-split", "2", "--msaa", "4", "--fov", "60", "--walk", "off", "--ai", "off",
            "--materials", "off", "--light-shadows", "all", "--preload-distance", "30",
            "--pick-locks", "on", "--weather", "SkyrimClear", "--build-budget", "4000",
            "--tiling", "2.5", "--tree-distance", "3", "--shadows", "off", "--image-space", "off",
            "--quests", "off", "--set-stage", "Q:10", "--preload-doors", "off", "--pck", "x"])
    expect(s.error == "" and s.world == "W" and s.cell == "", "a worldspace")
    expect(s.has_at and s.at == Vector3(1, 2, 3), "at in game units")
    expect(s.has_target and s.target == Vector3(4, 5, 6.5), "target in game units")
    expect(s.has_look and is_equal_approx(s.look_yaw, -deg_to_rad(90.0))
            and is_equal_approx(s.look_pitch, -deg_to_rad(10.0)), "look angles in radians")
    expect(s.time == 22.0 and s.time_scale == 0.0, "time and scale")
    expect(s.radius == 3 and s.lod_split == 2.0 and s.build_budget_usec == 4000, "view numbers")
    expect(s.msaa_index == 2, "msaa 4 is step 2")
    expect(s.has_fov and s.fov == 60.0, "fov")
    expect(s.fly, "--walk off flies")
    expect(not s.ai and not s.materials and s.all_light_shadows, "switches")
    expect(s.preload_distance == 30.0 and not s.preload_doors, "preloading")
    expect(s.pick_locks and s.weather == "SkyrimClear", "locks and weather")
    expect(s.has_tiling and s.tiling == 2.5 and s.has_tree_distance and s.tree_distance == 3.0, "lod options")
    expect(not s.shadows and not s.image_space and not s.quests and s.set_stage == "Q:10", "more switches")
    expect(s.has_pck, "--pck is noticed")

func _captures() -> void:
    var s := parse(["--pack", "p", "--cell", "C", "--screenshot", "out.png"])
    expect(s.has_screenshot and s.screenshot == "out.png" and s.captures, "a screenshot run")
    expect(not s.wander and not s.preload_doors and not s.interactive and s.fly,
            "it keeps actors still, does not preload, ignores input and flies")
    expect(parse(["--pack", "p", "--cell", "C", "--screenshot", "o", "--wander", "on"]).wander,
            "--wander on wins")
    var b := parse(["--pack", "p", "--world", "W", "--at", "0,0,0", "--benchmark", "5", "--fly-speed", "30"])
    expect(b.has_benchmark and b.benchmark == 5.0 and b.fly_speed == 30.0 and b.captures, "a benchmark run")
    var a := parse(["--pack", "p", "--cell", "C", "--activate", "0x303,0x213,7"])
    var wanted: Array[int] = [0x303, 0x213, 7]
    expect(a.has_activate and a.activate == wanted, "activations: %s" % [a.activate])
    expect(not a.interactive and not a.captures, "activating ignores input, but is no capture")
    expect(parse(["--pack", "p", "--cell", "C", "--no-input", "x"]).interactive == false, "--no-input")
    expect(not parse(["--pack", "p", "--cell", "C", "--activate"]).has_activate,
            "a key without a value is skipped")

func _errors() -> void:
    expect(parse([]).error.begins_with("usage:"), "no arguments: usage")
    expect(parse(["--pack", "p"]).error.begins_with("usage:"), "a pack alone: usage")
    expect(parse(["--cell", "C"]).error.begins_with("usage:"), "a cell without a pack: usage")
    expect(parse(["--pack", "p", "--cell", "C", "--at", "1,2"]).error.contains("--at wants X,Y,Z"),
            "a short --at")
    expect(parse(["--pack", "p", "--world", "W", "--target", "x"]).error.contains("--target wants"),
            "a bad --target")
    expect(parse(["--from-shot", "/does/not/exist.json"]).error.begins_with("cannot read the shot"),
            "a missing shot")

func _from_shot() -> void:
    var scratch := OS.get_environment("SKYDOT_SCRATCH")
    if scratch == "":
        scratch = OS.get_user_data_dir().path_join("smoke")
    DirAccess.make_dir_recursive_absolute(scratch)
    var path := scratch.path_join("viewer_settings_shot.json")
    var file := FileAccess.open(path, FileAccess.WRITE)
    file.store_string(JSON.stringify({
        "format": 1, "note": "too dark",
        "place": {"kind": "exterior", "world": "Tamriel"},
        "camera": {"game": {"x": 1, "y": 2, "z": 3, "heading": 90, "tilt": 10}},
        "time": {"hour": 9.5, "day": 3}, "weather": {"editor_id": "SkyrimClear"},
        "viewer": {"flying": true, "radius": 4, "lod_split": 2.0, "msaa": "4", "quests": false,
            "materials": false},
        "pack": {"path": "/some/pack"}}))
    file.close()
    var s := parse(["--from-shot", path, "--radius", "1"])
    expect(s.error == "", "a shot reads: " + s.error)
    expect(s.shot_note == "too dark", "the note is kept to print")
    expect(s.world == "Tamriel" and s.cell == "" and s.pack == "/some/pack", "place and pack")
    expect(s.has_at and s.at == Vector3(1, 2, 3) and s.has_look, "camera")
    expect(s.time == 9.5 and s.time_scale == 0.0, "the shot's time, stopped")
    expect(s.weather == "SkyrimClear" and s.fly and not s.quests and not s.materials, "options of the shot")
    expect(s.radius == 1, "the command line wins over the shot: %d" % s.radius)
    expect(s.lod_split == 2.0 and s.msaa_index == 2, "the rest of the shot's options")
    expect(parse(["--from-shot", path, "--pack", "mine"]).pack == "mine", "--pack wins over the shot's")
    file = FileAccess.open(path, FileAccess.WRITE)
    file.store_string(JSON.stringify({"format": 99}))
    file.close()
    expect(parse(["--from-shot", path]).error.begins_with("cannot read the shot"), "another format is refused")

func _clock() -> void:
    var clock := SkydotClock.new()
    clock.setup(9.5, 20.0, false)
    expect(clock.hour == 9.5 and clock.day == 0 and not clock.has_weather(), "kept time before any place")
    expect(clock.shift(1.0) == "", "nothing keeps the time without a weather or an AI")
    var state := clock.describe()
    expect(state["hour"] == 9.5 and state["day"] == 0 and not state.has("time_scale"),
            "inside, a shot gets hour and day only: %s" % [state])
