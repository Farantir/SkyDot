# Contributing

Each half has its own `CONTRIBUTING.md` (`converter/`, `engine/`). Two rules
hold for the whole repository and are enforced by `tools/ci/`, in CI and as a
pre-commit hook:

- **No Bethesda bytes in git**, checked by extension and by size
  (`check-no-game-data.sh`).
- **All untrusted input through `SpanReader`** in the converter's parsers
  (`check-raw-access.sh`).

Commit messages carry no AI attribution trailers (`check-commit-msg.sh`).
After cloning, run `tools/ci/install-hooks.sh` (or
`git config core.hooksPath tools/git-hooks`) to enable the hooks.

A change to the pack format updates `formats/`, the converter and the engine
in one commit.
