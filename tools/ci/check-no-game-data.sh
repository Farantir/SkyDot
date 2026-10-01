#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# No Bethesda bytes in git. Runs as a pre-commit hook (--staged) and in CI.
# Checks by extension, and by size to catch a renamed archive.
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 2

# Game-data extensions. Includes .nif/.dds/.pex: test fixtures for those are
# built in memory by tests/support/, never committed.
FORBIDDEN_EXT='\.(esm|esp|esl|bsa|ba2|nif|dds|pex|hkx|fuz|bik|lip|tri|btr|bto|seq|xwm)$'

# Anything above this is suspicious in a source repo.
MAX_BYTES=$((2 * 1024 * 1024))

status=0

if [[ -n "${1:-}" ]]; then
    files=$(git diff --cached --name-only --diff-filter=ACM)
else
    files=$(git ls-files)
fi

while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    if [[ "${f,,}" =~ $FORBIDDEN_EXT ]]; then
        echo "check-no-game-data: forbidden extension: $f" >&2
        status=1
        continue
    fi
    # Size of the indexed (staged) blob, not the working-tree file: that is
    # what gets committed.
    size=$(git cat-file -s ":$f" 2>/dev/null || echo 0)
    if (( size > MAX_BYTES )); then
        echo "check-no-game-data: $f is $size bytes (limit $MAX_BYTES)." >&2
        status=1
    fi
done <<< "$files"

if [[ $status -ne 0 ]]; then
    echo >&2
    echo "No Bethesda-derived file may ever enter this repository. Point the" >&2
    echo "corpus harness at your own install instead (tests/corpus/README.md)." >&2
fi

[[ $status -eq 0 ]] && echo "check-no-game-data: clean"
exit $status
