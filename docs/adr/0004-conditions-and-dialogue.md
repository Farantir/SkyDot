# One condition evaluator; dialogue state beside the VM, the menu in the shell

Step 4 of the order of work (SYSTEMS.md Q5, D1) adds conditions for quests
and dialogue and a dialogue menu. Decided on 2026-10-17:

## Conditions

- There is one CTDA evaluator, `engine/extension/src/conditions/`, C++
  without Godot, unit-tested with a fake host. AI packages, dialogue,
  quests and later perks call it; `ai/packages.cpp` no longer has its own.
- Condition functions read the game through a Godot-free `ConditionWorld`
  interface (quest states, actor bases and values, inventory, locations,
  dialogue state, script variables, clock), implemented by the owner of
  that state (`SkydotPapyrus`); callers add overrides for what only they
  know (AI: the current package, package data).
- A function without a binding is unknown: the condition is false and
  counted (one warning per function index). AI packages keep their old
  rule (value 0) until their unknown functions are bound, so this step does
  not change schedules. Functions of systems that do not exist yet are
  bound to that system's resting value and listed in
  `engine/docs/conditions.md`; they are not unknown.

## Dialogue

- Dialogue data is part of world.fb (format 26): branches, topics, infos
  with conditions and responses, quest dialogue conditions. Voice file
  paths are derived later from editor IDs, plugins and response numbers;
  world.fb stores no paths.
- Selection (which greeting, which topics, which INFO answers) and the
  dialogue state (said INFOs and topics, Say Once, talked to the player)
  are C++ without Godot (`engine/extension/src/dialogue/`), owned by
  `SkydotPapyrus` beside `QuestStates` and `ActorStates`, because
  conditions, INFO script fragments and saves all live there.
- The state writes its own save section, `DLGS` (version 1), into the ADR
  0002 container; optional on load (a save without it: nothing was said).
- The dialogue menu belongs to the shell (`viewer/dialogue_menu.gd`). It
  reads `GameSession` signals and calls its `dialogue_*` methods; nothing in
  `game/game/` knows how the menu looks, so a VR shell can draw it as a 3D
  panel.

## Considered options

- **Condition functions in GDScript:** every package tick and topic list
  would cross into GDScript per condition; no unit tests.
- **Unknown functions as value 0 everywhere (the AI rule):** most dialogue
  conditions are `== 1` tests, but `== 0` tests on unknown functions would
  show lines the game hides; false is the safer error for dialogue.
- **Dialogue state inside the VM's `VMST` section:** ties its format to the
  VM core's; ADR 0002 gives each state class its own section.
