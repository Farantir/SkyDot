# SPDX-License-Identifier: GPL-3.0-or-later
#
# The test pack's quest (converter/tools/testpack/main.cpp and scripts.hpp):
# start-game enabled, stage 10 is its start-up stage and shows objective 10,
# alias 0 is forced to the lever's reference and carries a script that sets
# stage 20 when the lever is activated; stage 20's fragment sets a global and
# completes the objective, and its log entry completes the quest.
extends SceneTree

const QUEST := 0x150
const GLOBAL := 0x140
const LEVER := 0x304
const INTERIOR := 0x300

var failures := 0
var stages: Array = []
var objectives: Array = []

func _init() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_quests: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_quests: SKIP (SKYDOT_TESTPACK unset)")
        quit(77)
        return
    _run(pack_dir)
    print("smoke_quests: failures=", failures)
    quit(failures)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func _setup(pack: SkydotPack, world: SkydotWorld) -> SkydotPapyrus:
    var vm := SkydotPapyrus.new()
    expect(vm.setup(pack, world) == OK, "the VM sets up")
    vm.error.connect(func(text: String) -> void: print("vm error: ", text))
    vm.quest_stage.connect(func(q: int, stage: int, text: String) -> void: stages.append([q, stage, text]))
    vm.objective_changed.connect(func(q: int, index: int, state: String, _text: String) -> void:
        objectives.append([q, index, state]))
    return vm

func _run(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    var world := pack.open_world()
    if world == null:
        expect(false, "world.fb opens")
        return

    # The world's view of the quest.
    expect(world.get_quest_count() == 1, "one quest")
    expect(world.find_quest("testpackquest") == QUEST, "found by editor id, any case")
    var info := world.get_quest(QUEST)
    expect(info.get("start_game_enabled", false), "start-game enabled")
    expect(info["fragment_script"] == "QF_TestpackQuest_00000150", "the fragment script")
    expect(info["stages"].size() == 2 and info["stages"][0]["start_up"], "stage 10 starts it up")
    expect(info["aliases"].size() == 1 and info["aliases"][0]["forced"] == LEVER,
           "alias 0 is forced to the lever")
    expect(info["aliases"][0]["scripts"].size() == 1, "and has a script")
    expect(world.get_global(GLOBAL).get("value", -1.0) == 0.0, "the global starts at 0")

    var vm := _setup(pack, world)
    expect(vm.start_game_enabled_quests() == 1, "the quest starts with the game")
    vm.update(0.0)
    var state := vm.get_quest_state(QUEST)
    expect(state.get("running", false), "and runs")
    expect(state.get("stage", 0) == 10, "at its start-up stage")
    expect(vm.get_alias_ref(QUEST, 0) == LEVER, "the alias holds the lever")
    expect(state["objectives"].get(10, {}).get("displayed", false), "stage 10's fragment shows objective 10")
    expect(stages.size() == 1 and stages[0][2] == "The lever waits.", "the stage signal carries the journal text")
    expect(not state.get("completed", true), "not completed yet")

    # Properties naming aliases and globals.
    var lever_ref = vm.call_method(QUEST, "TestpackQuestScript", "LeverRef", [])
    vm.update(0.0)
    expect(vm.take_result(lever_ref).get("value", 0) == LEVER, "the quest script reads its alias property")

    # A save now, to load after the quest has moved on.
    var saved := vm.save_state()

    # Activating the lever reaches the alias script as well as the lever's own.
    vm.attach_cell(INTERIOR)
    expect(vm.activate(LEVER) == 2, "the lever's script and the alias's run")
    vm.update(0.0)
    state = vm.get_quest_state(QUEST)
    expect(state.get("stage", 0) == 20, "the alias script set stage 20")
    expect(state.get("completed", false), "whose log entry completes the quest")
    expect(is_equal_approx(vm.get_global_value(GLOBAL), 5.0), "stage 20's fragment set the global")
    expect(state["objectives"][10]["completed"], "and completed the objective")
    expect(objectives.has([QUEST, 10, "completed"]), "objective signals")
    expect(state["stages_done"] == [10, 20], "both stages done")
    expect(not vm.set_stage(QUEST, 20), "a done stage does not repeat")
    expect(not vm.set_stage(QUEST, 30), "a missing stage is refused")

    # Stopping clears the aliases; the start-once rule does not apply.
    vm.stop_quest(QUEST)
    expect(not vm.get_quest_state(QUEST)["running"], "stopped")
    expect(vm.get_alias_ref(QUEST, 0) == 0, "and its alias cleared")
    expect(vm.start_quest(QUEST), "restarts")
    expect(vm.get_quest_state(QUEST)["stage"] == 10, "from its start-up stage")

    # The earlier save restores stage 10 and the untouched global.
    expect(vm.load_state(saved) == OK, "the save loads: " + vm.get_last_error())
    state = vm.get_quest_state(QUEST)
    expect(state.get("stage", 0) == 10 and state["stages_done"] == [10], "back at stage 10")
    expect(vm.get_alias_ref(QUEST, 0) == LEVER, "with the alias filled")
    expect(is_equal_approx(vm.get_global_value(GLOBAL), 0.0), "and the global at 0")
    vm.attach_cell(INTERIOR)
    vm.activate(LEVER)
    vm.update(0.0)
    expect(vm.get_quest_state(QUEST)["stage"] == 20, "the loaded alias script still sets stage 20")
    expect(vm.get_error_count() == 0, "no script errors (%d)" % vm.get_error_count())
