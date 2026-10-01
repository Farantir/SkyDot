#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bakes a pack into a Godot `.pck`.
#
# Imports a `bethconv view` of the pack, not the pack itself: assets are
# content-addressed, and glTF resolves image URIs relative to the document, so a
# direct import loses every material.
#
# The `.pck` is not byte-reproducible (Godot assigns resource ids without a
# seed); verify.gd checks it by loading it. See README.md.
set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
usage: bethconv-bake.sh --pack DIR --out FILE.pck [options]

  --pack DIR        the pack to bake (must contain vpath.idx and manifest.json)
  --out FILE        where to write the .pck
  --bethconv PATH   the bethconv binary (default: bethconv on PATH)
  --godot PATH      the Godot editor binary (default: $GODOT, else godot4)
  --work DIR        scratch directory (default: a temporary one, removed on exit)
  --filter TEXT     only bake virtual paths containing this substring
  --limit N         stop after this many index entries
  --from FILE       only bake the virtual paths listed in FILE (and their textures)
  --keep            keep the work directory
  --no-verify       skip loading the .pck back in a fresh project
USAGE
    exit 2
}

pack=""; out=""; bethconv="${BETHCONV:-bethconv}"; godot="${GODOT:-godot4}"
work=""; filter=""; limit=""; from=""; keep=0; verify=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pack) pack="${2:-}"; shift 2 ;;
        --out) out="${2:-}"; shift 2 ;;
        --bethconv) bethconv="${2:-}"; shift 2 ;;
        --godot) godot="${2:-}"; shift 2 ;;
        --work) work="${2:-}"; shift 2 ;;
        --filter) filter="${2:-}"; shift 2 ;;
        --limit) limit="${2:-}"; shift 2 ;;
        --from) from="$(realpath "${2:-}")"; shift 2 ;;
        --keep) keep=1; shift ;;
        --no-verify) verify=0; shift ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done

[[ -n "$pack" && -n "$out" ]] || usage
[[ -d "$pack" ]] || { echo "no such pack directory: $pack" >&2; exit 1; }
[[ -f "$pack/vpath.idx" ]] || { echo "$pack has no vpath.idx: not a pack" >&2; exit 1; }

command -v "$bethconv" >/dev/null 2>&1 || { echo "bethconv not found: $bethconv" >&2; exit 1; }
command -v "$godot" >/dev/null 2>&1 || { echo "godot not found: $godot" >&2; exit 1; }

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "$work" ]]; then
    work="$(mktemp -d)"
    [[ $keep -eq 1 ]] || trap 'rm -rf "$work"' EXIT
fi
project="$work/project"
rm -rf "$project"
mkdir -p "$project"

# ---- 1. view, so image URIs resolve ---------------------------------------
view_args=(view "$pack" -o "$project" -q)
[[ -n "$filter" ]] && view_args+=(--filter "$filter")
[[ -n "$limit" ]] && view_args+=(--limit "$limit")
[[ -n "$from" ]] && view_args+=(--from "$from")
echo "bake: materializing a view of $pack"
"$bethconv" "${view_args[@]}"

# ---- 2. the pinned project -----------------------------------------------
cp "$here/project.godot" "$project/project.godot"
cp "$here/bake.gd" "$project/bake.gd"

# ---- 3. import, then save scenes and write the pck ------------------------
# Two launches: --import must exit before a script can load its output.
echo "bake: importing"
"$godot" --headless --path "$project" --import

echo "bake: saving scenes and writing the pck"
"$godot" --headless --path "$project" --script bake.gd

[[ -f "$project/bethconv.pck" ]] || { echo "bake: no .pck was produced" >&2; exit 1; }

mkdir -p "$(dirname "$out")"
cp "$project/bethconv.pck" "$out"

# ---- 4. load it in a fresh project ----------------------------------------
if [[ $verify -eq 1 ]]; then
    consumer="$work/consumer"
    rm -rf "$consumer"
    mkdir -p "$consumer"
    printf 'config_version=5\n\n[application]\n\nconfig/name="bethconv bake check"\n' \
        > "$consumer/project.godot"
    cp "$here/verify.gd" "$consumer/verify.gd"
    cp "$out" "$consumer/bethconv.pck"
    echo "bake: verifying in a project that did not build it"
    "$godot" --headless --path "$consumer" --script verify.gd
fi

printf 'bake: %s (%s bytes)\n' "$out" "$(stat -c %s "$out")"
