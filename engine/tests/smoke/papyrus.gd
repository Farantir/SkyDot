# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the test pack's scripts (converter/tools/testpack/scripts.hpp, where each
# function's expected result is noted) through SkydotPapyrus: globals covering
# the opcodes, a latent wait, states, and the lever attached to its reference
# with its VMAD property, toggling the cube it names.
extends SceneTree

var failures := 0
var traces: Array[String] = []
var enabled_changes: Array = []
var animations: Array = []

func _init() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_papyrus: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_papyrus: SKIP (SKYDOT_TESTPACK unset)")
        quit(77)
        return
    _run(pack_dir)
    print("smoke_papyrus: failures=", failures)
    quit(failures)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

## Run a started thread to completion (at most `seconds` of VM time).
func finish(vm: SkydotPapyrus, thread: int, seconds := 0.0) -> Variant:
    vm.update(seconds)
    var result := vm.take_result(thread)
    expect(result["done"], "thread %d finished" % thread)
    return result.get("value")

func _run(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    var world := pack.open_world()
    if world == null:
        expect(false, "world.fb opens")
        return
    var vm := SkydotPapyrus.new()
    expect(vm.setup(pack, world) == OK, "the VM sets up")
    vm.trace.connect(func(text: String) -> void: traces.append(text))
    vm.error.connect(func(text: String) -> void: print("vm error: ", text))  # shows up in ctest -V
    vm.enable_changed.connect(func(ref: int, enabled: bool) -> void: enabled_changes.append([ref, enabled]))
    vm.play_animation.connect(func(ref: int, anim: String) -> void: animations.append([ref, anim]))

    # Globals: recursion, float arithmetic and casts, arrays, loops and strings.
    expect(finish(vm, vm.call_global("TestpackVmScript", "Fib", [10])) == 55, "Fib(10) is 55")
    expect(is_equal_approx(finish(vm, vm.call_global("TestpackVmScript", "Mix", [5, 3.0])), 6.5),
           "Mix(5, 3.0) is 6.5")
    expect(finish(vm, vm.call_global("testpackvmscript", "join", [2])) == "ab1",
           "Join(2) is ab1, and names are case-insensitive")
    expect(finish(vm, vm.call_global("TestpackVmScript", "Join", [5])) == "abc1", "Join(5) is abc1")

    # A latent native suspends the thread until the wait is over.
    var waits := vm.call_global("TestpackVmScript", "Waits", [])
    vm.update(0.0)
    expect(not vm.take_result(waits)["done"], "Wait(0.5) suspends")
    vm.update(0.4)
    expect(not vm.take_result(waits)["done"], "still waiting after 0.4 s")
    expect(finish(vm, waits, 0.2) == 42, "and returns 42 once 0.5 s have passed")

    # States: the auto state, GotoState through the compiled function, and
    # OnBeginState (declared with different case) running on the way.
    var stateful := vm.create_instance("TestpackStateScript")
    expect(stateful != 0, "an instance of the state script")
    expect(finish(vm, vm.call_method(stateful, "TestpackStateScript", "Ping", [])) == 1,
           "the auto state Idle answers 1")
    finish(vm, vm.call_method(stateful, "TestpackStateScript", "GoBusy", []))
    expect(finish(vm, vm.call_method(stateful, "TestpackStateScript", "Ping", [])) == 2,
           "after GotoState(Busy) it answers 2")
    expect(finish(vm, vm.call_method(stateful, "TestpackStateScript", "GetState", [])) == "Busy",
           "GetState says Busy")
    expect(traces.has("busy"), "OnBeginState ran: %s" % [traces])

    # The lever in the interior: its base script with the reference's Target.
    var interior := world.find_cell("TestpackInterior")
    expect(vm.attach(interior, 0x304) == 1, "the lever gets one script")
    expect(vm.attach(interior, 0x304) == 0, "and only once")
    expect(vm.get_variable(0x304, "TestpackLeverScript", "::Target_var") == 0x301,
           "the reference's VMAD sets Target to the cube")
    expect(not vm.is_ref_disabled(0x301), "the cube starts enabled")
    traces.clear()
    expect(vm.activate(0x304) == 1, "activating the lever runs OnActivate")
    vm.update(0.0)
    expect(traces == ["lever pulled 1"], "it traces: %s" % [traces])
    expect(enabled_changes == [[0x301, false]], "it disables the cube: %s" % [enabled_changes])
    expect(vm.is_ref_disabled(0x301), "which stays disabled")
    expect(animations == [[0x304, "Open"]], "and plays Open on itself: %s" % [animations])
    vm.activate(0x304)
    vm.update(0.0)
    expect(enabled_changes.back() == [0x301, true], "a second pull enables it again")
    expect(vm.get_variable(0x304, "TestpackLeverScript", "Pulls") == 2, "Pulls counts to 2")

    # Physics natives leave the bodies to the game layer.
    var physics := []
    vm.havok_impulse.connect(func(ref: int, direction: Vector3, magnitude: float) -> void:
        physics.append([ref, direction, magnitude]))
    vm.motion_type_changed.connect(func(ref: int, motion_type: int) -> void:
        physics.append([ref, motion_type]))
    finish(vm, vm.call_method(0x304, "TestpackLeverScript", "ApplyHavokImpulse", [0.0, 0.0, 1.0, 50.0]))
    finish(vm, vm.call_method(0x304, "TestpackLeverScript", "SetMotionType", [1, true]))
    expect(physics == [[0x304, Vector3(0, 0, 1), 50.0], [0x304, 1]],
           "impulses and motion types reach the game layer: %s" % [physics])
    expect(vm.get_error_count() == 0, "nothing went wrong so far")

    _check_triggers(vm, world)
    _check_timers_and_natives(vm)
    _check_save(vm, pack, world)

    # Missing classes and functions are reported, not fatal.
    expect(vm.call_global("NoSuchScript", "Run", []) == 0, "an unknown class starts nothing")
    expect(vm.get_error_count() == 1, "and is logged once: %d" % vm.get_error_count())
    print("smoke_papyrus: %d instructions, %.1f s of VM time" % [vm.get_instruction_count(), vm.get_time()])

## The trigger box is 128 units wide, 300 units south of the origin, turned
## 45 degrees: a point 80 units east of its centre is inside only if the turn
## is applied.
func _check_triggers(vm: SkydotPapyrus, world: SkydotWorld) -> void:
    var interior := world.find_cell("TestpackInterior")
    expect(vm.attach_cell(interior) == 1, "attach_cell finds the model-less trigger")
    var triggers := vm.get_triggers()
    expect(triggers.size() == 1 and triggers[0]["ref"] == 0x305, "it is registered: %s" % [triggers])
    traces.clear()
    vm.update_actor(SkydotPapyrus.PLAYER_REF, Vector3(0, 0, 0))
    vm.update(0.0)
    expect(traces.is_empty(), "far away nothing happens")
    vm.update_actor(SkydotPapyrus.PLAYER_REF, Vector3(80, -300, 10))
    vm.update(0.0)
    expect(traces == ["inside 1"], "entering sends OnTriggerEnter, with the count: %s" % [traces])
    expect(vm.get_trigger_count(0x305) == 1, "one actor inside")
    vm.update_actor(SkydotPapyrus.PLAYER_REF, Vector3(85, -300, 10))
    vm.update(0.0)
    expect(traces.size() == 1, "moving inside sends nothing more")
    vm.update_actor(SkydotPapyrus.PLAYER_REF, Vector3(0, -150, 0))
    vm.update(0.0)
    expect(traces == ["inside 1", "left"], "leaving sends OnTriggerLeave: %s" % [traces])
    expect(vm.get_variable(0x305, "TestpackTriggerScript", "Entered") == 1, "Entered counted once")

func _check_timers_and_natives(vm: SkydotPapyrus) -> void:
    var timer := vm.create_instance("TestpackTimerScript")
    finish(vm, vm.call_method(timer, "TestpackTimerScript", "Start", []))
    for i in 10:
        vm.update(0.25)
    expect(vm.get_variable(timer, "TestpackTimerScript", "Ticks") == 3,
           "OnUpdate every 0.5 s until it unregisters after three: %s"
               % vm.get_variable(timer, "TestpackTimerScript", "Ticks"))

    var rolls := {}
    for i in 60:
        var roll: int = finish(vm, vm.call_method(timer, "TestpackTimerScript", "Roll", []))
        rolls[roll] = true
    var keys := rolls.keys()
    keys.sort()
    expect(keys == [1, 2, 3, 4, 5, 6], "RandomInt(1, 6) gives each of 1 to 6: %s" % [keys])

    animations.clear()
    var animate := vm.call_method(timer, "TestpackTimerScript", "Animate", [])
    vm.update(0.0)
    expect(animations == [[timer, "Open"]], "PlayAnimationAndWait plays: %s" % [animations])
    vm.update(1.0)
    expect(not vm.take_result(animate)["done"], "and waits for its event")
    vm.notify_animation_event(timer, "done")
    expect(finish(vm, animate) == true, "which ends the wait, in any case")
    var timeout := vm.call_method(timer, "TestpackTimerScript", "Animate", [])
    vm.update(1.0)
    expect(not vm.take_result(timeout)["done"], "without the event it waits")
    expect(finish(vm, timeout, 10.0) == true, "until the timeout")

    expect(vm.get_lock_level(timer) == -1 and not vm.is_activation_blocked(timer), "unlocked, unblocked")
    finish(vm, vm.call_method(timer, "TestpackTimerScript", "Guard", []))
    expect(vm.is_activation_blocked(timer), "BlockActivation blocks")
    expect(vm.get_lock_level(timer) == 0, "Lock locks: %d" % vm.get_lock_level(timer))

## A save taken while a thread waits restores into a fresh VM and finishes
## there; variables, states and the world view come along.
func _check_save(vm: SkydotPapyrus, pack: SkydotPack, world: SkydotWorld) -> void:
    var stateful := vm.create_instance("TestpackStateScript")
    finish(vm, vm.call_method(stateful, "TestpackStateScript", "GoBusy", []))
    var waits := vm.call_global("TestpackVmScript", "Waits", [])
    vm.update(0.2)
    var bytes := vm.save_state()
    expect(bytes.size() > 0, "a save")

    var fresh := SkydotPapyrus.new()
    fresh.setup(pack, world)
    expect(fresh.load_state(bytes) == OK, "loads: " + fresh.get_last_error())
    expect(not fresh.take_result(waits)["done"], "the waiting thread is still waiting")
    # Wait ran in the update before the save, so all 0.5 s of it are left.
    fresh.update(0.45)
    expect(not fresh.take_result(waits)["done"], "0.45 s later it still waits")
    expect(finish(fresh, waits, 0.1) == 42, "and finishes once the 0.5 s are over")
    expect(fresh.get_variable(0x304, "TestpackLeverScript", "Pulls") == 2, "the lever's count survives")
    expect(finish(fresh, fresh.call_method(stateful, "TestpackStateScript", "Ping", [])) == 2,
           "the state script is still Busy")
    expect(not fresh.is_ref_disabled(0x301) and fresh.get_disabled_changes().has(0x301),
           "script-made enable changes survive")
    expect(fresh.get_trigger_count(0x305) == 0 and fresh.get_triggers().size() == 1,
           "the trigger is registered again")
    expect(fresh.attach(world.find_cell("TestpackInterior"), 0x304) == 0,
           "attached references are not attached twice")
    expect(fresh.save_state() == fresh.save_state(), "saves are reproducible")

    var broken := bytes.slice(0, bytes.size() / 2)
    var other := SkydotPapyrus.new()
    other.setup(pack, world)
    expect(other.load_state(broken) != OK, "a truncated save is refused")
    expect(other.load_state(PackedByteArray([1, 2, 3])) != OK, "and junk")
