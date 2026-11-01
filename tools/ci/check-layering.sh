#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The include layering of engine/extension/src (ARCHITECTURE.md section 7). A
# grep over `#include` lines, not an analysis: it names file:line of every
# include that points upward.
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 2

SRC=engine/extension/src

# directory:forbidden include prefixes (alternatives of a regex)
RULES=(
    "state:(vm|magic|data|build|ai|world|actors|render|physics|session|assets)/|godot_cpp/"
    "data:(vm|magic|build|ai)/"
    "magic:(vm|build|ai)/"
    "vm:(build|ai)/"
)

status=0
for rule in "${RULES[@]}"; do
    dir=${rule%%:*}
    forbidden=${rule#*:}
    [[ -d "$SRC/$dir" ]] || continue
    hits=$(grep -rnE --include='*.hpp' --include='*.cpp' --include='*.h' \
        "^[[:space:]]*#[[:space:]]*include[[:space:]]+[\"<]($forbidden)" "$SRC/$dir")
    if [[ -n "$hits" ]]; then
        echo "check-layering: $dir/ must not include these:" >&2
        echo "$hits" >&2
        status=1
    fi
done

[[ $status -eq 0 ]] && echo "check-layering: clean"
exit $status
