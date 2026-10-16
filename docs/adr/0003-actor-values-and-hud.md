# Actor values in the VM's state, the HUD in the shell

Step 3 of the order of work (SYSTEMS.md P5, U1, M3) adds actor values, a HUD
and gamepad input. Decided on 2026-10-17:

## Actor values

- The per-actor state record is C++ (`vm/actor_states.hpp`, no Godot),
  owned by `SkydotPapyrus` beside `RefStates`, because Papyrus natives read
  and write it and it needs unit tests. It is sparse: an actor has a record
  only once a value of it changed; everything else is computed from the data
  (NPC_, RACE) through `WorldAccess::actor_base`.
- It writes its own save section, `ACTV` (version 1), into the ADR 0002
  container. Unlike the sections listed there it is optional on load: a
  save without it means no actor value changed.
- A record goes away only with the reference (`forget_ref`); a cell
  detaching keeps it, as Skyrim keeps an unloaded actor's damage until the
  cell resets (W11).

## The HUD reads the game, never the other way

- The HUD belongs to the shell (`viewer/hud.gd` today; a VR shell draws its
  own). It reads `GameSession` getters (`actor_value`, `actor_value_max`,
  `activation_target`) and signals (`actor_died`); nothing in `game/game/`
  knows a HUD exists.
- What the prompt says (verb, name, lock level) is game logic and comes from
  the session; what the player looks at is the session's viewpoint ray, the
  same one `activate_in_view` uses, so a VR shell sets the viewpoint to its
  hand and gets the same prompt.
- Sprinting is the shell's body, stamina the game's: the player object of
  ADR 0002 gains `is_sprinting()` and `set_sprint_allowed(allowed)`, and the
  session drains stamina and forbids sprinting at 0.

## Input

Gamepads map to the existing InputMap actions plus four look actions; the
shell turns stick deflection into look speed. Debug commands stay
keyboard-only.

## Considered options

- **Actor values in GDScript in the game layer:** natives would call out
  of the VM into GDScript, and the regen and death rules would have no unit
  tests.
- **Actor values inside the VM's `VMST` section:** it would tie their
  format to the VM core's; ADR 0002 gives each state class its own section.
- **The HUD subscribing to `SkydotPapyrus` signals:** the shell would
  depend on the VM, which ADR 0002 hides behind the session.
