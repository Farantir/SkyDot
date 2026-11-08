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

## How systems and fixes are built

- **As the game does it, never guessed.** Behaviour and values come from the
  game's data (records, GMSTs, NIF, havok and behaviour files, scripts) and are
  checked in the running game (`skycompare`, SkyrimRemote) wherever possible.
  A constant fitted to a measurement is not a fix.
- **One system, as broad as possible.** Before adding a code path, check
  whether an existing one can cover the case. Differences between cases
  (human, creature, dragon; player and NPC; interior and exterior) are data or
  a specialisation of one shared system (e.g. inheritance), not parallel
  systems. Write as little code as possible and reuse as much as possible.
- **The most general fix for a bug.** Find the actual root cause, not the
  symptom, and ask what else shares it: could it affect other actors, races,
  objects or cells? Fix the mechanism for all of them and check a few of the
  others, not only the reported case. Never key a fix to one ref, editor ID,
  race, mesh or cell.

A review checks every change against these three rules. A guessed constant, a
fix for one case, or a parallel system is a finding even when the code works.
