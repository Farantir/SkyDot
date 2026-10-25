# SPDX-License-Identifier: GPL-3.0-or-later
#
# Native unit tests of the Godot-free half. They come first: the lookup below
# returns when there is no editor, and they need none.
add_subdirectory(unit)

# Headless Godot runs: the test is the editor actually loading the library.
# Each script in smoke/ is a SceneTree that exits with its error count.
#
# Scripts needing converter output skip with exit 77 (ctest: "Not Run") when it
# is not configured. SKYDOT_TESTPACK_REQUIRE=1 turns skips into failures.
find_program(SKYDOT_GODOT
    NAMES godot4.7 godot4 godot Godot
    HINTS ENV GODOT
    DOC "The Godot 4.7 editor binary the smoke tests run in")

if(NOT SKYDOT_GODOT)
    message(WARNING
        "skydot: no Godot binary found (looked for godot4.7, godot4, godot; set "
        "SKYDOT_GODOT or \$GODOT). The smoke tests are not registered, and "
        "`ctest` runs the unit tests only.")
    return()
endif()

# Location of a `bethconv-testpack` pack: by default the sibling bethconv
# checkout's build tree; the environment variable wins.
set(_default_testpack "$ENV{SKYDOT_TESTPACK}")
if(NOT _default_testpack)
    set(_sibling "${CMAKE_SOURCE_DIR}/../converter/build/linux-debug-asan/tests/testpack/out/pack")
    if(EXISTS "${_sibling}/manifest.json")
        set(_default_testpack "${_sibling}")
    endif()
endif()
set(SKYDOT_TESTPACK "${_default_testpack}" CACHE PATH
    "A pack directory written by bethconv-testpack; empty skips the pack tests")

# A pack of world.fb format 11 with large references (a Skyrim SE conversion
# with Tamriel and its LOD): smoke_large_refs runs the layer against it. Empty
# runs that test's pack-free half only.
set(SKYDOT_LARGE_REF_PACK "$ENV{SKYDOT_LARGE_REF_PACK}" CACHE PATH
    "A format 11 pack with large references; empty skips the layer test")

# A converted Skyrim SE pack: the `sse` tests replay activators of the real
# game data headless (no screenshot files; positions are in the tests below).
# Empty registers none of them.
set(SKYDOT_SSE_PACK "$ENV{SKYDOT_SSE_PACK}" CACHE PATH
    "A converted Skyrim SE pack; empty skips the sse tests")

# The pack tool's smoke test runs a real bethconv: by default the sibling
# checkout's build (release first); the environment variable wins.
set(_default_bethconv "$ENV{SKYDOT_BETHCONV}")
if(NOT _default_bethconv)
    foreach(_preset IN ITEMS linux-release linux-debug-asan windows-release)
        foreach(_exe IN ITEMS bethconv bethconv.exe)
            set(_candidate "${CMAKE_SOURCE_DIR}/../converter/build/${_preset}/tools/bethconv-cli/${_exe}")
            if(NOT _default_bethconv AND EXISTS "${_candidate}")
                set(_default_bethconv "${_candidate}")
            endif()
        endforeach()
    endforeach()
endif()
set(SKYDOT_BETHCONV "${_default_bethconv}" CACHE FILEPATH
    "A bethconv binary for the pack tool test; empty runs its pure checks only")

set(_game "${CMAKE_SOURCE_DIR}/game")
set(_smoke "${CMAKE_CURRENT_SOURCE_DIR}/smoke")

# Godot loads extensions listed in .godot/extension_list.cfg, which only the
# editor scan writes; headless --script runs do not scan. So the scan is a
# fixture for every smoke test (run via scan.cmake; see there).
add_test(NAME smoke_scan
         COMMAND ${CMAKE_COMMAND}
                 -DGODOT=${SKYDOT_GODOT}
                 -DPROJECT=${_game}
                 -DEXTENSION=res://skydot.gdextension
                 -P ${_smoke}/scan.cmake)
set_tests_properties(smoke_scan PROPERTIES
    LABELS smoke
    TIMEOUT 300
    FIXTURES_SETUP godot_scan)

# A script error aborts the check it happens in without counting a failure,
# and a shader that Godot cannot compile is only logged: both fail the test.
# smoke_shaders makes one on purpose, to see that it is reported, and looks for
# the others itself.
function(skydot_smoke_test name script)
    set(_errors "SCRIPT ERROR|SHADER ERROR")
    if(ARGC GREATER 2)
        set(_errors "${ARGV2}")
    endif()
    add_test(NAME "${name}"
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/${script}")
    set_tests_properties("${name}" PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "${_errors}"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_TESTPACK=${SKYDOT_TESTPACK};SKYDOT_LARGE_REF_PACK=${SKYDOT_LARGE_REF_PACK};SKYDOT_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/scratch;SKYDOT_BETHCONV=${SKYDOT_BETHCONV}")
endfunction()

skydot_smoke_test(smoke_extension extension.gd)
skydot_smoke_test(smoke_pack pack.gd)
skydot_smoke_test(smoke_assets assets.gd)
skydot_smoke_test(smoke_papyrus papyrus.gd)
skydot_smoke_test(smoke_quests quests.gd)
skydot_smoke_test(smoke_lod lod.gd)
skydot_smoke_test(smoke_large_refs large_refs.gd)
skydot_smoke_test(smoke_animation animation.gd)
skydot_smoke_test(smoke_actors actors.gd)
skydot_smoke_test(smoke_ai ai.gd)
skydot_smoke_test(smoke_physics physics.gd)
skydot_smoke_test(smoke_navigation navigation.gd)
skydot_smoke_test(smoke_weather weather.gd)
skydot_smoke_test(smoke_session session.gd)
skydot_smoke_test(smoke_game_save game_save.gd)
skydot_smoke_test(smoke_activation_prompt activation_prompt.gd)
skydot_smoke_test(smoke_shaders shaders.gd "SCRIPT ERROR")
skydot_smoke_test(smoke_viewer_settings viewer_settings.gd)
skydot_smoke_test(smoke_packtool packtool.gd)

# The viewer walks through the test pack's door pair: out of the interior,
# then (locks picked) back in through the locked outside door. Needs the pack,
# so it is only registered when one is configured.
if(SKYDOT_TESTPACK)
    add_test(NAME viewer_doors
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                     --pack "${SKYDOT_TESTPACK}"
                     --cell TestpackInterior --activate 0x303,0x213 --pick-locks on)
    set_tests_properties(viewer_doors PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        PASS_REGULAR_EXPRESSION "entered TestpackInterior.*leads to 0x00000213.*scripts: 2 references unloaded.*entered TestpackWorld.*leads to 0x00000303.*entered TestpackInterior"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR;never appeared"
        TIMEOUT 120)
    add_test(NAME viewer_locked
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                     --pack "${SKYDOT_TESTPACK}" --time 22
                     --cell TestpackInterior --activate 0x303)
    # The lock is stored on the outside door (0x213) only, as plugins do; the
    # pair shares it, so the inside door does not open either. At 22:00: by
    # day the NPC's work package unlocks it.
    set_tests_properties(viewer_locked PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        PASS_REGULAR_EXPRESSION "0x00000303 TestpackDoor \\(DOOR\\) is locked \\(level 25, Apprentice\\)"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR;entered TestpackWorld"
        TIMEOUT 120)
    # Picking it from inside opens both doors: back in through 0x213.
    add_test(NAME viewer_unlocked_pair
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                     --pack "${SKYDOT_TESTPACK}" --pick-locks on
                     --cell TestpackInterior --activate 0x303,0x213)
    set_tests_properties(viewer_unlocked_pair PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        PASS_REGULAR_EXPRESSION "entered TestpackWorld.*0x00000213 TestpackDoor \\(DOOR\\) leads to"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR;is locked"
        TIMEOUT 120)
    add_test(NAME viewer_lever
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                     --pack "${SKYDOT_TESTPACK}"
                     --cell TestpackInterior --activate 0x304,0x304)
    set_tests_properties(viewer_lever PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        PASS_REGULAR_EXPRESSION "scripts: 1 references.*lever pulled 1.*0x00000301 disabled by script.*lever pulled 2.*0x00000301 enabled by script"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR;never appeared;Papyrus:"
        TIMEOUT 120)
    # The VR shell without an OpenXR session (the OpenXR module is off without
    # --xr-mode on, so its interface cannot start; with the flag and no
    # runtime Godot itself opens a modal alert dialog and waits for a click):
    # it says so, runs as a desktop game on the test pack and quits by itself.
    add_test(NAME vr_shell_fallback
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://vr/vr_shell.tscn --
                     --pack "${SKYDOT_TESTPACK}"
                     --cell TestpackInterior --quit-after 10)
    set_tests_properties(vr_shell_fallback PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        PASS_REGULAR_EXPRESSION "vr: no OpenXR runtime, desktop fallback.*entered TestpackInterior.*vr: quit"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR;never appeared;OpenXR session started"
        TIMEOUT 120)
    # A viewer screenshot's JSON (F12) brings the viewer back to its place;
    # --pack given on the command line wins over the shot's.
    add_test(NAME viewer_from_shot
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                     --from-shot "${CMAKE_CURRENT_SOURCE_DIR}/smoke/testpack_shot.json"
                     --pack "${SKYDOT_TESTPACK}" --pick-locks on --activate 0x303)
    set_tests_properties(viewer_from_shot PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        PASS_REGULAR_EXPRESSION "entered TestpackInterior.*leads to 0x00000213"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR;cannot read the shot"
        TIMEOUT 120)
    # Pull the lever, save, then load in a fresh run and pull it again: the
    # count and the cube's state must carry over.
    set(_save "${CMAKE_CURRENT_BINARY_DIR}/scratch/viewer.skydot")
    add_test(NAME viewer_save
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                     --pack "${SKYDOT_TESTPACK}"
                     --cell TestpackInterior --activate 0x304 --save-to "${_save}")
    set_tests_properties(viewer_save PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        FIXTURES_SETUP viewer_saved
        PASS_REGULAR_EXPRESSION "lever pulled 1.*saved "
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR;Papyrus:"
        TIMEOUT 120)
    add_test(NAME viewer_load
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                     --pack "${SKYDOT_TESTPACK}"
                     --cell TestpackInterior --load "${_save}" --activate 0x304)
    set_tests_properties(viewer_load PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED "godot_scan;viewer_saved"
        PASS_REGULAR_EXPRESSION "loaded .*lever pulled 2.*0x00000301 enabled by script"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR;Papyrus:;cannot load"
        TIMEOUT 120)
endif()

# Activator regression runs on the real SE pack (GAMEPLAY-PLAN step 1). Each
# stands near what it activates (animators stop beyond 250 m from the camera,
# positions are game units) with time stopped, and checks the behaviour graph
# and script log lines the original game's data must produce.
if(SKYDOT_SSE_PACK)
    add_test(NAME sse_actor_values
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/actor_values.gd")
    set_tests_properties(sse_actor_values PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_combat_data
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/combat_data.gd")
    set_tests_properties(sse_combat_data PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_magic_data
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/magic_data.gd")
    set_tests_properties(sse_magic_data PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_magic_effects
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/magic_effects.gd")
    set_tests_properties(sse_magic_effects PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_magic_archetypes
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/magic_archetypes.gd")
    set_tests_properties(sse_magic_archetypes PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 600
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_magic_cast
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/magic_cast.gd")
    set_tests_properties(sse_magic_cast PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_magic_npc
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/magic_npc.gd")
    set_tests_properties(sse_magic_npc PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_magic_enchant
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/magic_enchant.gd")
    set_tests_properties(sse_magic_enchant PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_shout_player
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/shout_player.gd")
    set_tests_properties(sse_shout_player PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_shout_data
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/shout_data.gd")
    set_tests_properties(sse_shout_data PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_magic_menu
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/magic_menu.gd")
    set_tests_properties(sse_magic_menu PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_combat_rules
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/combat_rules.gd")
    set_tests_properties(sse_combat_rules PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_combat_melee
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/combat_melee.gd")
    set_tests_properties(sse_combat_melee PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_combat_npc
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/combat_npc.gd")
    set_tests_properties(sse_combat_npc PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_combat_ai
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/combat_ai.gd")
    set_tests_properties(sse_combat_ai PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_creatures
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/creatures.gd")
    set_tests_properties(sse_creatures PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_creatures_wolf
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/creatures_wolf.gd")
    set_tests_properties(sse_creatures_wolf PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_player_death
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/player_death.gd")
    set_tests_properties(sse_player_death PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_combat_death
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/combat_death.gd")
    set_tests_properties(sse_combat_death PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_dialogue
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/dialogue.gd")
    set_tests_properties(sse_dialogue PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_scene
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/scene.gd")
    set_tests_properties(sse_scene PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_items
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/items.gd")
    set_tests_properties(sse_items PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_items_session
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/items_session.gd")
    set_tests_properties(sse_items_session PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    add_test(NAME sse_crafting_session
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/crafting_session.gd")
    set_tests_properties(sse_crafting_session PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_SSE_PACK=${SKYDOT_SSE_PACK}")

    function(skydot_sse_test name)
        cmake_parse_arguments(T "" "PASS;FAIL;FIXTURES_SETUP;FIXTURES_REQUIRED" "ARGS" ${ARGN})
        add_test(NAME ${name}
                 COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                         --pack "${SKYDOT_SSE_PACK}" --walk off --no-input --time 13.13 --time-scale 0
                         ${T_ARGS})
        set(_fixtures godot_scan ${T_FIXTURES_REQUIRED})
        set(_fail "SCRIPT ERROR;SHADER ERROR")
        if(T_FAIL)
            list(APPEND _fail "${T_FAIL}")
        endif()
        set_tests_properties(${name} PROPERTIES
            LABELS sse
            FIXTURES_REQUIRED "${_fixtures}"
            PASS_REGULAR_EXPRESSION "${T_PASS}"
            FAIL_REGULAR_EXPRESSION "${_fail}"
            TIMEOUT 180)
        if(T_FIXTURES_SETUP)
            set_tests_properties(${name} PROPERTIES FIXTURES_SETUP ${T_FIXTURES_SETUP})
        endif()
    endfunction()

    # Activating a container opens its menu (flat viewer and VR shell fallback).
    skydot_sse_test(sse_open_container
        ARGS --cell RiverwoodRiverwoodTrader --pick-locks on --hud --open-container 0x5A3B6 --open-inventory
             --quit-after 20
        PASS "0x0005A3B6 PersonalChestSmall \\(CONT\\) opens")
    add_test(NAME sse_vr_open_container
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://vr/vr_shell.tscn --
                     --pack "${SKYDOT_SSE_PACK}" --cell RiverwoodRiverwoodTrader --pick-locks on --hud
                     --time 13.13 --time-scale 0 --open-container 0x5A3B6 --quit-after 20)
    set_tests_properties(sse_vr_open_container PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        PASS_REGULAR_EXPRESSION "PersonalChestSmall \\(CONT\\) opens.*vr: quit"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 180)

    # Activating a workbench opens the crafting menu: Alvor's forge (recipes,
    # Riverwood exterior), the Sleeping Giant Inn's alchemy lab (ingredients
    # that share an effect), and the VR shell's panel for the forge.
    skydot_sse_test(sse_open_forge
        ARGS --world Tamriel --at 20459,-45300,100 --look 0,0 --hud --give 0x5ACE4x2,0x800E4x2
             --open-container 0xD6987 --quit-after 20
        PASS "crafting: [A-Za-z ]+ opens \\(type 1, [1-9][0-9]* recipes\\)")
    skydot_sse_test(sse_open_alchemy
        ARGS --cell RiverwoodSleepingGiantInn --hud --give 0x4DA00x2,0x34D22x2
             --open-container 0x26D7B --quit-after 20
        PASS "crafting: [A-Za-z ]+ opens \\(type 5, [2-9][0-9]* ingredients\\)")
    add_test(NAME sse_vr_open_forge
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://vr/vr_shell.tscn --
                     --pack "${SKYDOT_SSE_PACK}" --world Tamriel --at 20459,-45300,100 --look 0,0 --hud
                     --time 13.13 --time-scale 0 --give 0x5ACE4x2,0x800E4x2 --open-container 0xD6987
                     --quit-after 20)
    set_tests_properties(sse_vr_open_forge PROPERTIES
        LABELS sse
        FIXTURES_REQUIRED godot_scan
        PASS_REGULAR_EXPRESSION "crafting: [A-Za-z ]+ opens \\(type 1, [1-9][0-9]* recipes\\).*vr: quit"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;SHADER ERROR"
        TIMEOUT 180)

    # Bleak Falls Barrow: the pillar lever sits at the pillars' room.
    set(_pillars --cell BleakFallsBarrow01 --at 5006.8,-893.3,-636 --look 197.1,38.5
                 --activate-wait 200 --quit-after 900)
    set(_solution 0xAA8CB,0xAA8CB,0xAA8CC,0xAA8CC,0xAA8CD)
    skydot_sse_test(sse_pillars_solved
        ARGS ${_pillars} --activate ${_solution},0xAA8C3
        PASS "0x000AA8C3 behaviour: LeverPushIdle -> LeverPush on LeverPushStart.*0x0003B9F1 behaviour: Closed -> Open on open.*0x0003B9F1 behaviour: Open -> Opened on Opening"
        FAIL "projectile 0x000141EF")
    skydot_sse_test(sse_pillars_unsolved
        ARGS ${_pillars} --activate 0xAA8C3
        PASS "projectile 0x000141EF from"
        FAIL "0x0003B9F1 behaviour: Closed -> Open")
    # The other lever of the room opens the same gate.
    skydot_sse_test(sse_lever_gate
        ARGS --cell BleakFallsBarrow01 --at 5238,-1500,-600 --look 0,0
             --activate 0x6A9E2 --activate-wait 300 --quit-after 400
        PASS "0x0006A9E2 behaviour: Pulled -> PulledToPushed on FullPush.*0x0003B9F1 behaviour: Closed -> Open on open.*0x0003B9F1 behaviour: Open -> Opened on Opening")
    skydot_sse_test(sse_pull_chain_gate
        ARGS --cell E3demoBleakFallsBarrow --at 2912,-8114,-2350 --look 0,0
             --activate 0x8473B --activate-wait 300 --quit-after 600
        PASS "0x0008473B behaviour: Up -> PlayDown on Pull.*0x0007CA50 behaviour: Closed -> Open on open")
    # The blades swing on over and over after the first pull and stop at the second.
    skydot_sse_test(sse_pull_chain_blades
        ARGS --cell BleakFallsBarrow01 --at 6000,-7200,-2130 --look 0,0
             --activate 0xFDA45,0xFDA45 --activate-wait 400 --quit-after 400
        PASS "0x000FDA45 behaviour: Up -> PlayDown on Pull.*0x0003AC33 behaviour: State00 -> State02 on Single.*0x0003AC33 behaviour: State00 -> State02 on Single.*0x000FDA45 behaviour: Up -> PlayDown on Pull.*0x0003AC33 behaviour: State02 -> State00 on Reset")
    # The Hall of Stories door: three wheels turned to the solution, then the
    # keyhole, which wants the Golden Claw (0x39647).
    set(_claw --cell BleakFallsBarrow02 --at 500,-320,-150 --look 4.712,-10
              --activate 0x4E2BA,0x4E2BA,0x4E2BB,0x4E2BB,0x4E2BC,0x4E2BC,0x4E2B9
              --activate-wait 600 --quit-after 600)
    skydot_sse_test(sse_claw_door
        ARGS ${_claw} --give 0x39647
        PASS "0x0004E2B9 behaviour: Activated -> Open on Open"
        FAIL "You need the Golden Claw")
    skydot_sse_test(sse_claw_door_no_claw
        ARGS ${_claw}
        PASS "You need the Golden Claw"
        FAIL "0x0004E2B9 behaviour: Activated -> Open on Open")
    # An opened gate stays open through a save and a load, with no replay.
    set(_gate_save "${CMAKE_CURRENT_BINARY_DIR}/scratch/sse_gate.skydot")
    skydot_sse_test(sse_gate_save
        ARGS ${_pillars} --activate ${_solution},0xAA8C3 --save-to "${_gate_save}"
        FIXTURES_SETUP sse_gate_saved
        PASS "0x0003B9F1 behaviour: Closed -> Open on open.*saved "
        FAIL "projectile 0x000141EF")
    skydot_sse_test(sse_gate_load
        ARGS --cell BleakFallsBarrow01 --at 5006.8,-893.3,-636 --look 197.1,38.5
             --load "${_gate_save}" --quit-after 100
        FIXTURES_REQUIRED sse_gate_saved
        PASS "0x0003B9F1 graph restored: Opened.*loaded "
        FAIL "0x0003B9F1 behaviour: Closed -> Open;cannot load")
endif()

# The tests that write saves share user://, and sse_player_death takes the newest save there and
# deletes every .skydot: one at a time.
foreach(_t sse_player_death sse_magic_effects sse_magic_cast sse_shout_player sse_magic_archetypes sse_magic_enchant sse_magic_menu sse_combat_death sse_items_session sse_crafting_session sse_combat_melee smoke_game_save)
    if(TEST ${_t})
        set_property(TEST ${_t} APPEND PROPERTY RESOURCE_LOCK user_saves)
    endif()
endforeach()
