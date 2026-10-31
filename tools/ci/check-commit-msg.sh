#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Rejects commit messages carrying AI attribution trailers.
#   check-commit-msg.sh FILE        check one message (commit-msg hook)
#   check-commit-msg.sh --range A..B  check every commit in the range (CI)
set -u
pattern='co-authored-by:.*(claude|anthropic)|noreply@anthropic\.com|claude\.ai/code|claude-session:|generated with .*claude'

check() { # label, message text on stdin
  local hit
  hit=$(grep -n -i -E -- "$pattern" | head -1)
  if [ -n "$hit" ]; then
    echo "commit message rejected ($1): ${hit#*:}" >&2
    return 1
  fi
}

if [ "${1:-}" = "--range" ]; then
  [ $# -eq 2 ] || { echo "usage: $0 FILE | --range A..B" >&2; exit 2; }
  bad=0
  for c in $(git rev-list "$2"); do
    git log -1 --format=%B "$c" | check "$(git log -1 --format='%h %s' "$c")" || bad=1
  done
  exit $bad
fi
[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: $0 FILE | --range A..B" >&2; exit 2; }
check "$1" < "$1" || exit 1
