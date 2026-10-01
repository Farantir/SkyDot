#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# All untrusted input goes through SpanReader. A grep, not an analysis: it
# rejects the constructs that bypass bounds checks outside the exempt files.
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 2

# Directories that parse untrusted bytes.
GUARDED=(converter/src/archive converter/src/record converter/src/mesh converter/src/texture
         converter/src/script converter/src/pack)

# The reader itself, and the mmap wrapper where an OS pointer becomes a span.
EXEMPT=(converter/src/io/span_reader.cpp converter/src/io/mapped_file.cpp)

# reinterpret_cast / bit_cast: overlaying a struct on a file buffer.
# memcpy / memmove: unchecked size argument.
PATTERNS='reinterpret_cast|\bmemcpy\b|\bmemmove\b|\bstd::bit_cast\b'

status=0
existing=()
for dir in "${GUARDED[@]}"; do
    [[ -d "$dir" ]] && existing+=("$dir")
done

if [[ ${#existing[@]} -eq 0 ]]; then
    echo "check-raw-access: no guarded directories present yet, nothing to check"
    exit 0
fi

exempt_args=()
for f in "${EXEMPT[@]}"; do
    exempt_args+=(":(exclude)$f")
done

hits=$(git grep --untracked -nE "$PATTERNS" -- "${existing[@]}" "${exempt_args[@]}" 2>/dev/null)

if [[ -n "$hits" ]]; then
    echo "check-raw-access: raw byte access outside SpanReader:" >&2
    echo "$hits" >&2
    echo >&2
    echo "Route this through bethconv::io::SpanReader. If it cannot express what" >&2
    echo "you need, extend SpanReader instead." >&2
    status=1
fi

# Pointer arithmetic on data() also bypasses the check.
ptr_hits=$(git grep --untracked -nE '\.data\(\)\s*\+|\bdata\(\)\[' -- "${existing[@]}" "${exempt_args[@]}" 2>/dev/null)
if [[ -n "$ptr_hits" ]]; then
    echo "check-raw-access: pointer arithmetic on .data() outside SpanReader:" >&2
    echo "$ptr_hits" >&2
    status=1
fi

[[ $status -eq 0 ]] && echo "check-raw-access: clean (${#existing[@]} guarded dirs)"
exit $status
