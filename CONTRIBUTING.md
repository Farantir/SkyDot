# Contributing

Each half has its own `CONTRIBUTING.md` (`converter/`, `engine/`). Two rules
hold for the whole repository and are enforced by `tools/ci/`, in CI and as a
pre-commit hook:

- **No Bethesda bytes in git**, checked by extension and by size
  (`check-no-game-data.sh`).
- **All untrusted input through `SpanReader`** in the converter's parsers
  (`check-raw-access.sh`).

A change to the pack format updates `formats/`, the converter and the engine
in one commit.
