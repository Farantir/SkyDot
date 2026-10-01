# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run the editor's project scan and check what it wrote, not its exit code.
#
# Godot 4.7.2 (rc1 and stable, 2026-09-12) crashes on exit after the first
# headless `--import` of a project whose extension registers a class, after
# writing .godot/extension_list.cfg. See docs/godot-notes.md. So the extension
# list is checked instead; a clean exit without it still fails.
foreach(v GODOT PROJECT EXTENSION)
    if(NOT ${v})
        message(FATAL_ERROR "scan.cmake: ${v} is not set")
    endif()
endforeach()

execute_process(
    COMMAND "${GODOT}" --headless --path "${PROJECT}" --import
    RESULT_VARIABLE rc
    OUTPUT_QUIET)

set(list_file "${PROJECT}/.godot/extension_list.cfg")
if(NOT EXISTS "${list_file}")
    message(FATAL_ERROR "scan.cmake: the scan (exit ${rc}) wrote no ${list_file}")
endif()
file(STRINGS "${list_file}" listed)
if(NOT "${EXTENSION}" IN_LIST listed)
    message(FATAL_ERROR
        "scan.cmake: the scan (exit ${rc}) did not list ${EXTENSION} in ${list_file}; "
        "it holds: ${listed}")
endif()
if(NOT rc EQUAL 0)
    message(STATUS "scan.cmake: the scan exited ${rc} after writing the extension list "
                   "(the first-scan abort described at the top of this file); continuing")
endif()
