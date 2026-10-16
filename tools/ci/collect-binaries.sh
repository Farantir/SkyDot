#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Collects a release build's binaries for tools/ci/package.sh:
#
#   tools/ci/collect-binaries.sh PRESET OUT
#
#   OUT/bin/        bethconv and what it needs at run time; goes next to the
#                   exported game
#   OUT/extension/  the skydot extension; goes to engine/game/bin/ for the
#                   export
#
# PRESET is linux-release or windows-release, built in converter/ and engine/.
# On Windows it runs after the MSVC environment is set up (VCToolsRedistDir).
set -euo pipefail
shopt -s nullglob

cd "$(dirname "$0")/../.."
preset="$1"
out="$2"
mkdir -p "$out/bin" "$out/extension"
cli="converter/build/$preset/tools/bethconv-cli"

case "$preset" in
linux-release)
    cp "$cli/bethconv" "$out/bin/"
    cp engine/game/bin/libskydot.linux.template_release.x86_64.so "$out/extension/"
    # libstdc++ and libgcc are linked in; OpenMP's runtime is not on every
    # system, so it ships next to bethconv, which looks there first (the CI
    # build links with the rpath $ORIGIN).
    gomp="$(ldd "$cli/bethconv" | awk '/libgomp/ { print $3 }')"
    [[ -z "$gomp" ]] || cp -L "$gomp" "$out/bin/"
    strip --strip-debug "$out/bin/bethconv" "$out/extension/"*.so
    ;;
windows-release)
    cp "$cli/bethconv.exe" "$cli/"*.dll "$out/bin/"
    cp engine/game/bin/libskydot.windows.template_release.x86_64.dll "$out/extension/"
    # The VC++ runtime and OpenMP's (vcomp), which Windows does not always have.
    redist="$(cygpath -u "$VCToolsRedistDir")/x64"
    cp "$redist"/Microsoft.VC*.CRT/*.dll "$redist"/Microsoft.VC*.OpenMP/*.dll "$out/bin/"
    ;;
*)
    echo "unknown preset: $preset" >&2
    exit 2
    ;;
esac

ls -l "$out/bin" "$out/extension"
