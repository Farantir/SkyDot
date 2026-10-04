# Contributing

## Rules

These are enforced in review and, where possible, in CI.

### Clean room

Never decompile, disassemble or debug the game executable.

Allowed sources:

- the [UESP format documentation](https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format)
- xEdit's record definitions
- bytes you observed in files
- `docs/format-notes/`

Cite the source of every non-obvious format claim in a comment. An uncited magic
number blocks review.

### No Bethesda bytes in git

No `.esm .esp .esl .bsa .ba2 .nif .dds .pex .hkx .fuz` files, and no large
binaries that could be one of them. `../tools/ci/check-no-game-data.sh` enforces
this as a pre-commit hook and in CI. Run `../tools/ci/install-hooks.sh` once
after cloning.

Real data is tested through `tests/corpus/`, which reads `$SKYRIM_DATA_LE`,
`$SKYRIM_DATA_SE` and `$SKYRIM_DATA_VR` and records only counts, versions,
flags and hashes. Each install is optional and skipped when its variable is
unset, which is how CI runs.

### All untrusted input goes through `SpanReader`

Mod files contain truncated archives, wrong sizes and offsets past EOF. No raw
pointer arithmetic, no `reinterpret_cast` over a file buffer, no `memcpy` with a
size read from the file. Use `bethconv::io::SpanReader`;
`../tools/ci/check-raw-access.sh` enforces it. If `SpanReader` cannot express what
you need, extend it. To hand bytes to an API that wants `uint8_t` or `char`
(FlatBuffers, iostreams), use `io::as_u8` / `io::as_chars` from `byte_view.hpp`;
a `static_cast` pair through `void*` is a `reinterpret_cast` and is rejected too.

### Malformed input is never fatal

Parsers return `ParseResult<T>`. A bad file becomes a line in `report.json` and
a skipped asset, not an abort.

### Output is never redistributed

No prebuilt packs or downloads.

### Licences

GPL-3.0-or-later. Every dependency is listed in `THIRD_PARTY.md` with its
licence and pinned revision.

No AGPL code. skymp's Papyrus VM may be read as a reference but must not be
copied or linked; it would make the whole project AGPL.

## Practical

- C++23 with `std::expected`. Exceptions only for programmer error, never for
  bad input.
- Warnings are errors, including `-Wconversion` (`cmake/BethconvWarnings.cmake`).
- Before a PR:
  ```sh
  cmake --build --preset linux-debug-asan && ctest --preset linux-debug-asan
  ../tools/ci/check-raw-access.sh && ../tools/ci/check-no-game-data.sh
  ```
- New format code comes with a synthetic fixture that covers its error paths.
