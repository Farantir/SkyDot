#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Starts the pack tool (engine/game/packtool/) from a checkout, building what
# is missing first:
#
#   - bethconv (converter/build/linux-release), which the tool runs
#   - the skydot extension, optimised (engine/build/linux-play), into
#     engine/build/play-project/: an overlay of engine/game/ (symlinks to
#     everything but bin/ and .godot/), so the editor binary loads the fast
#     library while game/bin/ keeps the debug one that tests and dev runs use
#   - the project's import scan, which registers the extension
#
#   tools/packtool.sh            build (incremental), then start
#   tools/packtool.sh --rebuild  also rebuild the converter even if it exists
#   tools/packtool.sh --debug    run engine/game/ with the debug library
#                                (engine/build/linux-debug) instead
#
# GODOT=/path/to/godot overrides the Godot 4.7 binary; VCPKG_ROOT defaults to
# ~/.local/opt/vcpkg. Extra arguments after -- go to Godot.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
godot="${GODOT:-$(command -v godot4.7 || command -v godot4 || command -v godot || true)}"
export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/.local/opt/vcpkg}"

rebuild=0
debug=0
godot_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rebuild) rebuild=1 ;;
        --debug) debug=1 ;;
        --) shift; godot_args=("$@"); break ;;
        -h|--help) sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

if [[ -z "$godot" ]]; then
    echo "error: no Godot 4.7 binary found; install it or set GODOT=/path/to/godot" >&2
    exit 1
fi

bethconv="$repo/converter/build/linux-release/tools/bethconv-cli/bethconv"
if [[ $rebuild -eq 1 || ! -x "$bethconv" ]]; then
    echo "== building the converter (linux-release)"
    (cd "$repo/converter" &&
        { [[ -d build/linux-release ]] || cmake --preset linux-release; } &&
        cmake --build --preset linux-release)
fi

if [[ $debug -eq 1 ]]; then
    project="$repo/engine/game"
    preset=linux-debug
else
    project="$repo/engine/build/play-project"
    preset=linux-play
    mkdir -p "$project/bin"
    for entry in "$repo/engine/game/"* "$repo/engine/game/".[!.]*; do
        name="$(basename "$entry")"
        [[ -e "$entry" && "$name" != bin && "$name" != .godot ]] || continue
        ln -sfn "$entry" "$project/$name"
    done
fi

echo "== building the engine extension ($preset)"
(cd "$repo/engine" &&
    { [[ -d build/$preset ]] || cmake --preset "$preset"; } &&
    cmake --build --preset "$preset")

# Godot only loads extensions an editor scan has listed.
if [[ ! -f "$project/.godot/extension_list.cfg" ]]; then
    echo "== first start: scanning the project"
    "$godot" --headless --path "$project" --import >/dev/null 2>&1 || true
fi

echo "== starting the pack tool ($godot, $preset)"
exec "$godot" --path "$project" "${godot_args[@]}"
