# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run `bethconv form --magic` on the pack and require the census numbers of
# the pack the engine's sse tests run on (step 7a-1).

execute_process(COMMAND "${BETHCONV}" form --magic "${PACK}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bethconv form --magic failed (${rc}): ${err}")
endif()

foreach(line IN ITEMS
        "whole world: 1451 SPEL, 472 ALCH, 104 SCRL, 757 ENCH, 180 INGR, 0 other items with effects; 5213 effect entries"
        "slice sources: 166 SPEL, 234 ALCH, 11 SCRL, 348 ENCH, 91 INGR, 0 other; 1387 effect entries"
        "not resolved: 40 spell-list forms that are neither SPEL nor LVSP (shouts), 0 actor bases missing, 0 effects naming no MGEF")
    string(FIND "${out}" "${line}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "the census lacks the line\n  ${line}\nit printed:\n${out}")
    endif()
endforeach()

# The most used archetypes in the slice, in census order.
foreach(pattern IN ITEMS
        "\n  34  PeakValueModifier +726 +107 +1790 +299"
        "\n   0  ValueModifier +363 +69 +1637 +421")
    if(NOT out MATCHES "${pattern}")
        message(FATAL_ERROR "the census lacks ${pattern}; it printed:\n${out}")
    endif()
endforeach()
