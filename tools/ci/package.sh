#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exports the game and packs each platform's archive:
#
#   tools/ci/package.sh VERSION BINARIES OUT
#
# BINARIES holds one binaries-<platform>/ directory per platform from
# tools/ci/collect-binaries.sh (linux-x86_64, windows-x86_64); each platform
# found there is packaged. OUT gets skydot-VERSION-linux-x86_64.tar.gz and
# skydot-VERSION-windows-x86_64.zip.
#
# Needs godot4.7 (GODOT overrides it) and its export templates for the release
# builds of both platforms.
set -euo pipefail

cd "$(dirname "$0")/../.."
repo="$PWD"
version="$1"
binaries="$(realpath "$2")"
mkdir -p "$3"
out="$(realpath "$3")"
godot="${GODOT:-godot4.7}"
stage="$repo/engine/build/package"
rm -rf "$stage"

# A release version must be the one the binaries report (bethconv --version).
if [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    for file in converter/CMakeLists.txt engine/CMakeLists.txt; do
        if ! grep -q "VERSION $version\$" "$file"; then
            echo "error: $file does not have VERSION $version" >&2
            exit 1
        fi
    done
fi

for dir in "$binaries"/binaries-*/; do
    cp "$dir"/extension/* engine/game/bin/
done
# Godot only exports extensions an editor scan has listed.
"$godot" --headless --path engine/game --import >/dev/null 2>&1 || true

for dir in "$binaries"/binaries-*/; do
    platform="$(basename "$dir")"
    platform="${platform#binaries-}"
    name="skydot-$version-$platform"
    case "$platform" in
    linux-x86_64) preset=Linux exe=skydot.x86_64 ;;
    windows-x86_64) preset=Windows exe=skydot.exe ;;
    *) echo "unknown platform: $platform" >&2; exit 2 ;;
    esac

    mkdir -p "$stage/$name"
    "$godot" --headless --path engine/game --export-release "$preset" "$stage/$name/$exe"
    if [[ ! -f "$stage/$name/$exe" || ! -f "$stage/$name/skydot.pck" ]]; then
        echo "error: the $preset export wrote no game" >&2
        exit 1
    fi
    cp "$dir"/bin/* "$stage/$name/"
    cp README.md LICENSE LICENSE-EXCEPTION.md "$stage/$name/"

    case "$platform" in
    linux-*) tar -C "$stage" -czf "$out/$name.tar.gz" "$name" ;;
    windows-*) (cd "$stage" && zip -q -r "$out/$name.zip" "$name") ;;
    esac
done

ls -l "$out"
