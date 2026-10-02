# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds a cell with collision and navmesh, lets its actors wander for a
# while and prints what each did: state over time, how far it went from
# home, whether it ever fell or stuck. Interior by editor id, or an exterior
# around a game position (`--world Tamriel --at x,y`, 3x3 cells).
#
#   godot4.7 --headless --path game --script res://tools/actor_walk.gd -- \
#       --pack <pack> --cell RiverwoodSleepingGiantInn [--seconds 40] [--trace on]
extends SceneTree

var _world: SkydotWorld
var _root: Node3D
var _actors: Array = []
var _elapsed := 0.0
var _seconds := 40.0
var _samples := {}
var _trace := false
var _next_trace := 0.0

func _initialize() -> void:
    var argv := OS.get_cmdline_user_args()
    var args := {}
    for i in range(0, argv.size() - 1):
        args[argv[i]] = argv[i + 1]
    _seconds = float(args.get("--seconds", "40"))
    _trace = args.get("--trace", "off") != "off"
    var pack := SkydotPack.new()
    if pack.open(args.get("--pack", "")) != OK:
        printerr("actor_walk: ", pack.get_error())
        quit(1)
        return
    _world = pack.open_world()
    if args.has("--world"):
        var ws: int = _world.find_world(args["--world"])
        var at: PackedStringArray = String(args.get("--at", "0,0")).split(",")
        var gx := floori(float(at[0]) / 4096.0)
        var gy := floori(float(at[1]) / 4096.0)
        _root = Node3D.new()
        for dx in range(-1, 2):
            for dy in range(-1, 2):
                var cell := _world.build_exterior(ws, gx + dx, gy + dy)
                if cell != null:
                    _root.add_child(cell)
    else:
        var cell: int = _world.find_cell(args.get("--cell", "RiverwoodSleepingGiantInn"))
        if cell == 0:
            printerr("actor_walk: no such cell")
            quit(1)
            return
        _root = _world.build_cell(cell)
    get_root().add_child(_root)
    _collect(_root)
    print("actors: ", _actors.size())
    physics_frame.connect(_tick)

func _collect(node: Node) -> void:
    if node is SkydotActor:
        _actors.append(node)
    for child in node.get_children():
        _collect(child)

func _tick() -> void:
    var dt := 1.0 / Engine.physics_ticks_per_second
    _elapsed += dt
    for a: SkydotActor in _actors:
        if not is_instance_valid(a):
            continue
        var s: Dictionary = _samples.get(a, {"states": {}, "far": 0.0, "walked": 0.0, "last": a.global_position, "walks": 0, "prev": ""})
        var state: String = a.get_state()
        s["states"][state] = s["states"].get(state, 0.0) + dt
        if state != s["prev"] and (state == "walk" or state == "run"):
            s["walks"] += 1
        s["prev"] = state
        var p: Vector3 = a.global_position
        s["walked"] += Vector2(p.x - s["last"].x, p.z - s["last"].z).length()
        s["last"] = p
        s["far"] = max(s["far"], Vector2(p.x - a.home.x, p.z - a.home.z).length())
        s["drop"] = min(s.get("drop", 0.0), p.y - a.home.y)
        _samples[a] = s
    if _trace and _elapsed >= _next_trace:
        _next_trace += 1.0
        for a: SkydotActor in _actors:
            var anim: AnimationPlayer = a.get_node_or_null("AnimationPlayer")
            var g := SkydotWorld.godot_to_skyrim(a.global_position)
            print("%5.1f %-12s %-5s at %6.0f,%6.0f,%5.0f  v %.2f  anim %s x%.2f @%.2f" % [_elapsed, a.name.substr(11, 12), a.get_state(),
                g.x, g.y, g.z, Vector2(a.velocity.x, a.velocity.z).length(),
                anim.current_animation if anim else "-", anim.speed_scale if anim else 0.0,
                anim.current_animation_position if anim and anim.is_playing() else -1.0])
    if _elapsed >= _seconds:
        _report()
        quit(0)

func _report() -> void:
    var walkers := 0
    for a: SkydotActor in _actors:
        var s: Dictionary = _samples.get(a, {})
        if s.is_empty():
            continue
        var states := []
        for k in s["states"]:
            states.append("%s %.0fs" % [k, s["states"][k]])
        var anim: AnimationPlayer = a.get_node_or_null("AnimationPlayer")
        var clips := "" if anim == null else ",".join(anim.get_animation_list())
        print("%-40s %-28s walks %d, walked %.1f m, farthest %.1f m from home, lowest %.2f m, r %.2f h %.2f [%s]" % [
            a.name.substr(0, 40), " ".join(states), s["walks"], s["walked"], s["far"], s.get("drop", 0.0),
            a.radius, a.height, clips])
        if s["walks"] > 0:
            walkers += 1
    print("%d of %d actors walked in %.0f s" % [walkers, _actors.size(), _seconds])
