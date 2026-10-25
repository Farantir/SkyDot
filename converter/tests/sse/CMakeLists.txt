# SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks against a converted Skyrim SE pack (the engine's `sse` tests use the
# same one). Empty registers none of them, as in CI.
set(SKYDOT_SSE_PACK "$ENV{SKYDOT_SSE_PACK}" CACHE PATH
    "A converted Skyrim SE pack; empty skips the sse tests")

if(SKYDOT_SSE_PACK)
    # `bethconv form --magic`: the magic census of step 7a-1 (sources in the
    # whole world and in the first-milestone slice).
    add_test(NAME sse_magic_census
             COMMAND ${CMAKE_COMMAND}
                     -DBETHCONV=$<TARGET_FILE:bethconv_cli>
                     -DPACK=${SKYDOT_SSE_PACK}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/magic_census.cmake)
    set_tests_properties(sse_magic_census PROPERTIES LABELS sse TIMEOUT 120)
endif()
