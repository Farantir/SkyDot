# Spike: Papyrus and the AGPL question

**Question:** what would a clean-room Papyrus VM cost, and is that cheaper than
linking skymp's AGPL-3.0 VM?

**Budget:** 4 days · **Took:** ~2 hours · **Result:** go for a clean-room VM,
but not before quests, dialogue and AI exist.

## Approach

The plan said to read skymp's VM and estimate from the public opcode docs.
Instead no skymp code was read: the vanilla scripts were measured directly,
which gives numbers rather than an impression of someone else's design and
keeps the clean-room claim simple.

`papyrus/pex.py` reads PEX from UESP's format description;
`papyrus/pex_census.py` counts opcodes, resolves every call to a native
function, and lists engine events and fragments. Calls are resolved through
the declared types of locals, parameters, object variables and `self`, then
up the class hierarchy; 211 of 130,127 calls (0.16%) stay unresolved.

```sh
bethconv scan <Data>/*.bsa <Data> --list 100000 --filter .pex   # -> pex-list.txt
bethconv extract --source ... --from pex-list.txt -o pex/
python3 docs/spikes/papyrus/pex_census.py pex/                  # ~5 s
```

## Results (vanilla SE with DLC and the four CC plugins)

| | |
| --- | --- |
| Scripts | 14,302, all parsed with no bytes left over |
| Classes declaring natives | 44 (Actor, ObjectReference, Game, Debug, Utility, Quest, ...) |
| Natives declared / called | 686 / 499 |
| Instructions (outside the 44) | 310,078 |
| Opcodes | 36 defined, 34 used (`array_rfindelement` and `nop` never) |
| Calls | 65,351 to natives, 64,565 to script functions |
| Engine events | 97 declared on native classes, 70 handled by vanilla |
| Properties | 44,199, of which 43,928 auto (values come from VMAD) |
| Functions in named states | 2,021 |

Opcode mix: `callmethod` 119,078, `cast` 48,032, `assign` 43,152, jumps
39,463, `return` 16,323, `callstatic` 10,962, property get/set 10,348;
arithmetic and arrays are rare (arrays 1,045 in total).

**Native surface.** Call sites are concentrated:

| Most-called natives | Share of call sites | Scripts calling no other native directly |
| ---: | ---: | --- |
| 12 | 50% | 8,287 of 14,258 (2,764 not fragments) |
| 55 | 80% | 11,274 (4,490) |
| 110 | 90% | 12,426 (5,176) |
| 172 | 95% | 13,161 (5,625) |
| 305 | 99% | 13,959 (6,177) |
| 499 | 100% | 14,258 (6,427) |

The top twelve: `TopicInfo.GetOwningQuest` 6,494, `Game.GetPlayer` 5,815,
`ReferenceAlias.GetReference` 4,525, `Quest.SetObjectiveDisplayed` 2,240,
`ObjectReference.Disable` 2,153, `Utility.Wait` 2,093,
`Quest.SetObjectiveCompleted` 2,036, `ObjectReference.Enable` 2,006,
`GetLinkedRef` 1,532, `AddItem` 1,373, `MoveTo` 1,288,
`Actor.EvaluatePackage` 1,281. Five of those need quests, aliases, dialogue or
AI packages.

**Fragments.** 7,831 of the 14,258 non-native scripts (55%) are fragments:
5,958 dialogue (`TIF__`), 919 quest stage (`QF_`), 588 scene (`SF_`), 356
package (`PF_`), 10 perk. They only run once the system that owns them exists.

**Events.** Most-handled: `OnActivate` 1,024, `OnTriggerEnter` 643, `OnLoad`
441, `OnDeath` 370, `OnUpdate` 315, `OnHit` 269, `OnEffectStart` 236.

## What the VM needs

- **Execution model.** Named registers per frame (parameters, locals,
  `::temp` variables), not an operand stack. An explicit frame stack rather
  than native recursion, so a frame can suspend.
- **Latent calls.** `Utility.Wait` alone has 2,093 call sites, and animation
  and movement calls also block, so suspendable frames and a scheduler are
  core, not an extra.
- **Concurrency.** Papyrus runs many script threads and locks per object; a
  deterministic cooperative scheduler with the same observable ordering is
  the cheaper and more testable target.
- **Binding data.** Scripts attach to references, base forms, aliases, active
  effects and fragments through VMAD, which bethconv keeps verbatim and never
  parses (7,668 REFR records alone). Decoding it, and carrying it in the pack,
  is a prerequisite.
- **Save state.** Suspended frames and script variables have to survive a
  save.

## Estimate

| Part | FTE-wk |
| --- | ---: |
| PEX loader through SpanReader, corpus test over all scripts | 0.5–1 |
| Values, 36 opcodes, casts, strings, arrays | 1.5–2 |
| Objects: instances on forms/aliases/effects, inheritance, states, auto properties | 2 |
| VMAD decoding in bethconv and the pack | 1.5–2 |
| Scheduler: suspendable frames, events, update timers, locking semantics | 2–3 |
| Saving and restoring VM state | 1.5–2 |
| **VM core** | **9–12** |

That sits inside the plan's 8–16. The native surface is the real cost, and
it is the same under either licence. Seven of the twelve most-called natives
are thin wrappers over things the engine will have anyway (enable/disable,
move, items, linked refs, timers); the rest stand for whole subsystems
(quests and aliases, dialogue, scenes, AI packages, combat, the story
manager).

## Licence

- skymp's VM: AGPL-3.0 (per the earlier licence review). Linking it puts an
  AGPL floor under the whole engine. Not read for this spike.
- Caprica (Papyrus compiler): MIT. Usable as a reference for what the compiler
  emits, and as a way to build test scripts.
- Champollion (PEX decompiler): LGPL-3.0. Fine to read; nothing needs linking.
- The PEX format is documented on UESP. Native signatures (names, parameter
  names and types) are in the game's own PEX files, so the engine can read the
  API surface from the player's install rather than from Bethesda's `.psc`
  sources.

## Recommendation

Go for the clean-room VM: 9–12 weeks is modest next to the native surface,
and it keeps the engine GPL-3.0. Do not start it yet. The first useful slice
is world mechanics (doors, levers, traps, triggers: `OnActivate`,
`OnTriggerEnter`, `OnLoad`), which needs the VM core and VMAD. With the 55
most-called natives, 4,490 of the 6,427 non-fragment scripts call no other
native directly (script-to-script calls not followed), so the slice is real,
but it only pays off once cells stream and references can animate.

## Not measured

- Which natives are latent: PEX has no flag for it; it comes from the
  documentation.
- Mods: the FUS list's scripts, and SKSE's natives, which are not in vanilla.
- Transitive coverage: the table counts direct native calls only.
