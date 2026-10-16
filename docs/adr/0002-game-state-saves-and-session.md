# One owner for game state: sectioned saves, one clock, a game session

Until 0.1.0 the game state was split across the VM's binary save, a
Dictionary in `SkydotPapyrus`, GDScript nodes and `SkydotClock`, and the game
was composed inside the viewer (`cell_viewer.gd`). That caused lost state on
load (moved references, the clock), deleted references whose scripts kept
running, two game clocks, and failed loads that emptied the running game
(`CODE-REVIEW.md`, B1–B6, B9). Decided on 2026-10-16:

## The save file

- A container: magic `SKYS`, `u32` container version (1), then sections
  until the end, then a trailer. A section is a FourCC tag, a `u32` section
  version, a `u64` length and that many bytes. The trailer is a CRC-32 of
  everything before it. All integers little-endian.
- Each state class writes and reads its own section with its own version,
  starting at 1. There is no version chain across classes. A reader rejects
  a section version newer than it knows and skips unknown tags; a missing
  required section rejects the file.
- Sections (required unless marked):
  - `HEAD`: engine version text; the pack's `world.hash` and load order from
    `manifest.json`. A save from another pack or load order is refused (form
    IDs would map to the wrong objects).
  - `CLCK`: the clock (days passed, fraction = time of day; time scale;
    current weather form).
  - `VMST`: the VM core (`Vm::save` without the parts below).
  - `RFST`: reference states (`RefStates`, below).
  - `PLCD`, `DEST`, `INVT`, `ANIM`, `FLST`: placed references, destruction,
    inventories, animation/graph states, form lists — one section each, moved
    out of `Vm::save`.
  - `QUST`: quests, quest scripts, alias handles and fills, globals.
  - `PAPY`: what `SkydotPapyrus` itself tracks: attached references,
    trigger occupants, actor positions, the next form ID.
  - `PLYR`: where the player is (cell or worldspace, position, yaw, pitch),
    written by the shell's player object (see below).
  - Later, optional at first: `AIAC` (AI actors), `PROJ` (live projectiles).
- Saves from 0.1.0 and older are not read. There is no compatibility promise
  before 1.0; the old format code is deleted, not kept behind a version.
- Writing goes to `<path>.tmp`, flushed, then renamed over `<path>` (B9).
- The container lives in `engine/extension/src/save/` (`SaveWriter`,
  `SaveReader`; no Godot) inside `skydot_core`, so unit tests cover it. The
  Godot binding takes and returns sections as `PackedByteArray`s, so a
  GDScript class (the player) can write a section too.

## Loading is two-phase

Every section is decoded into new, detached objects first (a fresh VM state,
`RefStates`, clock state, ...). Only when all of them decoded does the game
swap them in, retire the live graphs and enter the saved place. A corrupt,
truncated or foreign save leaves the running game untouched and returns an
error (B6). The CRC is checked before any section is decoded.

## Reference state

- `vm/ref_states.hpp` (no Godot, next to `runtime_refs.hpp`): `RefStates`
  holds per reference what scripts changed about it: disabled, open state,
  locked, activation blocked, and the moved-to transform (Skyrim units,
  radians). It replaces `SkydotPapyrus::disabled_`, `open_`, `locked_`,
  `blocked_` and `moved_`, and writes `RFST` (B2).
- One forget path: `Vm::detach(ref)` drops a reference's script instances,
  timers, update registrations, scheduled host calls, inventory and
  animation state; `SkydotPapyrus::forget_ref(ref)` calls it plus
  `RefStates`, `DestructionStates` and `PlacedRefs`. Delete and finished
  transient explosions use it (B1); cell detach (B8) will. Other scripts
  may still hold a detached instance as a value, so `detach` marks it and
  keeps it until `Vm::clear`; dispatch treats it as gone.

## One clock

- `SkydotClock` is the only source of game time. `WorldAccess` gains
  `game_days()` (days passed, fraction = time of day) and `time_scale()`;
  `SkydotPapyrus` gets the clock with `attach_clock(clock)` and answers them
  from it. `Utility.GetCurrentGameTime` uses them, and so will game-time
  update registrations and `WaitGameTime` when the VM gets them;
  `k_timescale` goes away.
- The globals `GameHour`, `GameDaysPassed` and `TimeScale` (found by editor
  ID; Skyrim.esm `0x38`, `0x39`, `0x3A`) read from the clock; `SetValue` on them moves the
  clock (`shift`) or sets its scale. `VM time` (real seconds) stays for
  `Wait`, real-time updates and `GetCurrentRealTime`.
- The clock writes and reads `CLCK` (B4, B5).

## The game session

- `game/game/game_session.gd`, `class_name GameSession extends Node`, is the
  composition root of the game. It owns the pack, world, `SkydotPapyrus`,
  `SkydotAi`, `SkydotClock`, `SkydotStreamer`, `Place`, `PlaceTransition`,
  `PlacedRefs`, `Projectiles`, `Sounds` and the saves, and connects every
  `SkydotPapyrus` signal in one place. The game logic of `script_bridge.gd`
  (graph persistence, Reset moves, enable/disable visibility, havok
  impulses, motion types, PlayAnimation, activation outcomes, open/lock
  visuals) moves into `game/game/`; `save_service.gd` moves there too.
- It runs headless: a smoke test builds it with a plain `Node3D` as the
  viewpoint and no shell.
- The shell (today the viewer, later a flat game and a VR shell) owns the
  camera and player rig, input, image space, the debug overlay, shots,
  benchmarks, settings and the command line. It gives the session:
  - a viewpoint `Node3D` the streamer and AI follow;
  - a player object with `save_section() -> PackedByteArray` and
    `load_section(bytes) -> bool` for `PLYR`, and `place_at(position, yaw,
    pitch)`.
- The session's surface towards the shell: `open(config) -> bool` (pack
  path, AI on/off, start hour, time scale, still), `start(cell_or_world,
  position)`, `activate(ref)`, `save_game(path) -> Error`,
  `load_game(path) -> Error`; signals `failed(text)`, `message(text, box)`,
  `quest_started`, `quest_stage`, `objective_changed`, `place_built(root,
  cell_id)`, `place_left`, `actor_arrived`, `activations_finished`. The
  journal and notes in the debug overlay subscribe to these, not to
  `SkydotPapyrus`.

## Considered options

- **Keep extending the Dictionary save and `Vm::load`'s version chain:**
  each feature adds a branch in two places, and nothing validates the whole
  file before applying it.
- **Godot `var_to_bytes` for everything:** easy for GDScript, but untyped,
  not testable without Godot, and the VM half is already binary.
- **Keep the game in `cell_viewer.gd` and add the VR shell beside it:** the
  VR shell would copy the composition and drift from it.
