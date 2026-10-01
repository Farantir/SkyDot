# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the test pack twice under two different directory names and require the
# two packs to be byte-identical.

file(REMOVE_RECURSE "${WORK}")

foreach(name IN ITEMS a "a-considerably-longer-name")
    execute_process(COMMAND "${TESTPACK}" "${WORK}/${name}"
                    RESULT_VARIABLE rc OUTPUT_QUIET)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "bethconv-testpack failed in ${name}: ${rc}")
    endif()
endforeach()

set(first "${WORK}/a/pack")
set(second "${WORK}/a-considerably-longer-name/pack")

file(GLOB_RECURSE first_files RELATIVE "${first}" "${first}/*")
file(GLOB_RECURSE second_files RELATIVE "${second}" "${second}/*")
list(SORT first_files)
list(SORT second_files)

if(NOT first_files STREQUAL second_files)
    message(FATAL_ERROR "the two packs hold different files:\n  ${first_files}\n  ${second_files}")
endif()

if(first_files STREQUAL "")
    message(FATAL_ERROR "the pack is empty")
endif()

foreach(relative IN LISTS first_files)
    file(MD5 "${first}/${relative}" one)
    file(MD5 "${second}/${relative}" two)
    if(NOT one STREQUAL two)
        message(FATAL_ERROR "${relative} differs between two builds of the same input")
    endif()
endforeach()

message(STATUS "${first_files} identical across both builds")
