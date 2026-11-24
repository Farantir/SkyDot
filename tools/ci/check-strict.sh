#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The engine under clang with warnings as errors, as CI's linux-clang-debug
# job builds it (the local gcc build misses clang-only warnings such as
# -Wdouble-promotion on float-to-double initialisation). Builds and runs the
# unit tests. MSVC-only warnings still surface only in CI.
set -euo pipefail

cd "$(dirname "$0")/../../engine"

preset=linux-clang-werror
if [[ ! -f build/$preset/CMakeCache.txt ]]; then
    cmake --preset "$preset" > /dev/null
fi
if ! out=$(cmake --build --preset "$preset" -- -k 0 2>&1); then
    grep -E 'error:|FAILED:' <<< "$out" | head -40
    echo "check-strict: build failed ($preset)"
    exit 1
fi
if ! out=$(ctest --test-dir "build/$preset" -L unit 2>&1); then
    tail -30 <<< "$out"
    echo "check-strict: unit tests failed ($preset)"
    exit 1
fi
echo "check-strict: clean"
