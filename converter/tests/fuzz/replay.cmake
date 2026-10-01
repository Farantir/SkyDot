# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates the seed corpus and replays it through every fuzz target.
#
# Driven from ctest as `fuzz-replay`. Expects SEEDS_TOOL, SEEDS_DIR and BIN_DIR.

if(NOT DEFINED SEEDS_TOOL OR NOT DEFINED SEEDS_DIR OR NOT DEFINED BIN_DIR)
    message(FATAL_ERROR "replay.cmake needs SEEDS_TOOL, SEEDS_DIR and BIN_DIR")
endif()

# Regenerate so a stale corpus cannot hide a broken generator.
file(REMOVE_RECURSE "${SEEDS_DIR}")
file(MAKE_DIRECTORY "${SEEDS_DIR}")

execute_process(COMMAND "${SEEDS_TOOL}" "${SEEDS_DIR}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "seed generation failed (${rc}):\n${out}\n${err}")
endif()
message(STATUS "${out}")

# target -> seed directory. `forms` reuses the ESM seeds.
set(map_esm      esm)
set(map_forms    esm)
set(map_bsa      bsa)
set(map_nif      nif)
set(map_dds      dds)
set(map_strings  strings)
set(map_snapshot snapshot)
set(map_pex      pex)
set(map_lod      lod)

set(names esm forms bsa nif dds strings snapshot pex lod)
set(failed "")

foreach(name IN LISTS names)
    set(binary "${BIN_DIR}/bethconv-fuzz-${name}-replay")
    if(NOT EXISTS "${binary}")
        list(APPEND failed "${name}: replay binary missing at ${binary}")
        continue()
    endif()
    set(corpus "${SEEDS_DIR}/${map_${name}}")
    if(NOT IS_DIRECTORY "${corpus}")
        list(APPEND failed "${name}: no seeds under ${corpus}")
        continue()
    endif()

    # All seeds in one process, so state leaking between inputs shows up.
    execute_process(COMMAND "${binary}" "${corpus}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        list(APPEND failed "${name}: exited ${rc}\n${out}\n${err}")
    else()
        string(STRIP "${out}" out)
        message(STATUS "fuzz-${name}: ${out}")
    endif()
endforeach()

if(failed)
    string(REPLACE ";" "\n" report "${failed}")
    message(FATAL_ERROR "fuzz replay failures:\n${report}")
endif()
