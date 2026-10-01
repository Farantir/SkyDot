# SPDX-License-Identifier: GPL-3.0-or-later
#
# The engine's CI runs against the test pack, so catch breakage here.
# bethconv-testpack verifies its own output and exits nonzero on failure, so
# running it is the test.

add_test(NAME testpack
         COMMAND bethconv_testpack "${CMAKE_CURRENT_BINARY_DIR}/out")
set_tests_properties(testpack PROPERTIES LABELS testpack)

# Same input must give byte-identical output. The two output directories have
# different names because the directory name once leaked into vpath.idx.
add_test(NAME testpack_deterministic
         COMMAND ${CMAKE_COMMAND}
                 -DTESTPACK=$<TARGET_FILE:bethconv_testpack>
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/determinism
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/determinism.cmake)
set_tests_properties(testpack_deterministic PROPERTIES LABELS testpack)
