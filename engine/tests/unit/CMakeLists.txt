# SPDX-License-Identifier: GPL-3.0-or-later
#
# Native unit tests of skydot_core, the Godot-free half of the extension. No
# editor and no test pack: the inputs are built in memory (support/). They link
# skydot_core_testable, the same sources compiled like the tests (see
# extension/CMakeLists.txt for why not skydot_core itself).
include(Catch)

add_executable(skydot_core_tests
    support/script_builder.cpp
    support/script_builder.hpp
    support/vm_rig.cpp
    support/vm_rig.hpp
    support/world_builder.cpp
    support/fake_condition_world.hpp
    support/world_builder.hpp
    test_actor_states.cpp
    test_actors.cpp
    test_activation.cpp
    test_behavior.cpp
    test_conditions.cpp
    test_combat_ai.cpp
    test_combat_runtime.cpp
    test_flight.cpp
    test_magic_choice.cpp
    test_crafting.cpp
    test_crafting_runtime.cpp
    test_activation_runtime.cpp
    test_item_runtime.cpp
    test_magic_runtime.cpp
    test_dialogue.cpp
    test_dialogue_runtime.cpp
    test_game_time.cpp
    test_items.cpp
    test_magic.cpp
    test_magic_spells.cpp
    test_shouts.cpp
    test_perks.cpp
    test_progression.cpp
    test_progression_natives.cpp
    test_magic_natives.cpp
    test_casting.cpp
    test_enchanting.cpp
    test_finish_grace.cpp
    test_large_refs.cpp
    test_leveled.cpp
    test_mapped_file.cpp
    test_packages.cpp
    test_save_file.cpp
    test_scene_runtime.cpp
    test_scenes.cpp
    test_triggers.cpp
    test_vm.cpp
    test_ref_forget.cpp
    test_vm_bench.cpp
    test_vm_natives_actor_extra.cpp
    test_vm_natives_debug.cpp
    test_vm_natives_effectshader.cpp
    test_vm_natives_form.cpp
    test_vm_natives_game.cpp
    test_vm_natives_noop.cpp
    test_vm_natives_objectreference.cpp
    test_vm_natives_quest.cpp
    test_vm_natives_sound.cpp
    test_vm_natives_utility.cpp
    test_vm_natives_weapon.cpp
    test_vm_natives_world.cpp
    test_vm_objects.cpp
    test_vm_save.cpp
    test_vm_threads.cpp
    test_vm_value.cpp
    test_vpath.cpp
    test_world_data.cpp
)
target_link_libraries(skydot_core_tests
    PRIVATE skydot_core_testable skydot::warnings Catch2::Catch2WithMain)

catch_discover_tests(skydot_core_tests PROPERTIES LABELS unit)
