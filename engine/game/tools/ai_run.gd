# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs actors' AI packages in a built place without a window: builds an
# interior (or 3x3 exterior cells around a game position) with collision and
# navmeshes at a time of day, lets the clock run, and prints who arrives,
# who leaves through which door, and every few seconds what each actor is
# doing and where it is.
#
#   godot4.7 --headless --path game --script res://tools/ai_run.gd -- \
#       --pack <pack> --cell RiverwoodAlvorsHouse --time 7.9 [--time-scale 60] \
#       [--seconds 60] [--every 5]
#   godot4.7 --headless --path game --script res://tools/ai_run.gd -- \
#       --pack <pack> --world Tamriel --at 20586,-45138 --time 7.9
extends SceneTree

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _ai: SkydotAi
var _root: Node3D
var _cells := {}  # Vector2i -> Node3D, outside
var _space := 0
var _elapsed := 0.0
var _seconds := 60.0
var _every := 5.0
var _next := 0.0
var _names := {}

func _initialize() -> void:
    var argv := OS.get_cmdline_user_args()
    var args := {}
    for i in range(0, argv.size() - 1):
        args[argv[i]] = argv[i + 1]
    _seconds = float(args.get("--seconds", "60"))
    _every = float(args.get("--every", "5"))
    var pack := SkydotPack.new()
    if pack.open(args.get("--pack", "")) != OK:
        printerr("ai_run: ", pack.get_error())
        quit(1)
        return
    _world = pack.open_world()
    _papyrus = SkydotPapyrus.new()
    _papyrus.setup(pack, _world)
    _papyrus.start_game_enabled_quests()
    _papyrus.lock_changed.connect(func(ref: int, locked: bool) -> void:
        print("%6.1fs %s door 0x%08X" % [_elapsed, "locks" if locked else "unlocks", ref]))
    _ai = SkydotAi.new()
    _ai.setup(_world, _papyrus)
    _ai.days = float(args.get("--day", "0")) + float(args.get("--time", "12")) / 24.0
    _ai.time_scale = float(args.get("--time-scale", "60"))
    _ai.actor_left.connect(func(ref: int, door: int) -> void:
        print("%6.1fs %s leaves through 0x%08X" % [_elapsed, _name(ref), door]))
    _ai.actor_arrived.connect(_arrived)
    _root = Node3D.new()
    get_root().add_child(_root)
    if args.has("--world"):
        _space = _world.find_world(args["--world"])
        _ai.set_space(_space)
        var started := Time.get_ticks_usec()
        print("placed: ", _ai.place_actors(), " in %.1f ms" % ((Time.get_ticks_usec() - started) / 1000.0))
        var at: PackedStringArray = String(args.get("--at", "0,0")).split(",")
        var gx := floori(float(at[0]) / SkydotWorld.CELL_UNITS)
        var gy := floori(float(at[1]) / SkydotWorld.CELL_UNITS)
        for dx in range(-1, 2):
            for dy in range(-1, 2):
                var cell := _world.build_exterior(_space, gx + dx, gy + dy)
                if cell != null:
                    _cells[Vector2i(gx + dx, gy + dy)] = cell
                    _root.add_child(cell)
    else:
        _space = _world.find_cell(args.get("--cell", "RiverwoodSleepingGiantInn"))
        if _space == 0:
            printerr("ai_run: no such cell")
            quit(1)
            return
        _ai.set_space(_space)
        print("placed: ", _ai.place_actors())
        var cell := _world.build_cell(_space)
        _cells[Vector2i.ZERO] = cell
        _root.add_child(cell)
    print("attached: ", _ai.attach_built(_root), " at %05.2f" % _ai.get_hour())
    _report()
    physics_frame.connect(_tick)

func _name(ref: int) -> String:
    if not _names.has(ref):
        var plan := _world.get_actor_plan(ref)
        var npc := _world.get_actor(ref)
        _names[ref] = "0x%08X" % ref
        for actor in _root.find_children("*", "SkydotActor", true, false):
            if actor.get_meta("skydot_ref", 0) == ref:
                _names[ref] = String(actor.name)
        if _names[ref].begins_with("0x") and not plan.is_empty():
            _names[ref] = "0x%08X npc 0x%08X" % [ref, npc.get("base", 0)]
    return _names[ref]

func _arrived(ref: int) -> void:
    var place := _world.get_actor_place(ref)
    var parent: Node = null
    if _world.get_cell(_space).get("interior", false):
        parent = _cells.get(Vector2i.ZERO)
    else:
        var p: Vector3 = place["position"]
        parent = _cells.get(Vector2i(floori(p.x / SkydotWorld.CELL_UNITS),
                floori(p.y / SkydotWorld.CELL_UNITS)))
    if parent == null:
        print("%6.1fs 0x%08X arrives where nothing is built" % [_elapsed, ref])
        return
    var node := _world.build_actor(ref)
    if node == null:
        return
    parent.add_child(node)
    _ai.attach_built(node)
    print("%6.1fs %s arrives at %s" % [_elapsed, _name(ref), place["position"]])

func _report() -> void:
    print("-- %05.2f" % _ai.get_hour())
    for actor in _root.find_children("*", "SkydotActor", true, false):
        var ref: int = actor.get_meta("skydot_ref", 0)
        var state := _ai.get_actor_state(ref)
        var at := SkydotWorld.godot_to_skyrim(actor.global_position)
        var target := ""
        if state.has("target"):
            var t: Vector3 = state["target"]["position"]
            target = "-> %s (%d, %d, %d)%s" % ["here" if state["target"]["space"] == _space else "0x%08X" % state["target"]["space"],
                t.x, t.y, t.z, " via door 0x%08X" % state["door"] if state.get("door", 0) != 0 else ""]
        print("  %-28s %-34s %-14s %-5s (%d, %d, %d) %s" % [String(actor.name).left(28), state.get("package_editor_id", "-"),
            state.get("procedure", "-"), actor.get_state(), at.x, at.y, at.z, target])

func _tick() -> void:
    var dt := 1.0 / Engine.physics_ticks_per_second
    _elapsed += dt
    _ai.update(dt)
    _papyrus.update(dt)
    if _elapsed >= _next:
        _next += _every
        _report()
    if _elapsed >= _seconds:
        quit(0)
