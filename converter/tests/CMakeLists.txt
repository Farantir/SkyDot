# SPDX-License-Identifier: GPL-3.0-or-later
add_subdirectory(unit)
add_subdirectory(corpus)
add_subdirectory(testpack)

# Always added. BETHCONV_BUILD_FUZZERS only controls the libFuzzer binaries;
# the replay drivers build everywhere so ctest runs the targets without clang.
add_subdirectory(fuzz)
