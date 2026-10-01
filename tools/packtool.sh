#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Starts the pack tool (engine/game/packtool/) from a checkout, building what
# is missing first:
#
#   - bethconv (converter/build/linux-release), which the tool runs
#   - the skydot extension (engine/build/linux-debug), which the editor binary
#     loads
#   - the project's import scan, which registers the extension
#
#   tools/packtool.sh            build if needed, then start
#   tools/packtool.sh --rebuild  build both halves even if they exist
#
# GODOT=/path/to/godot overrides the Godot 4.7 binary; VCPKG_ROOT defaults to
# ~/.local/opt/vcpkg. Extra arguments after -- go to Godot.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
godot="${GODOT:-$(command -v godot4.7 || command -v godot4 || command -v godot || true)}"
export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/.local/opt/vcpkg}"

rebuild=0
godot_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rebuild) rebuild=1 ;;
        --) shift; godot_args=("$@"); break ;;
        -h|--help) sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

extension="$repo/engine/game/bin/libskydot.linux.template_debug.x86_64.so"
if [[ $rebuild -eq 1 || ! -f "$extension" ]]; then
    echo "== building the engine extension (linux-debug)"
    (cd "$repo/engine" &&
        { [[ -d build/linux-debug ]] || cmake --preset linux-debug; } &&
        cmake --build --preset linux-debug)
fi

# Godot only loads extensions an editor scan has listed.
if [[ ! -f "$repo/engine/game/.godot/extension_list.cfg" ]]; then
    echo "== first start: scanning the project"
    "$godot" --headless --path "$repo/engine/game" --import >/dev/null 2>&1 || true
fi

echo "== starting the pack tool ($godot)"
exec "$godot" --path "$repo/engine/game" "${godot_args[@]}"
