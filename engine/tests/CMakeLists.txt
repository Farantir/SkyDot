# SPDX-License-Identifier: GPL-3.0-or-later
#
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
        "`ctest` will report that no tests ran.")
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

function(skydot_smoke_test name script)
    add_test(NAME "${name}"
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" --script "${_smoke}/${script}")
    set_tests_properties("${name}" PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        SKIP_RETURN_CODE 77
        # A script error aborts the check it happens in without counting a
        # failure.
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR"
        TIMEOUT 300
        ENVIRONMENT "SKYDOT_TESTPACK=${SKYDOT_TESTPACK};SKYDOT_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/scratch")
endfunction()

skydot_smoke_test(smoke_extension extension.gd)
skydot_smoke_test(smoke_pack pack.gd)
skydot_smoke_test(smoke_assets assets.gd)
skydot_smoke_test(smoke_papyrus papyrus.gd)
skydot_smoke_test(smoke_quests quests.gd)
skydot_smoke_test(smoke_lod lod.gd)
skydot_smoke_test(smoke_physics physics.gd)
skydot_smoke_test(smoke_navigation navigation.gd)
skydot_smoke_test(smoke_weather weather.gd)

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
        PASS_REGULAR_EXPRESSION "entered TestpackInterior.*leads to 0x00000213.*entered TestpackWorld.*leads to 0x00000303.*entered TestpackInterior"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;never appeared"
        TIMEOUT 120)
    add_test(NAME viewer_locked
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                     --pack "${SKYDOT_TESTPACK}"
                     --cell TestpackInterior --activate 0x303,0x213,0x212)
    set_tests_properties(viewer_locked PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        PASS_REGULAR_EXPRESSION "entered TestpackWorld.*0x00000213 TestpackDoor \\(DOOR\\) is locked \\(level 25\\)"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;entered TestpackInterior.*entered TestpackInterior"
        TIMEOUT 120)
    add_test(NAME viewer_lever
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                     --pack "${SKYDOT_TESTPACK}"
                     --cell TestpackInterior --activate 0x304,0x304)
    set_tests_properties(viewer_lever PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED godot_scan
        PASS_REGULAR_EXPRESSION "scripts: 1 references.*lever pulled 1.*0x00000301 disabled by script.*lever pulled 2.*0x00000301 enabled by script"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;never appeared;Papyrus:"
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
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;Papyrus:"
        TIMEOUT 120)
    add_test(NAME viewer_load
             COMMAND "${SKYDOT_GODOT}" --headless --path "${_game}" res://viewer/cell_viewer.tscn --
                     --pack "${SKYDOT_TESTPACK}"
                     --cell TestpackInterior --load "${_save}" --activate 0x304)
    set_tests_properties(viewer_load PROPERTIES
        LABELS smoke
        FIXTURES_REQUIRED "godot_scan;viewer_saved"
        PASS_REGULAR_EXPRESSION "loaded .*lever pulled 2.*0x00000301 enabled by script"
        FAIL_REGULAR_EXPRESSION "SCRIPT ERROR;Papyrus:;cannot load"
        TIMEOUT 120)
endif()
