# Papyrus

The VM (`extension/src/vm/`) runs the script assets bethconv decodes from
`.pex` (`../formats/pack-format.md`, "Script assets"). It is written from
the PEX format and bethconv's measurements (`docs/spikes/papyrus.md` there); no
other VM's code was read, so it stays GPL-3.0.

## Pieces

| File | Role |
| --- | --- |
| `vm/script_class.*` | One script: verified, indices checked, identifiers resolved to registers, variables, `self` or `::State` once at load |
| `vm/value.hpp` | Values; arrays are shared, everything else copied |
| `vm/vm.*` | Instances, threads, the interpreter, the scheduler, timers, native binding |
| `vm/save.cpp` | The VM's save format |
| `vm/papyrus.*` | `SkydotPapyrus`: the VM bound to a pack and `SkydotWorld`, the engine's natives, triggers, saves |
| `vm/quests.cpp` | Quests, aliases, stages, objectives, globals |

## Execution

- Every call runs on a thread with an explicit frame stack; nothing recurses
  natively. A latent native returns a wait time, or an event name with a
  timeout, and the thread sleeps until the VM clock passes it or `notify`
  names the event.
- Threads run in creation order, each until it finishes, sleeps or uses its
  budget (100,000 instructions per update). Events start one thread per
  attached script that handles them. Runs are deterministic, random numbers
  included (a seeded generator whose state is saved).
- `RegisterForSingleUpdate`/`RegisterForUpdate` set one timer per script;
  when it is due, OnUpdate goes to that script alone.
- Runtime errors (None objects, bad indices, division by zero, missing
  functions, unbound natives) are logged and give None or 0; the script
  continues, as in the game. Missing `On...` functions are not errors.

## Objects and dispatch

An object value is a form, the class it is seen as (the type it was created
or cast with) and, if one is attached, the script instance of that class. A
method is looked up in an attached script derived from that class, in its
current state and then its default state, up the class chain; otherwise in
the class's own chain. Casting to a script type finds the attached instance
or gives None; casting to an engine type (a class that declares natives)
keeps the form.

Instances hold their variables per class in the chain. `::State` reads and
writes the instance's state; the compiled `GotoState` does the rest.

## Attaching

`SkydotPapyrus.attach(cell, ref)` takes the base's scripts, then the
reference's (VMAD), merging entries of the same name so the reference's
property values win, skips scripts marked removed, sets properties (auto
properties directly, others through their setter) and sends `OnInit`.
`attach_cell(cell)` attaches every scripted reference of a cell, including
those without a model (triggers); `attach_built(root)` those a build tagged,
and sends them `OnLoad`.

A property naming a reference or a quest attaches that one's scripts too,
loaded or not: the game keeps such references persistent, and a quest's
scripts exist before it starts. A property without an instance behind it (as
`GlobalVariable.Value`) runs its getter or setter on the object, like a
method.

## Quests

`start_quest` fills the quest's aliases, attaches its scripts and its
aliases' scripts (once), sends them `OnInit` and sets the start-up stage.
`start_game_enabled_quests` does that for every start-game-enabled quest, as
a new game does (443 of vanilla SE's 2,338 start). A quest set to run once
starts only once.

`set_stage` (and `Quest.SetStage`) starts the quest if needed, marks the stage
done, runs its fragment (`Fragment_N` of the fragment script, on a new thread),
emits `quest_stage` with the journal text, completes or fails the quest if the
log entry says so and stops it at a shut-down stage. A done stage does not
repeat unless the quest allows it; a stage the quest lacks is refused.
Objectives emit `objective_changed`.

An alias is an object of its own: a handle from `ALIAS_HANDLE_BASE`
(0xFF800000) up, given out on first use and saved. Its scripts attach to the
handle, and every event sent to a reference (activation, triggers, `OnLoad`,
`send_event`) also goes to the aliases that reference fills. Aliases fill
from a forced reference (or location), a unique actor (its lowest placed
ACHR) or another quest's alias. `ForceRefTo`, `ForceRefIfEmpty` and `Clear`
change them; stopping a quest clears them.

`GlobalVariable` values start at the record's and are saved once scripts
change them. `Game.GetFormFromFile` uses the plugin prefixes in `world.fb`.

Not done, and different from the game:

- Conditions are not evaluated. Aliases filled by conditions ("find
  matching reference") and created references stay empty, required or not,
  and the quest still starts. Of a stage's log entries the first is taken,
  with its fragment.
- `OnInit` goes to a quest's scripts when a property first names it and on
  every start; the game's order may differ. Restarting does not reset script
  variables.
- The story manager does not start quests; nothing evaluates events such as
  `ENAM`.
- Dialogue, scenes and packages, and so their fragments, do not exist.

## Triggers

An attached reference with an XPRM box or sphere becomes a trigger.
`update_actor(actor, position)` (Skyrim space) sends `OnTriggerEnter` and
`OnTriggerLeave` as the actor crosses one, and `GetTriggerObjectCount` counts
who is inside. The viewer moves the player (reference 0x14) with the
camera's feet, 1.7 m below the eye. Box bounds are taken as half extents,
turned and scaled with the reference, and a sphere's radius as its x: xEdit
shows XPRM bounds doubled. Not checked against the game.

## Natives

| Class | Bound |
| --- | --- |
| Debug | `Trace`, `Notification`, `MessageBox` |
| Utility | `Wait`, `GetCurrentRealTime`, `GetCurrentGameTime` (timescale 20), `RandomInt`, `RandomFloat` |
| Game | `GetPlayer` (reference 0x14), `GetForm`, `GetFormFromFile` |
| Form | `GetFormID`, `RegisterForSingleUpdate`, `RegisterForUpdate`, `UnregisterForUpdate` |
| ObjectReference | `Enable`, `Disable`, `IsDisabled`, `IsEnabled`, `Is3DLoaded`, `GetLinkedRef`, `GetBaseObject`, `GetPositionX/Y/Z`, `GetAngleX/Y/Z`, `GetScale`, `GetDistance`, `PlayAnimation`, `PlayAnimationAndWait`, `Add/RemoveDependentAnimatedObjectReference`, `Activate`, `BlockActivation`, `IsActivationBlocked`, `IsFurnitureInUse`, `SetNoFavorAllowed`, `GetOpenState`, `SetOpen`, `Lock`, `IsLocked`, `GetLockLevel`, `GetTriggerObjectCount` |
| EffectShader | `Play`, `Stop` |
| Quest | `Start`, `Stop`, `Reset`, `IsRunning`, `IsStopped`, `IsStarting`, `IsStopping`, `IsCompleted`, `CompleteQuest`, `GetStage`, `GetCurrentStageID`, `SetStage`, `SetCurrentStageID`, `GetStageDone`, `IsStageDone`, `IsActive`, `SetActive`, `GetAlias`, `SetObjectiveDisplayed/Completed/Failed`, `IsObjectiveDisplayed/Completed/Failed`, `CompleteAllObjectives`, `FailAllObjectives`, `UpdateCurrentInstanceGlobal`, `PrepareForReinitializing` |
| Alias | `GetOwningQuest`, `GetID`, `GetName`, `RegisterForSingleUpdate`, `RegisterForUpdate`, `UnregisterForUpdate` |
| ReferenceAlias | `GetReference`, `GetRef`, `GetActorReference`, `GetActorRef`, `ForceRefTo`, `ForceRefIfEmpty`, `Clear` |
| LocationAlias | `GetLocation`, `ForceLocationTo`, `Clear` |
| GlobalVariable | `GetValue`, `SetValue` |

World changes are signals: `enable_changed`, `play_animation`,
`activate_requested`, `open_changed`, `lock_changed`, `effect_shader`,
`message`, `trigger`, and for quests `quest_started`, `quest_stopped`,
`quest_stage`, `objective_changed`. The viewer shows or builds the reference, plays the
animation on its `SkydotAnimator` (whose text keys and finished clips come
back through `notify_animation_event` for `PlayAnimationAndWait`), activates
the target, opens or closes the door, and honours locks and blocked
activation. Everything else logs once per native.

No-ops, because what they control does not exist yet: dependent animated
objects (physics), furniture in use (always false), favours. Effect shaders
are signalled, not drawn.

A probe over Bleak Falls Barrow 01 and 02, Breezehome, the Sleeping Giant,
Dragonsreach and the Blue Palace (184 scripted references, 46 triggers,
`OnInit` and `OnLoad`, then Bleak Falls' pillar-puzzle lever) reaches no
unbound native. Starting vanilla SE's start-game-enabled quests and running
a minute reaches about 40 unbound natives, mostly on actors, form lists and
inventories, which do not exist yet.

## Saving

`save_state()` returns bytes; `load_state(bytes)` replaces everything, in a
VM set up on the same pack. The VM part (`vm/save.cpp`) holds instances with
their variables and states, running and waiting threads with every frame,
timers, pending results, the clock and the random state; `SkydotPapyrus` adds
which references are attached, script-made changes to enabled, open, locked
and blocked state, who is inside which trigger, and actor positions. Loading
treats the bytes as untrusted and refuses a save whose frames do not match
the scripts in the pack. Quests (running, stages done, objectives), alias
handles and fills and changed globals are saved too (save format 2; format 1
still loads). The viewer saves the place and camera with it (F5,
F9, `--save-to`, `--load`).

Not saved: the world's own state outside scripts (none is simulated yet).

## Assumptions not checked against the game

- Floats print with six decimals (`1.500000`), bools as `True`/`False`,
  objects as `[Class <FormID>]`.
- An array is true when it has elements.
- The auto state's `OnBeginState` is not sent on attach.
- `GetLinkedRef(None)` returns the link without a keyword.
- A reference with XLOC starts locked; picking a lock unlocks it for good.
- `PlayAnimationAndWait` gives up after 10 s without its event.

## Not yet

Fragments of dialogue, scenes and packages, condition evaluation, the story
manager, per-object locking between threads, unloading (`Is3DLoaded` stays
true once built), and most of the 499 natives vanilla calls.
