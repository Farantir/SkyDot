# Game rules live in GDScript, where script mods can change them

Decided on 2026-10-31 during the architecture review (refactoring plan
after step 7b).

## Context

ADR 0001 lets script mods (GDScript against the Mod API) use any licence,
while native mods stay GPL and should contribute to the engine instead. A
rule that lives in C++ can only be changed by a native mod or an engine
patch, so every rule moved into C++ takes it out of reach of the mods ADR
0001 is meant to serve. Until now the choice was made case by case and leaned
towards C++, because Godot-free C++ is unit-tested and GDScript had only
smoke tests. The architecture review proposed deepening casting and attack
rules by moving them into C++. That would have been the wrong direction for
modding.

## Decision

- **Game rules belong in the Game layer's GDScript.** This means what the
  game does with its data: casting and hand states, attack, damage and
  stamina rules, death and detection policy, crafting outcomes, player state
  (dead, invisible, can act). Shared by player and NPC code, so a script mod
  changes both at once.
- **C++ holds data, state and the interpreter.** That is pack data access,
  the stores that saves write (actor values, inventories, reference
  states, quests), the Papyrus VM and its natives (Papyrus scripts are
  moddable as data), the condition evaluator (ADR 0004), and asset, render,
  physics and navigation work.
- **A rule moves to C++ only for a measured hot path.** When it does, it
  keeps its tunables and decision points reachable from GDScript (parameters
  or a hook), and the reason is written next to it.
- **Refactors move rules up, not down.** Rules that are in C++ today and
  should be moddable (for example parts of `vm/papyrus_combat.cpp` and
  `ai/ai_combat.cpp`) move into GDScript when their area is next worked on.
- **GDScript rules get unit tests.** There is a headless GDScript test layer
  below the smoke tests, so moving logic into GDScript does not lose test
  coverage.

## Consequences

- Deep modules for rules are GDScript classes with small interfaces. They
  ask C++ narrow data questions instead of many round trips over
  `SkydotPapyrus`.
- The Mod API (M3) can expose or replace these GDScript modules directly.
- Some per-frame rule code runs slower in GDScript than in C++. It is
  measured before anything moves back.
