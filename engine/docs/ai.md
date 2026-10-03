# AI packages

How placed actors spend their day: `world/packages.*` reads AI packages
(PACK) from `world.fb` (format 9, see `formats/pack-format.md`), and
`SkydotAi` (`world/ai.*`) runs them against a game clock. Walking itself is
`SkydotActor` (`actors.md`).

## Which package runs

An actor's packages are its NPC_'s PKID list, then its default package list
(DPLT, an FLST), each taken from the NPC_ that the template flags "AI
packages" (0x20) and "default package list" (0x400) lead to. The first that
passes runs:

- **Schedule** (PSDT). Month and date, or -1/0 for any. Day of week: 0
  Sundas … 6 Loredas, 7 weekdays, 8 weekends, 9 Morndas/Middas/Fredas, 10
  Tirdas/Turdas. With an hour, the package covers `duration` minutes from
  then (an hour when 0), possibly past midnight; without one, any time.
- **Conditions** (CTDA), in order, a condition with the OR flag (0x01) joined
  with the next; the runs of ORs are ANDed. GLOB comparison values (0x04)
  are read from the scripts' globals.
- **Not just completed.** A package whose steps are all done (and whose tree
  does not repeat) is skipped for the rest of its window, so the next one
  takes over: Sigrid's "walk to the garden at 8" (duration 0, must complete)
  gives way to hoeing the garden once she is there.

Packages given by quest aliases (ALPC) and scenes are not read yet: an actor
does what its NPC_ says, not what a running quest wants of it.

Packages are chosen again every 1.5 s for built actors, and every five game
minutes for the others.

## What a package does

A package (PKDT type 18) runs its template's (type 19) procedure tree with its
own data inputs over the template's, matched by key (UNAM). When the package
starts, the tree is flattened into steps:

- **Sequence:** its children in order. **Stacked:** the first child whose
  conditions pass. **Random:** one child at random (seeded per actor).
  **Simultaneous:** Finds first, then the first procedure that moves or
  stays, for as long as the Waits beside it say.
- Branch conditions are evaluated then, once; GetNumericPackageData (612)
  reads the package's own inputs, which is how Sandbox runs its Travel and
  UnlockDoors only when "Unlock On Arrival?" is set.

A step's place comes from its procedure's first Location input (PLDT):
near a reference, in a cell (by its door), near the editor location (where
the actor was placed), near the actor's linked reference with a keyword,
a quest alias's reference; anything else is where the actor is. Its target
is the first SingleRef/TargetSelector input (PTDA: a reference, the nearest
object of a base or type, a linked reference, an alias, itself) or what a
Find put in an ObjectList input.

| Procedure | Built actor |
| --- | --- |
| Travel | walks there (within the radius, at least 96 units); done on arrival |
| Sandbox, Wander, Guard | walks there, then wanders within the radius (at least 256 units) |
| Sleep, Sit, Eat, UseIdleMarker, Activate, HoldPosition | walks to the bed, chair, marker or place and stands there |
| Patrol | walks from the start marker along its linked references (no keyword), round again |
| Find | the nearest bed (FURN named "bed"), chair ("chair", "bench", "stool", "throne", "seat"), furniture or base within the radius, for the steps after it |
| UnlockDoors, LockDoors | the load doors of the place's interior (outside: those within its radius + 1024 units); only doors with a lock are locked |
| Wait | stands for its seconds |
| anything else (Follow, Escort, ForceGreet, UseWeapon, …) | stands |

Run speed: when the package's "preferred speed" flag (0x2000) is set and its
speed is Run; otherwise actors walk.

## Moving between places

The viewer builds one space at a time: an interior, or a worldspace's cells
around the camera.

- **Built actors** whose step leads to another space walk to the first load
  door of the shortest door route there (spaces are interiors and
  worldspaces; up to six doors), go through it (`actor_left`), and are gone
  from this space. Doors on the way are not opened; only persistent actors
  leave their space.
- **Actors not built** are where their package says: `place_actors` (and,
  a few at a time, `update`) puts each persistent actor at the place of the
  last step of its plan that has one (a sandbox: somewhere within half its
  radius; snapped to the navmesh). Cells built afterwards build it there
  (`SkydotWorld.set_actor_place`). An actor whose package brings it into the
  space on screen from elsewhere appears at the door it comes through
  (`actor_arrived`; the viewer builds it) and walks on.
- A built actor whose cell is freed stays where it was (or where its walk
  was going).

A pass runs over every persistent actor with packages (1,771 in SE with
the CC plugins; about 75 ms in a debug build, the first 470 ms) before a
space's cells are built. `place_actors` does it at once. The viewer instead
calls `begin_placing` when it starts preparing what is behind a load door,
which does the pass a slice per `update` (PLACE_BUDGET_USEC), and
`settle_actors` on arrival: that finishes a pass still under way, does
nothing if one started less than two game minutes ago, and otherwise
places everyone as `place_actors`. Going through a prepared door then
costs no pass at all.

## Doors

Packages lock and unlock load doors through SkydotPapyrus, which keeps the
lock state (both doors of a pair share it):

- the "unlock doors at package start" (0x40) and "at end" (0x80) flags, for
  the space the actor is in then;
- UnlockDoors and LockDoors steps, for their place.

For actors not built, both happen when their package starts; for an actor
that leaves through a door, the rest of its package's lock steps happen as
it leaves. During a placement pass the changes are gathered and unlocking
wins, since the residents of one house may run different packages.

## Conditions answered

GetIsID, GetIsReference, GetInFaction, GetFactionRank, GetIsRace, GetIsSex,
GetStage, GetStageDone, GetQuestRunning, GetQuestCompleted, GetGlobalValue,
GetVMQuestVariable (through the quest's scripts), HasLinkedRef, GetDisabled,
GetRandomPercent, GetCurrentTime, GetDayOfWeek, GetIsCurrentPackage,
IsInInterior, GetInCell, GetInWorldspace, GetDistance, and
GetNumericPackageData inside a tree. Every other function is 0, which is
what most "is it so?" functions are by default (GetDead, GetSleeping,
IsInCombat, GetPlayerTeammate). GetActorValue is 0 as well, so packages
gated on Variable01-10 behave as for an actor no script has touched.

A fresh viewer session has no story progress: quest-gated packages run as
before the player first arrives. Sven guards Riverwood all day
(SvenRiverwoodIntroPatrol, until MQ102 stage 50 or the Riverwood intro
scene's stage 10 is done); `--set-stage MQ102:50` lets him go about his day.

## Guesses to check against the game

- The game starts on a Morndas (`GetDayOfWeek` in game at the start).
- A start hour with duration 0 covers an hour (9 vanilla packages, all
  must-complete travels).
- A completed package stays done for its window (or an hour without one).
- Unloaded actors move instantly when their package changes, rather than
  walking there in the game's low process.

## Checking

```sh
godot4.7 --headless --path game --script res://tools/package_check.gd -- \
    --pack <pack> --npcs Alvor,Sigrid,Faendal --list on [--hours 0,6,8,12,18,22]
godot4.7 --headless --path game --script res://tools/ai_run.gd -- \
    --pack <pack> --cell RiverwoodAlvorsHouse --time 7.95 [--time-scale 60] [--seconds 60]
godot4.7 --headless --path game --script res://tools/ai_run.gd -- \
    --pack <pack> --world Tamriel --at 20586,-45138 --time 7.95
```

`package_check.gd` prints NPCs' packages with their schedules and
conditions (and what each condition evaluates to), then for each hour the
package chosen, its steps and where it sends them. `ai_run.gd` builds a place
with collision, runs the clock and prints arrivals, departures, locks and
every few seconds each actor's package, step and position. In the viewer, I
tells what the nearest actor is doing; `--ai off` turns packages off.
`smoke_ai` covers the test pack's NPC: home by night, out through the door
by day, unlocking the door outside.

Measured on vanilla SE (`ai_run.gd`, Riverwood, 07:57 to 08:30 at 60x): at
08:00 Alvor and Sigrid come out of Alvor's house, Hod and Dorthe (who
plays with Frodnar) out of Gerdur's; Alvor works at the forge, Sigrid walks to the
garden and then hoes it, Hod walks to the sawmill, Gerdur works, the
children play, the guards guard, chickens and the cow sandbox. Inside
Alvor's house at 08:00 Alvor and Sigrid leave through the front door.

## Not done

- Quest alias packages (ALPC), scene packages, combat and dialogue
  interruptions, package OnBegin/OnEnd idles and fragments (the VMAD is kept
  raw).
- Furniture: actors stand next to beds and chairs instead of using them
  (furniture markers and entry animations are next).
- Doors on the way are not opened; actors pass through closed doors inside
  a space as the navmesh allows. Load doors are used without the door
  animation.
- Follow, Escort, ForceGreet, KeepAnEyeOn, Shout, UseWeapon, Flee, Acquire,
  CarryAndDropItem's carrying: the actor stands.
- Location aliases, "object id/type" locations, GetInCurrentLoc and other
  location (LCTN) conditions: no cell-to-location mapping yet.
- Horses (RideHorseIfPossible), swimming, preferred paths.
