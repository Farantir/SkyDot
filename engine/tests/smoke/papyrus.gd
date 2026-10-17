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
    # Weapon.Fire reaches the game layer with the weapon, the source and the ammo
    # (None here: scripts cannot be handed references from outside).
    var fired := []
    vm.fire_weapon.connect(func(weapon: int, source: int, ammo: int) -> void:
        fired.append([weapon, source, ammo]))
    var weapon := vm.create_instance("TestpackFireScript")
    finish(vm, vm.call_method(weapon, "TestpackFireScript", "Shoot", []))
    expect(fired == [[weapon, 0, 0]], "Weapon.Fire emits fire_weapon: %s" % [fired])
    # Sound.Play hands out instance ids and emits play_sound; PlayAndWait waits
    # for notify_sound_finished.
    var played := []
    vm.play_sound.connect(func(sound: int, source: int, instance: int) -> void:
        played.append([sound, source, instance]))
    var clicker := vm.create_instance("TestpackSoundScript")
    finish(vm, vm.call_method(clicker, "TestpackSoundScript", "Click", []))
    finish(vm, vm.call_method(clicker, "TestpackSoundScript", "Click", []))
    expect(played == [[clicker, 0, 1], [clicker, 0, 2]], "Sound.Play emits play_sound with instances: %s" % [played])
    expect(vm.get_error_count() == 0, "nothing went wrong so far")

    _check_triggers(vm, world)
    _check_detach(vm, world)
    _check_timers_and_natives(vm)
    _check_items(vm, pack, world)
    _check_save(vm, pack, world)
    _check_activation(vm)
    _check_loaded_state(vm, world, interior)

    # Missing classes and functions are reported, not fatal.
    expect(vm.call_global("NoSuchScript", "Run", []) == 0, "an unknown class starts nothing")
    expect(vm.get_error_count() == 1, "and is logged once: %d" % vm.get_error_count())
    print("smoke_papyrus: %d instructions, %.1f s of VM time" % [vm.get_instruction_count(), vm.get_time()])

## activate_ref: default_only, locks by source, and the sources' difference.
func _check_activation(vm: SkydotPapyrus) -> void:
    var player := SkydotPapyrus.PLAYER_REF
    var pulls_before: int = vm.get_variable(0x304, "TestpackLeverScript", "Pulls")
    var outcome := vm.activate_ref(0x304, player, true, SkydotPapyrus.ACTIVATE_PLAYER, false)
    expect(outcome["scripts"] == 0, "default processing only skips OnActivate")
    vm.update(0.0)
    expect(vm.get_variable(0x304, "TestpackLeverScript", "Pulls") == pulls_before, "so the lever is not pulled")
    outcome = vm.activate_ref(0x304, player, false, SkydotPapyrus.ACTIVATE_PLAYER, false)
    expect(outcome["scripts"] == 1, "and the full activation runs it")
    vm.update(0.0)
    expect(vm.get_variable(0x304, "TestpackLeverScript", "Pulls") == pulls_before + 1, "once")
    expect(vm.activate_ref(0x9999999, player, false, 0, false)["kind"] == "missing", "an unknown reference is missing")
    # Not furniture with a graph: nothing waits, and completing nothing is harmless.
    expect(not outcome.get("deferred", false), "an activator is not deferred")
    vm.complete_activation(0x304)
    vm.update(0.0)
    expect(vm.get_variable(0x304, "TestpackLeverScript", "Pulls") == pulls_before + 1, "nothing was pending")

    # 0x213 is locked (level 25); its partner 0x303 shares the lock.
    expect(vm.get_lock_level(0x213) == 25, "the outside door starts locked")
    outcome = vm.activate_ref(0x213, player, false, SkydotPapyrus.ACTIVATE_PLAYER, false)
    expect(outcome["kind"] == "locked" and "Apprentice" in outcome["note"], "the player is refused: %s" % [outcome])
    outcome = vm.activate_ref(0x213, player, false, SkydotPapyrus.ACTIVATE_SCRIPT, false)
    expect(outcome["kind"] == "door" and outcome["door"]["destination"] == 0x303, "a script goes through")
    outcome = vm.activate_ref(0x213, player, false, SkydotPapyrus.ACTIVATE_PARENT, true)
    expect(outcome["kind"] == "door", "so does an activate parent, even with the pick flag")
    expect(vm.get_lock_level(0x213) == 25 and vm.get_lock_level(0x303) == 25, "and neither unlocks it")
    outcome = vm.activate_ref(0x213, player, false, SkydotPapyrus.ACTIVATE_PLAYER, true)
    expect(outcome["kind"] == "door" and vm.get_lock_level(0x213) == -1, "the player's pick does")
    vm.set_locked(0x213, true)

## Is3DLoaded follows the node registry: built, then gone when the node is freed
## (or leaves the tree).
func _check_loaded_state(vm: SkydotPapyrus, world: SkydotWorld, interior: int) -> void:
    expect(world.find_ref_node(0x304) == null and not vm.is_3d_loaded(0x304), "nothing is built yet")
    var holder := world.build_ref(interior, 0x304)
    expect(holder != null, "the lever builds")
    if holder == null:
        return
    var node := world.find_ref_node(0x304)
    expect(node != null and holder.is_ancestor_of(node) and vm.is_3d_loaded(0x304), "its model is registered and loaded")
    # tree_exiting removes it (a scene tree is not running here to leave).
    expect(node.get_signal_connection_list("tree_exiting").size() == 1, "its exit from the tree is watched")
    holder.free()
    holder = world.build_ref(interior, 0x304)
    expect(vm.is_3d_loaded(0x304), "built again, it is loaded")
    holder.free()
    expect(world.find_ref_node(0x304) == null and not vm.is_3d_loaded(0x304), "a freed model is gone")

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

## A cell going away unloads its scripts: OnUnload, no more updates, and the
## cell is no longer attached; coming back sends OnLoad again, not OnInit.
func _check_detach(vm: SkydotPapyrus, world: SkydotWorld) -> void:
    var interior := world.find_cell("TestpackInterior")
    var script := "TestpackTriggerScript"
    expect(vm.attach_cell(interior) == 0, "attached again, nothing new gets OnInit")
    vm.update(1.1)
    var updates: int = vm.get_variable(0x305, script, "Updates")
    expect(updates >= 1, "the trigger's update timer runs while loaded: %d" % updates)
    expect(vm.get_variable(0x305, script, "Unloads") == 0, "not unloaded yet")
    var unloaded: int = vm.detach_cell(interior)
    expect(unloaded >= 1, "detach_cell unloads the scripted references: %d" % unloaded)
    vm.update(0.0)
    expect(vm.get_variable(0x305, script, "Unloads") == 1, "OnUnload was sent")
    vm.update(2.0)
    expect(vm.get_variable(0x305, script, "Updates") == updates, "its updates stopped")
    expect(vm.detach_cell(interior) == 0 and vm.detach_all() == 0, "nothing is unloaded twice")
    var loads: int = vm.get_variable(0x305, script, "Loads")
    expect(vm.attach_cell(interior) == 0, "attaching again sends no OnInit")
    vm.update(0.0)
    expect(vm.get_variable(0x305, script, "Loads") == loads, "attach_cell alone sends no OnLoad")
    vm.update(1.0)
    expect(vm.get_variable(0x305, script, "Updates") == updates, "no timer came back with it")
    expect(vm.detach_all() >= 1, "detach_all unloads what is loaded")
    vm.update(0.0)
    expect(vm.get_variable(0x305, script, "Unloads") == 2, "and sent OnUnload again")
    expect(vm.get_moved_changes().is_empty(), "no script moved a reference")

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

    # A handler that reports the event synchronously must still wake the wait.
    var sync_handler := func(ref: int, _anim: String) -> void: vm.notify_animation_event(ref, "done")
    vm.play_animation.connect(sync_handler)
    var quick := vm.call_method(timer, "TestpackTimerScript", "Animate", [])
    vm.update(0.0)
    vm.update(0.1)
    expect(vm.take_result(quick)["done"], "an event raised while the animation starts still ends the wait")
    vm.play_animation.disconnect(sync_handler)

    expect(vm.get_lock_level(timer) == -1 and not vm.is_activation_blocked(timer), "unlocked, unblocked")
    finish(vm, vm.call_method(timer, "TestpackTimerScript", "Guard", []))
    expect(vm.is_activation_blocked(timer), "BlockActivation blocks")
    expect(vm.get_lock_level(timer) == 0, "Lock locks: %d" % vm.get_lock_level(timer))

const SWORD := 0x190
const CUIRASS := 0x191
const GOLD := 0x192
const CHEST := 0x214
const PLAYER := 0x14

func _item(vm: SkydotPapyrus, fn: String, args: Array) -> Variant:
    return finish(vm, vm.call_global("TestpackItemScript", fn, args))

## The test pack's chest holds 25 gold and one of a leveled list's sword or
## cuirass; items move, equip and survive a save, through the API and the
## natives.
func _check_items(vm: SkydotPapyrus, pack: SkydotPack, world: SkydotWorld) -> void:
    var inventory := vm.inventory(CHEST)
    expect(inventory.size() == 2, "the chest resolves to gold and one more: %s" % [inventory])
    var gold: Dictionary = inventory[0] if inventory[0]["form"] == GOLD else inventory[1]
    var other: Dictionary = inventory[1] if inventory[0]["form"] == GOLD else inventory[0]
    expect(gold["form"] == GOLD and gold["count"] == 25 and gold["name"] == "Testpack Gold", "25 gold")
    expect(gold["category"] == "misc" and gold["value"] == 1 and gold["playable"], "gold is a playable misc item")
    expect(other["form"] in [SWORD, CUIRASS] and other["count"] == 1, "one of the list's items")
    expect(vm.inventory(CHEST) == inventory, "and the same again")
    expect(_item(vm, "Count", [CHEST, GOLD]) == 25, "GetItemCount sees the gold")

    var sword := vm.item_info(SWORD)
    expect(sword["found"] and sword["category"] == "weapon" and sword["weapon_type"] == 1, "the sword: %s" % [sword])
    expect(sword["equip_slot"] == 0x13F42 and sword["damage"] == 7.0 and sword["value"] == 25, "its slot, damage, value")
    var cuirass := vm.item_info(CUIRASS)
    expect(cuirass["category"] == "armor" and cuirass["armor_type"] == 1 and cuirass["biped_slots"] == 4, "the cuirass")
    expect(is_equal_approx(cuirass["armor_rating"], 25.0), "its rating is DNAM / 100")
    expect(vm.item_info(CHEST).is_empty(), "a chest is no item")

    # Moving: gold, then everything.
    expect(vm.transfer(CHEST, PLAYER, GOLD, 100) == 25, "all 25 gold move")
    expect(vm.transfer(CHEST, PLAYER, GOLD, 1) == 0, "no more")
    expect(vm.transfer(CHEST, PLAYER, other["form"], 1) == 1, "the other item moves")
    expect(vm.inventory(CHEST).is_empty(), "the chest is empty and stays so (it is resolved)")
    expect(_item(vm, "Count", [PLAYER, GOLD]) == 25, "the player has the gold")

    # Equipping.
    vm.add_item(PLAYER, SWORD, 1)
    vm.add_item(PLAYER, CUIRASS, 1)
    expect(vm.equip(PLAYER, SWORD) and vm.equip(PLAYER, CUIRASS), "sword and cuirass equip")
    expect(not vm.equip(PLAYER, GOLD), "gold does not")
    expect(_item(vm, "Wearing", [PLAYER, SWORD]) and _item(vm, "Wearing", [PLAYER, CUIRASS]), "IsEquipped")
    expect(_item(vm, "HandType", [PLAYER, 1]) == 1 and _item(vm, "HandType", [PLAYER, 0]) == 0, "a sword in the right hand")
    expect(_item(vm, "WeaponId", [PLAYER, false]) == SWORD and _item(vm, "WeaponId", [PLAYER, true]) == 0, "GetEquippedWeapon")
    expect(_item(vm, "ShieldId", [PLAYER]) == 0, "no shield")
    var worn := vm.equipment(PLAYER)
    expect(worn["right"] == SWORD and worn["left"] == 0 and worn["worn"] == PackedInt64Array([CUIRASS]), "equipment: %s" % [worn])
    var rows := vm.inventory(PLAYER).filter(func(row: Dictionary) -> bool: return row["equipped"])
    expect(rows.size() == 2, "the inventory marks both")

    # Saved with the game.
    var bytes := vm.save_state()
    var fresh := SkydotPapyrus.new()
    fresh.setup(pack, world)
    expect(fresh.load_state(bytes) == OK, "loads: " + fresh.get_last_error())
    expect(fresh.inventory(CHEST).is_empty(), "the chest is still empty after a load")
    expect(fresh.equipment(PLAYER) == worn, "the equipment comes back")
    expect(fresh.get_item_count(PLAYER, GOLD) == 25, "and the gold")

    # Removing the last one takes it off; EquipItem adds what is missing.
    _item(vm, "Take", [PLAYER, SWORD, 1])
    expect(vm.equipment(PLAYER)["right"] == 0 and _item(vm, "Count", [PLAYER, SWORD]) == 0, "the sword is gone and off")
    _item(vm, "Equip", [PLAYER, SWORD])
    expect(_item(vm, "Count", [PLAYER, SWORD]) == 1 and _item(vm, "Wearing", [PLAYER, SWORD]), "EquipItem adds the sword")
    _item(vm, "Unequip", [PLAYER, SWORD])
    expect(not _item(vm, "Wearing", [PLAYER, SWORD]) and _item(vm, "Count", [PLAYER, SWORD]) == 1, "UnequipItem keeps it")
    _item(vm, "Equip", [PLAYER, SWORD])
    _item(vm, "Strip", [PLAYER])
    expect(vm.equipment(PLAYER)["worn"].is_empty() and vm.equipment(PLAYER)["right"] == 0, "UnequipAll")

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
