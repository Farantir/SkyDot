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
    "perks:(vm|build|ai|world|session|render|physics|assets)/|godot_cpp/"
    "vm:(build|ai)/"
)

# glob (under $SRC):forbidden include prefixes. Natives families are Godot-free
# free functions over interfaces (ARCHITECTURE.md, "Natives and the world"), so
# they link into the unit tests; so do the per-domain modules
# (vm/<domain>_runtime.*, listed one by one). The Godot helpers for papyrus*.cpp live in
# vm/papyrus_common.hpp, which is not a natives_* file.
FILE_RULES=(
    "vm/natives_*:godot_cpp/|vm/papyrus\\.hpp|world/|build/|ai/|session/"
    "vm/combat_runtime.*:godot_cpp/|vm/papyrus\\.hpp|world/|build/|ai/|session/"
    "vm/crafting_runtime.*:godot_cpp/|vm/papyrus\\.hpp|world/|build/|ai/|session/"
    "vm/activation_runtime.*:godot_cpp/|vm/papyrus\\.hpp|world/|build/|ai/|session/"
    "vm/item_runtime.*:godot_cpp/|vm/papyrus\\.hpp|world/|build/|ai/|session/"
    "vm/magic_runtime.*:godot_cpp/|vm/papyrus\\.hpp|world/|build/|ai/|session/"
    "vm/progression_runtime.*:godot_cpp/|vm/papyrus\\.hpp|world/|build/|ai/|session/"
    "vm/dialogue_runtime.*:godot_cpp/|vm/papyrus\\.hpp|world/|build/|ai/|session/"
    "vm/scene_runtime.*:godot_cpp/|vm/papyrus\\.hpp|world/|build/|ai/|session/"
)

# FILE's includes as "line:path", a quoted path that resolves next to FILE
# (same directory, ../) rewritten relative to $SRC so the rules see it.
includes() {
    local f=$1 dir line path
    dir=$(dirname "$f")
    grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]+["<]' "$f" |
        while IFS= read -r line; do
            path=$(sed -E 's/^[0-9]+:[[:space:]]*#[[:space:]]*include[[:space:]]+["<]([^">]*)[">].*/\1/' <<<"$line")
            if [[ $line =~ include[[:space:]]+\" && ( $path == ../* || $path == ./* || -f $dir/$path ) ]]; then
                path=$(realpath -m --relative-to="$SRC" "$dir/$path")
            fi
            echo "${line%%:*}:$path"
        done
}

# Files under $SRC matching GLOB whose includes start with FORBIDDEN, as file:line:path.
violations() {
    local forbidden=$1 f
    shift
    for f in "$@"; do
        [[ -f "$f" ]] || continue
        includes "$f" | grep -E "^[0-9]+:($forbidden)" | sed "s|^|$f:|"
    done
}

status=0
for rule in "${RULES[@]}"; do
    dir=${rule%%:*}
    forbidden=${rule#*:}
    [[ -d "$SRC/$dir" ]] || continue
    mapfile -t files < <(find "$SRC/$dir" -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' \))
    hits=$(violations "$forbidden" "${files[@]}")
    if [[ -n "$hits" ]]; then
        echo "check-layering: $dir/ must not include these:" >&2
        echo "$hits" >&2
        status=1
    fi
done

for rule in "${FILE_RULES[@]}"; do
    glob=${rule%%:*}
    forbidden=${rule#*:}
    # shellcheck disable=SC2206
    files=("$SRC"/$glob)
    hits=$(violations "$forbidden" "${files[@]}")
    if [[ -n "$hits" ]]; then
        echo "check-layering: $glob must not include these:" >&2
        echo "$hits" >&2
        status=1
    fi
done

[[ $status -eq 0 ]] && echo "check-layering: clean"
exit $status
