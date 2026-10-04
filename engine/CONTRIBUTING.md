# Contributing

## Rules

The same rules as bethconv, applied to the engine. Enforced in review and,
where possible, in CI.

### Clean room

Never decompile, disassemble or debug the game executable.

Sources: `formats/pack-format.md` (the contract this engine reads) and,
for what records mean, the sources bethconv uses: the
[UESP format documentation](https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format),
xEdit's record definitions and observed data.

Cite the source of every non-obvious format claim in a comment, as
`extension/src/assets/pack.cpp` does for `pack-format.md`.

### No Bethesda bytes in git

No `.esm .esp .esl .bsa .ba2 .nif .dds .pex .hkx .fuz` or `.pck` files, and no
large binaries that could be one. `../tools/ci/check-no-game-data.sh` enforces
this as a pre-commit hook and in CI. Run `../tools/ci/install-hooks.sh` once
after cloning.

Tests use bethconv's `bethconv-testpack` output (no game data) via
`SKYDOT_TESTPACK`. Numbers recorded from packs of real installs are counts,
never content.

### The engine reads packs, never game files

No parsers for plugins, archives, NIFs or DDS belong here. If the engine needs
something a pack lacks, the pack format grows (with a version bump in bethconv)
and this reader follows the document.

Packs are still untrusted files: bounds are checked, manifest counts are
compared with what is on disk, and unknown versions are refused with a
message.

### Output is never redistributed

No prebuilt packs or baked `.pck` downloads.

### Keep the limits visible

The README lists the non-goals first. Changes that make them sound closer than
they are get reverted.

### Licences

GPL-3.0-or-later. Every dependency is listed in `THIRD_PARTY.md` with its
licence and pinned revision.

No AGPL code. skymp's Papyrus VM may be read as a reference for `.pex`, but not
copied or linked.

## Practical

- C++20 in `extension/`. GDScript in `game/` is glue only, never a hot path.
- Errors cross the boundary as `godot::Error`, with the explanation available
  via `SkydotPack.get_error()` and in the error log.
- Warnings are errors, `-Wconversion` included (`cmake/SkydotWarnings.cmake`).
  godot-cpp's headers are `SYSTEM`.
- Before a PR:
  ```sh
  cmake --build --preset linux-debug && ctest --preset linux-debug
  ../tools/ci/check-no-game-data.sh
  ```
- Code that does not touch Godot goes in `skydot_core` (see
  `extension/CMakeLists.txt`) and is covered by Catch2 tests in `tests/unit/`,
  which build their inputs in memory and need no editor. Anything else is a
  headless editor run (`tests/smoke/`). Each refusal path gets a test that
  triggers it with a file the test writes; new reported numbers are checked
  against the test pack (see `converter/tools/testpack/main.cpp`).
- Record hard-won facts about the editor and bindings in `docs/godot-notes.md`,
  with version and date.
