# Papyrus

The VM (`extension/src/vm/`) runs the script assets bethconv decodes from
`.pex` (`../formats/pack-format.md`, "Script assets"). It is written from
the PEX format and bethconv's measurements (`docs/spikes/papyrus.md` there); no
other VM's code was read, so it stays GPL-3.0.

## Pieces

| File | Role |
| --- | --- |
| `vm/script_class.*` | One script: verified, indices checked, identifiers resolved to registers, variables, `self` or `::State` once at load |
| `vm/value.hpp` | Values: 24 bytes, a kind and what it needs; arrays and strings are shared, everything else copied |
| `vm/name.hpp` | Names with their hash, and the hash table functions and properties are found in |
| `vm/vm.*` | Instances, threads, the interpreter, the scheduler, timers, native binding |
| `vm/save.cpp` | The VM's save format |
| `vm/papyrus.*` | `SkydotPapyrus`: the VM bound to a pack and `SkydotWorld`, triggers, saves; `bind_natives` registers the natives from one table |
| `vm/natives_<class>.cpp` | The natives of one class each (`debug`, `utility`, `game`, `form`, `objectreference`, `effectshader`, `weapon`, `sound`, `quest`, `spell`, `magiceffect`, `activemagiceffect`), the intended no-ops (`noop`), and `world`, written against `WorldAccess` |
| `vm/world_access.hpp` | `WorldAccess`: typed questions natives ask the world (base form, linked ref, player level), so a native needs no Dictionary and a test no pack |
| `vm/census.cpp` | `Vm::census`: which natives a set of scripts call |
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
- `RegisterForAnimationEvent(sender, event)` (false unless the sender's 3D is loaded) keeps one
  registration per script, sender and event (any case); `OnAnimationEvent(akSource, asEventName)`
  goes to the registered scripts when `notify_animation_event` gets the event: a behaviour graph's
  event, a NIF text key, or an actor's clip annotation (`SkydotActor.animation_event`, from the
  Animation's markers: the clip file's and the behaviour project's). Registrations go with their
  script's reference (`Vm::detach`), are dropped when the sender's cell unloads and are saved
  (`AEVT`). Each script class and event logs once; `get_animation_event_count` counts deliveries.
- `RegisterForSingleUpdate`/`RegisterForUpdate` set one timer per script;
  when it is due, OnUpdate goes to that script alone.
- Names are hashed once: a script's when it loads, an instance's state when
  it is set (kept as the script wrote it, which `GetState` returns, with a
  lowercase key beside it). A `callstatic` instruction remembers what it
  resolved to and a native its binding; operands are read in place, not
  copied.
- Runtime errors (None objects, bad indices, division by zero, missing
  functions, unbound natives) are logged and give None or 0; the script
  continues, as in the game. An unbound native gives the default of its
  declared return type (0, 0.0, False, "", None for objects and arrays). Missing `On...` functions are not errors.
- Int arithmetic wraps at 32 bits, as the game's native VM does: `INT_MAX + 1`
  is `INT_MIN`, `INT_MIN / -1` is `INT_MIN` and `INT_MIN % -1` is 0.

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

## Scenes

`scenes/scenes.hpp` runs SCEN records (rules in its header). Natives: `Scene.Start`,
`ForceStart`, `Stop`, `IsPlaying`, `GetOwningQuest`, `IsActionComplete(index)`.
The SF_ fragment script attaches to the scene's form. The game layer calls
`scenes_update(dt)`, speaks the returned dialogue actions and answers
`scene_action_done`; a package action emits `scene_package(actor, package, push)`.
Save section `SCEN` v1 (optional). Scene and phase conditions use the player as
subject (an assumption); IsSceneActionComplete (550) reads the running scene.
A scene a script starts while an actor of it is not built waits like one that
starts with its quest, and after 20 s (`Scenes::k_grace`, **guess**: Skyrim
runs unloaded actors in low process) begins with whoever is built; the others
say nothing (MQ104's FortIntroScene, its soldiers far off).

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
| Utility | `Wait`, `GetCurrentRealTime`, `GetCurrentGameTime` (the game clock, SkydotClock), `RandomInt`, `RandomFloat` |
| Game | `GetPlayer` (reference 0x14), `GetForm`, `GetFormFromFile` |
| Form | `GetFormID`, `HasKeyword` (a reference answers for its base's KWDA; only bases that world.fb has, i.e. with a model or scripts, carry keywords), `RegisterForSingleUpdate`, `RegisterForUpdate`, `UnregisterForUpdate` |
| ObjectReference | `Enable`, `Disable`, `EnableNoWait`, `DisableNoWait` (no fade), `Reset` (signal `ref_moved`: position, rotation), `GetParentCell`, `PlaceAtMe`, `Delete`, `DeleteWhenAble`, `DamageObject`, `ClearDestruction`, `SetDestroyed`, `IsDestroyed`, `GetCurrentDestructionStage`, `IsDisabled`, `IsEnabled`, `Is3DLoaded`, `GetLinkedRef`, `GetNthLinkedRef`, `CalculateEncounterLevel` (the player's level, 1 until stats exist), `GetBaseObject`, `GetPositionX/Y/Z`, `GetAngleX/Y/Z`, `GetScale`, `GetDistance`, `PlayAnimation`, `PlayAnimationAndWait`, `Add/RemoveDependentAnimatedObjectReference`, `ApplyHavokImpulse`, `SetMotionType`, `Activate`, `BlockActivation`, `IsActivationBlocked`, `IsFurnitureInUse`, `SetNoFavorAllowed`, `GetOpenState`, `SetOpen`, `Lock`, `IsLocked`, `GetLockLevel`, `GetTriggerObjectCount`, `GetItemCount`, `AddItem`, `RemoveItem` (the owner's start inventory is resolved first; an item that runs out is taken off) |
| Actor | `IsDead`, `IsInCombat`, `GetCombatState` (0 none, 1 combat, 2 searching), `GetCombatTarget`, `StartCombat`, `StopCombat` (answered by `SkydotAi`'s combat, `ai/ai_combat.cpp`; `OnCombatStateChanged(akTarget, aeCombatState)` goes to the actor's scripts), the actor value natives, `EquipItem` (adds the item if missing; potions, food, ingredients and scrolls are consumed with a log line, books read with one, neither does anything yet), `UnequipItem`, `UnequipAll`, `IsEquipped`, `GetEquippedItemType` (hand 0 left, 1 right: 0 none, 1 sword, 2 dagger, 3 axe, 4 mace, 5 two-handed sword, 6 two-handed axe, 7 bow, 8 staff, 10 shield, 11 torch, 12 crossbow), `GetEquippedWeapon`, `GetEquippedShield` (rules: `vm/equipment.hpp`) |
| Actor, ObjectReference (flight) | `IsFlying`, `GetFlyingState` (0 not flying, 1 taking off, 2 cruising, 3 hovering, 4 landing or crashing; the node's flight mover answers through `SkydotAi`), `CanFlyHere` (an actor that can fly, outside), `SetAllowFlying(abAllowed = true)` / `SetAllowFlyingEx`, `IsAllowedToFly`, `SetForcedLandingMarker(ObjectReference)` (None clears; the dragon lands there at once, and at later landings), `ObjectReference.GetLength` (the actor's bounding length in game units, 0 for anything else). The allowed flag (default true) and the marker are per actor in the ACTV section (version 3; versions 1 and 2 still load) and are read by the flight tactics (`ai/flight.hpp`) |
| Keyword | `GetKeyword` (by editor id, any case; KYWD table of world.fb) |
| FormList | `GetSize`, `GetAt`, `Find`, `HasForm` (the FLST's forms, then those added; nested lists are not looked into), `AddForm`, `RemoveAddedForm`, `Revert` (the additions are `Vm::form_lists`, saved with the VM) |
| Message | `Show` (MESG text, `%.0f`-style placeholders filled from the arguments; signal `message`; returns 0, the first button), `ShowAsHelpMessage` (as a notification) |
| Cell | `IsInterior`, `IsAttached` (its references were handed to the VM; a cell never detaches) |
| EffectShader | `Play`, `Stop` |
| Sound | `Play` (signal `play_sound`: sound, source reference, instance id; the game layer plays it, `game/game/sounds.gd`), `PlayAndWait` (waits for `notify_sound_finished(instance)`, which the game layer sends when the clip ends, or 30 s), `StopInstance` (signal `stop_sound`), `SetInstanceVolume` (signal `sound_volume`) |
| Weapon | `Fire` (signal `fire_weapon`: the game layer launches the ammo's projectile, `game/game/projectiles.gd`) |
| Quest | `Start`, `Stop`, `Reset`, `IsRunning`, `IsStopped`, `IsStarting`, `IsStopping`, `IsCompleted`, `CompleteQuest`, `GetStage`, `GetCurrentStageID`, `SetStage`, `SetCurrentStageID`, `GetStageDone`, `IsStageDone`, `IsActive`, `SetActive`, `GetAlias`, `SetObjectiveDisplayed/Completed/Failed`, `IsObjectiveDisplayed/Completed/Failed`, `CompleteAllObjectives`, `FailAllObjectives`, `UpdateCurrentInstanceGlobal`, `PrepareForReinitializing` |
| Alias | `GetOwningQuest`, `GetID`, `GetName`, `RegisterForSingleUpdate`, `RegisterForUpdate`, `UnregisterForUpdate` |
| ReferenceAlias | `GetReference`, `GetRef`, `GetActorReference`, `GetActorRef`, `ForceRefTo`, `ForceRefIfEmpty`, `Clear` |
| LocationAlias | `GetLocation`, `ForceLocationTo`, `Clear` |
| GlobalVariable | `GetValue`, `SetValue` |

Activation is decided in the engine: `activate_ref(ref, activator,
default_only, source, force)` (source player, script or parent) applies the
rules of `vm/activation.hpp`, runs OnActivate with the real activator unless
`default_only`, schedules the activate children on the VM's clock (the parent
is their activator, as trap scripts expect; saved with
it) unless a script blocks activation, and returns an outcome the host
applies (door, furniture, toggle_open, or a note). A script's `Activate` and
the children arrive as `activation_outcome`.

World changes are signals: `enable_changed`, `ref_moved`, `ref_placed`, `ref_deleted`, `destruction_model`, `play_animation`,
`activation_outcome`, `open_changed`, `lock_changed`, `effect_shader`,
`message`, `trigger`, `havok_impulse`, `fire_weapon`, `play_sound`, `stop_sound`, `sound_volume`, `motion_type_changed`, and for
quests `quest_started`, `quest_stopped`, `quest_stage`, `objective_changed`.
The viewer shows or builds the reference, plays the animation on its
`SkydotAnimator` (whose text keys and finished clips come back through
`notify_animation_event` for `PlayAnimationAndWait`), activates the target,
opens or closes the door, honours locks and blocked activation, and pushes,
releases or holds clutter (`physics.md`). Everything else logs once per native.

No-ops, because what they control does not exist yet: dependent animated
objects (constraints), furniture in use (always false), favours. Effect shaders
are signalled, not drawn.

Adding a native: bind it in `vm/natives_<class>.cpp` (a member of
`SkydotPapyrus` when it changes the world or reads the Dictionaries) or, for
one that only asks the world a question, in `vm/natives_world.cpp` against
`WorldAccess`, which a unit test can fake. Register a new file in the table
in `SkydotPapyrus::bind_natives`. A native nobody bound warns once, by name,
and gives the default of its return type. These are bound as silent no-ops
(`vm/natives_noop.cpp`), because nothing in the engine raises what they
register for or shows what they do:

- `RegisterForSleep`, `RegisterForLOS`, `RegisterForSingleLOSGain/Lost`,
  `RegisterForTrackedStatsEvent` and their `Unregister...` (on `Form`,
  `Alias`, `ActiveMagicEffect`): no sleeping, line of sight or stats events.
- `Keyword.SendStoryEvent(AndWait)`: no story manager; returns False.
- `ObjectReference.SetActorCause`: no blame or crime until combat.
- `Game.ShakeCamera`, `ShakeController`, `AddAchievement`, `IncrementStat`.

Not on the list, though they are registrations: `RegisterForAnimationEvent`
(actors' events will come) and the `...GameTime` timers.

A probe over Bleak Falls Barrow 01 and 02, Breezehome, the Sleeping Giant,
Dragonsreach and the Blue Palace (184 scripted references, 46 triggers,
`OnInit` and `OnLoad`, then Bleak Falls' pillar-puzzle lever) reaches no
unbound native. Starting vanilla SE's start-game-enabled quests and running
a minute reaches about 40 unbound natives, mostly on actors and
inventories, which do not exist yet.

### Run-time references and destruction

`PlaceAtMe(form, count, persist, disabled)` makes `count` references (at most
64) of `form` at the caller's position, rotation and scale and in its cell,
and returns the last. Their ids are runtime ids from 0xFF001000. The VM keeps
them (`Vm::placed`, `vm/runtime_refs.hpp`; base, cell, transform) and the
base's scripts attach to them, OnInit at once and OnLoad (with OnCellAttach,
OnCellLoad) when the game layer reports the node built (`ref_built`).
`ref_placed(ref, base, cell, position, rotation, scale, disabled)` makes
`game/game/placed.gd` build the node from the base's model with
`SkydotWorld.build_model`, tag it for `find_ref_node` and report it built.
`Delete` and `DeleteWhenAble` remove a placed reference (`ref_deleted`); on a
reference from the editor they only disable it. Persist is ignored; placed
actors have no model, so nothing is drawn for them (warned once).

An EXPL (world.fb format 23) as `form` is an explosion: a transient reference
that is not saved. The VM plays the record's sound at it (`play_sound`), the
game layer plays the model's controllers and particles once, stops the
emitters after 1.5 s and has the VM remove the reference (`remove_placed`)
after 5 s. That life is fixed: the animator does not report how long its
controllers run. No damage, force, light, impact data or placed object yet.
Placed references are saved with the VM (save version 6; version 5 still
loads) and built again when a save loads; `load_state` removes the old ones.

Destruction uses the base's DEST. `DamageObject(d)` takes health off (it
never goes below 0); the stage number is the count of stages opened, 0 when
intact, and a stage opens when the remaining health has fallen to its
`health_percent`. Each stage opened by a blow takes effect in order: its
model replaces the reference's (`destruction_model(ref, model, stage)`; the
game layer swaps the node), its explosion is placed, flag 2 disables the
reference and flag 4 marks it destroyed (cap damage and ignore external
damage are not applied). Then the scripts get
`OnDestructionStageChanged(old, current)`. `ClearDestruction` (and `Reset`)
forget the damage and bring back the base's model. State is saved with the VM
(`Vm::destruction`). Self-damage per second and debris are not applied.

## Items

Equip events: what really changes in a hand, the ammo slot or the worn set,
and a spell or shout put in a hand or the voice slot, sends
`OnObjectEquipped`/`OnObjectUnequipped(base, None)` to the actor and its
aliases; a cast (hands, power, scroll, a shout with its word spell; a
concentration spell once) sends `OnSpellCast(spell)` (MQ104 92 and 100).

A container's or an actor's start inventory is resolved the first time
anything looks at it (`resolve`, `inventory`, `transfer`, a script's
`GetItemCount`, `AddItem`, `RemoveItem`, a condition's item count): a
container's CNTO, an actor's CNTO (through a template that supplies the
inventory) and its default outfit, which it wears. Leveled lists in them are
drawn by `data/leveled.hpp` (splitmix64 seeded from the reference, so a
reference always holds the same, and the actor builder draws its outfit with
the same seed); the player's level is 1 until stats exist. A placed leveled
actor list picks its actor once and the pick is kept. What is held, worn and
resolved is saved (ITST). `item_info(form)`, `inventory(owner)`,
`transfer(from, to, form, count[, extra])`, `equip(owner, form[, extra])`,
`unequip(owner, form[, extra])` and `equipment(owner)` are for the game layer;
the rules are in `vm/equipment.hpp`. An actor built after a script equipped
something shows it (`SkydotWorld.set_worn_provider`).

Units with extra data (tempered, enchanted; `vm/item_extras.hpp`, ADR 0007)
are rows of their own with an `extra` id (-1 as an argument: any, plain
first); potions that alchemy made are created forms
(`vm/created_forms.hpp`, ids 0xFF700000 up; sections CRFT, and ITST version
3). Crafting (`vm/crafting.hpp`): `recipes(bench)`, `craft(recipe, count)`,
`temper_rows(bench)`, `temper(recipe, form, extra)`, `enchant_rows()` (a
stub), `alchemy_rows()`, `alchemy_preview(forms)`, `alchemy_mix(forms)`.
Activating a FURN with a bench type gives the outcome kind `crafting`
(`bench_type`); activating a FLOR or TREE with a harvest gives `harvest`
(`harvest_form`, `harvest_count`, `harvest_name`, `harvest_sound`), then
"already harvested" until 10 game days pass (`vm/harvest.hpp`, section HRVS;
`harvested(ref)` for the prompt).

## Census

`game/tools/papyrus_census.gd` reads the code of every script in a pack
(nothing runs) and lists the natives they call with the number of call
instructions that reach each. The callee is found from the declared type of
the object, so `actor.GetLinkedRef()` counts for `ObjectReference`. Each
native is bound, ignored (a silent no-op) or unbound.

```
godot4.7 --headless --path game --script res://tools/papyrus_census.gd -- \
    --pack <pack> [--top 40] [--all on]
```

`--all on` adds a table of every native called. Rerun it after binding
natives and update the numbers below.

Skyrim SE pack, 14,325 scripts (2026-10-18, after the magic natives):

| | Natives | Call sites |
| --- | ---: | ---: |
| Called | 549 | 65,685 |
| Bound | 156 | 53,181 |
| Ignored (no-op) | 19 | 499 |
| Unbound | 374 | 12,005 |

Unbound natives with most call sites (movement and actor state, which are
the work still to come):

| Native | Call sites |
| --- | ---: |
| `ObjectReference.MoveTo` | 1,291 |
| `Actor.EvaluatePackage` | 1,282 |
| `Actor.RemoveFromFaction` | 536 |
| `Actor.PlayIdle` | 327 |
| `Actor.Kill` | 287 |
| `ObjectReference.AddToMap` | 287 |
| `Actor.IsInFaction` | 285 |
| `Actor.SetRelationshipRank` | 272 |
| `Actor.SetGhost` | 222 |
| `VisualEffect.Play` | 216 |

## Magic

Spells, effects and potions run in `magic/` (docs/adr/0008-active-magic-effects.md);
`vm/papyrus_magic.cpp` is the glue. The natives are Godot-free and call the game
through `MagicApi` (`vm/natives_magic.hpp`): `Spell` (`natives_spell.cpp`: Cast,
RemoteCast, GetMagickaCost, GetEffectiveMagickaCost, GetCastTime, IsHostile, the
`GetNthEffect...` family, GetPerk), `MagicEffect` (`natives_magiceffect.cpp`:
GetAssociatedSkill, GetResistance, casting and delivery types, base cost, skill level,
the forms the record names), `ActiveMagicEffect` (`natives_activemagiceffect.cpp`:
GetTargetActor, GetCasterActor, GetBaseObject, Dispel, GetMagnitude, GetDuration,
GetTimeElapsed) and, in `natives_actor.cpp`, `Actor.AddSpell` (with a message "<Spell> added"
for the player), RemoveSpell, HasSpell, DispelSpell, DispelAllSpells, HasMagicEffect,
HasMagicEffectWithKeyword, DoCombatSpellApply, EquipSpell (aiSource 0 left, 1 right, 2 the
power; the voice slot is shared with a shout), UnequipSpell and GetEquippedSpell, and the shouts
(`magic/shouts`): `Actor.AddShout`, RemoveShout, EquipShout, UnequipShout, GetEquippedShout and (SKSE)
SetVoiceRecoveryTime / GetVoiceRecoveryTime, `Game.TeachWord` (a message "Word of Power Learned: <word>"
for the player), UnlockWord, IsWordUnlocked, and (SKSE, `bind_shout_natives`) `Shout.GetNthWordOfPower`,
GetNthSpell, GetNthRecoveryTime. The game layer's calls are `shout_info`, `shouts_of`, `equipped_shout`,
`equip_shout`, `teach_word`, `unlock_word`, `unlock_word_with_soul`, `voice_recovery`,
`set_voice_recovery` and `cast_shout`.

An effect whose MGEF has scripts gets a script object of its own: its handle is the effect's
instance id (`id_ranges::effect_instances`), it is sent `OnEffectStart(akTarget, akCaster)` when
the effect starts acting and `OnEffectFinish` when it ends, and detached two updates later. The
game layer's calls are `cast_spell(caster, spell, target[, seconds])` (pays the cost, checks a
power's cooldown), `spell_cost`, `spend_magicka`, `apply_item_effects(actor, form[, extra])`
(`equip` of a potion, food or ingredient calls it), `spells_of`, `equip_spell`, `equipped_spell`,
`active_effects` and the signal `magic_effect_changed(target, mgef, archetype, started, instance)`.
The optional save section MAGC (version 2; version 1 still reads, with no shouts) holds the running
effects (abilities are derived again), the spells added and removed, the equipped spells, the power
cooldowns and the known enchantments, and in a third part the ShoutBook (words known and unlocked,
shouts added and removed, the voice slot, the recovery left); the temporary modifiers effects hold are not in ACTV, the effects hold them again
on load.

## Saving

A save is a sectioned file (`extension/src/save/`, docs/adr/0002-game-state-saves-and-session.md):
magic `SKYS`, sections (FourCC, version, length, bytes), a CRC-32 trailer.
`SkydotPapyrus.write_sections(writer)` adds HEAD (engine version, the pack's
world hash and load order), the VM's VMST (instances with their variables
and states, running and waiting threads with every frame, timers, pending
results, scheduled host calls, the clock and the random state), INVT, MAGC (optional: effects, spells, hands, cooldowns, shouts; version 2), ITST (optional: resolved owners, leveled actor picks, equipment, item extras; an old save without it has every inventory unresolved and resolves it on touch), ANIM
(behaviour graph snapshots), FLST (forms added to lists), PLCD (placed
references) and DEST (destruction), then RFST (what scripts changed per
reference: enabled, open, locked, blocked, moved), PAPY (attached references,
who is inside which trigger, actor positions) and QUST (quests, alias handles
and fills, globals). The viewer's SaveService adds PLYR (the place and the
camera) and writes through a temporary file and a rename (F5, F9, `--save-to`,
`--load`).

Loading is two phases. `prepare_load(reader)` checks HEAD against the open
pack (a save of another pack or load order is refused) and decodes every
section into new objects, treating the bytes as untrusted and refusing a save
whose frames do not match the scripts in the pack; the running game is not
touched, whatever the outcome. `commit_load()` then swaps it in. Sections have
versions of their own, a newer one is refused, unknown tags are skipped, and
there is no compatibility with saves of 0.1.0 and older. `save_state()` and
`load_state(bytes)` do both steps on a file's bytes, for tests.

Behaviour graphs (`SkydotBehavior`, a gate or lever) are saved as opaque bytes
per reference (`set_animation_state`, `get_animation_state`; ANIM). The bytes
are `BehaviorSet::save()`: per machine whether it runs, its state id, clip
time, held pose and fired triggers. The viewer's `ScriptBridge` restores a
built reference's graph silently (no events) before its scripts' OnLoad, and
records it when it changes, when its cell unloads and before a save, so an
opened gate stays open after F9 and after leaving the cell and coming back.
A snapshot that does not fit the graph is ignored and the graph starts over.

Deleting a placed reference (`Delete`, a finished explosion) goes through
`forget_ref`, which drops its scripts, timers, running threads, scheduled
calls, inventory, animation, destruction and script-made state.

Not saved yet: AI actors and live projectiles and sounds (AIAC, PROJ later), the world's own state outside scripts (none is simulated yet).

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
