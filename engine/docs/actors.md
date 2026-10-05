# Actors

How the engine builds a character from a pack. The converter's side (Havok
files, the `.animfb` asset) is in `../converter/docs/spikes/hkx.md` and
`../formats/schema/animation.fbs`.

## Skeleton, body and clips (`SkydotAnimation`)

- `build_skeleton(asset)`: the asset's first skeleton (e.g.
  `meshes/actors/character/character assets/skeleton.hkx`) as a Skeleton3D at
  its reference pose, in game units and Z-up. Put it under a node with the
  converter's z-up/unit transform (`bethconv_z_up_to_y_up`: -90 degrees about
  X, scale 0.014287), as converted meshes have.
- `attach_skinned(model, skeleton)`: moves every skinned mesh of a converted
  body part (`malebody_1.nif`, hands, feet, head) onto the skeleton. Binds
  match bones by name, case-insensitively, and stay relative to their bone,
  as in the game: a part follows the actor's bones, not the bones of the NIF
  it came from. (Keeping each part where its own NIF put it instead opened a
  gap between a FaceGen head and the body at the neck.) Binds naming a
  missing bone keep the part where its NIF put it, on the root, and are
  counted (`get_last_missing_bones`).
- `build_clip(asset, skeleton, skeleton_path)`: the asset's first clip as an
  Animation. Its splines are sampled once per frame (30 fps) at load, a
  position and a rotation track per bone, so playback costs what any Godot
  animation costs. Track i drives the binding's bone i, by index: build the
  skeleton from the file the clip was made for. Bones parked about 2e7 units
  away (prop slots the clip has nothing for) get no track. Annotations
  (`FootLeft`, `SoundPlay.…`, `HitFrame`) become markers; repeated texts get
  `#2`, `#3`. `build_clip_for(asset, skeleton_asset, skeleton_path)` builds
  the same for the skeleton asset without making a Skeleton3D (C++ only),
  so the asset cache's workers can do it.
- `describe(asset)`: skeleton names and per clip duration, frames, tracks and
  annotations, for tools.

Skeleton3D updates its global poses at the end of a frame: right after a
seek, compose `get_bone_pose` up the parents (as `game/tools/actor_check.gd`
does) or wait a frame.

## Placed actors (`SkydotWorld`)

Cells build their placed actors (ACHR) after their references; exterior
cells include the worldspace's persistent actors standing in them, and their
models and clips are part of `get_exterior_resources` (interiors:
`get_cell_resources`), so streaming loads them on the workers first: a clip
is the asset cache key `AssetCache::clip_key(clip, skeleton)`, sampled into
an Animation there, not on the main thread when the first actor of a kind is
built (that cost 12-25 ms per kind). A build keeps what its actors need, and
models keep their textures, so trimming the cache before the actors are
placed (leaving a place) does not throw that work away. Which actors a cell builds is
decided once its references are placed, so a cell built ahead
(`continue_build_static`, the viewer's door preloading) gets the actors that
are there when it is finished on arrival. `actors = false` (viewer:
`--actors off`) builds none.

What an actor is built from is decided from `world.fb` alone
(`data/actors.cpp`, `get_actor_plan(ref)` shows it):

- **Templates.** TPLT chains are followed while the NPC takes traits (race,
  sex, skin, height, weight, face) or inventory (the outfit) from them. A
  leveled list (LVLN, LVLI) picks one entry, the same every time for a
  reference, so a bandit looks the same after a reload.
- **Skeleton.** The race's skeleton NIF for the sex, with `.hkx` instead
  of `.nif`: `character assets/skeleton.hkx`, `character assets
  female/skeleton_female.hkx`.
- **Worn.** The default outfit's armor (a leveled entry resolved as above);
  the first item on a slot keeps it. The skin (the NPC's WNAM, else the
  race's) adds its addons where nothing worn covers their slots: bodies
  under clothes go, hands and feet without gloves and boots stay.
- **Models.** An addon fits a race named in its RNAM or additional races, or
  the race's armor race. The sex's model, else the other's; with a weight
  slider, `_0` below weight 50 and `_1` from it, when the pack has it.
- **Head.** The FaceGen NIF the game precomputes
  (`facegendata/facegeom/<plugin>/<id>.nif`), when the pack has it.
- **Idle.** The behaviour project's idle clip (see Locomotion), else in the
  behaviour graph's folder `animations/male|female/mt_idle.hkx`, then
  `animations/mt_idle.hkx`, `idle.hkx`, `idle1.hkx` or `idlestand.hkx`.
  Played looping; one Animation per (clip, skeleton), cached by the asset
  cache, for every actor.
- **Size and facing.** NPC height times the race's height for the sex; only
  the rotation about Z.

`game/tools/cell_actors.gd` prints the plans of an interior's actors and
builds it. In the Sleeping Giant Inn every actor stands dressed in its outfit
in the idle.

## Locomotion (`actors/locomotion.*`)

Which clips move an actor, and how fast, is looked up by clip name in its
race's behaviour project, without interpreting the behaviour graph (PLAN.md).
For a race whose behaviour is `meshes/actors/character/defaultmale.hkx`:

1. `meshes/animationdata/defaultmale.txt` lists the project's behaviour
   graphs and its clips: name, id, playback speed.
2. Each behaviour graph's clip generators name the file a clip plays
   (`MT_WalkForward` -> `animations/male/mt_walkforward.hkx`).
3. `meshes/animationdata/boundanims/anims_defaultmale.txt` gives the motion
   for a clip id: 93.4 units forward over 1.133 s, so 82.5 units/s, times the
   playback speed.

The DLC creatures' projects are only in
`meshes/animationdatasinglefile.txt`, which is read when a project has no
file of its own. (The id is not an index into the character's animation
list, as the HKX spike assumed: for humans that gives the wrong files.)

Names differ between creatures, so each gait tries a list first, humans
then creature schemes, and takes the first the project has whose animation
moves forward:

| Gait | Names, in order |
| --- | --- |
| idle | `MT_Idle`, `MainIdle`, `Idle` (a trailing `.hkx` ignored) |
| walk | `MT_WalkForward`, `WalkForward`, `MTWalkForward`, `Forward_Walk`, `WalkForward00`, `WalkForward00_Wolf`, `WalkF`, `MT_Walk_F`, `H2HWalkForward`, `MTForward`, `MT_Forward`, `DefaultForward`, `Forward` |
| run | `MT_RunForward`, `RunForward`, `MTRunForward`, `Forward_Run`, `RunForward00`, `RunF`, `MT_Run_F`, `H2HRunForward`, `MTFastForward`, `MT_FastForward`, `DefaultFastForward`, `FastForward` |

Failing those, the shortest name saying walk (run) forward without a side,
pace or stance. A run no faster than the walk is dropped.
`SkydotWorld.get_locomotion(behaviour)` shows the choice. On vanilla SE,
11,594 of 11,937 placed actors get a walk; humans walk at 1.18 m/s and run
at 5.01 m/s. Without one: storm atronachs, wisps, witchlights, ballista
centurions, dragons, slaughterfish, and those with only a run (chaurus
hunters, vampire brutes, netches, ice wraiths); they stand.
`game/tools/locomotion_check.gd` prints the table for a pack.

## Walking (`SkydotActor`)

A placed actor is a `SkydotActor`: a `SkydotPlayer` (cylinder, gravity,
slopes, step-up, pushing clutter) steered by itself.

- **Body.** As wide as half the skeleton's narrower side at rest, as tall
  as its highest bone, stepping up a quarter of that (0.15-0.6 m). Skinned
  meshes' own bounds are not used: they are not where the bones put them.
  NPCs are on their own physics layer (`LAYER_NPC`); they collide with the
  world, clutter, the player and each other, and the player with them.
  Rays (`pick_ref`, ground checks) do not hit them.
- **Ground.** It holds still until a ray finds the world or terrain under
  it (cells stream their collision in after their actors), and goes back
  home to wait again if it falls 30 m below it.
- **Walking.** `walk_to(target, run)` asks the navigation map for a path
  (from within 1.5 m of the navmesh) and follows its corners, turning at
  5 rad/s; a corner behind it turns it on the spot. Speeds are the clips'
  (metres per second times the actor's scale). Less than 0.2 m of progress
  in 1.5 s gives up the walk (another actor, a locked way).
- **Doors.** Every 0.2 s a waist-high ray looks 0.8 m past the body along
  the way; a closed plain door it hits (models tagged
  `skydot_plain_door`: DOOR bases that lead nowhere) it opens, standing
  0.8 s while it swings. It closes it again 2 s later at the earliest, once
  it is 2 m from the hinge, unless someone else closed or toggled it since.
  Locks are ignored (in the game, residents carry keys). `door_toggled`
  reports both; SkydotAi passes it on to SkydotPapyrus's open state. In the
  Sleeping Giant, Orgnar opens a back room door, goes in and closes it on
  the way out (`game/tools/actor_walk.gd` prints door events).
- **Animation.** Its AnimationPlayer plays `idle`, `walk` or `run` with a
  0.25 s blend, and plays the gait as fast as the body moves (0.5-2x), so
  feet keep pace.
- **Wander.** It idles 6-20 s, then walks to a random point within a radius
  (7.3 m, 512 units, the CK's default sandbox radius, unless set) of its home,
  and idles again. AI packages (`ai.md`) set home and radius for a sandbox
  and turn wandering off for everything else; an actor without packages
  wanders about where it was placed. A path longer than three
  times the radius is not taken. The choices are seeded by the reference,
  so a reload repeats them (physics timing aside).
  `SkydotWorld.actor_wander` (viewer `--wander off`) keeps actors in their
  idle; `--screenshot` and `--benchmark` runs keep them still unless
  `--wander on`.

Measured headless (`game/tools/actor_walk.gd`): in the Sleeping Giant Inn
all three actors walk and stay on the floor; around Riverwood 13 of 13
(guards, chickens, a dog, a cow, a horse) wander for a minute without
falling or sticking.

## Skin and faces (`SkydotMaterials`)

- **Model-space normals.** Bodies, hands, feet and FaceGen heads carry
  model-space normal maps (`_msn`) and no vertex normals. In the NIF's axes
  the normal is (r, b, g) * 2 - 1: measured against the face normals of
  `malebody_1` (mean dot 0.98; the next candidate 0.68). Skinned shapes use
  the bind pose's axes, so limbs bent far from it are lit as if they were
  not.
- **Specular.** Skin (shader type 5) and FaceGen heads (4) take their
  specular mask from slot 7 (`_s`). Before, the mask was 1 everywhere and
  their strong, bluish specular (strength 4.5) washed skin pale blue-grey.
- **Specular flag.** Without SLSF1 Specular (bit 0) a surface gets no
  highlight at all; most clothing has none. Applying it everywhere made
  outfits look polished and Faendal wet.
- **SkinTint (5):** the actor's skin tone, the traits NPC's QNAM
  (`world.fb` `npcs.skin_tone`), soft-lit onto the texture as the game
  does, in gamma space: `base² + 2·tint·base·(1 − base)`, so 0.5 keeps the
  texture. Multiplying instead made Dorthe (tone 0.44, 0.38, 0.34) dark
  brown. Each actor gets its own copy of the material.
- **FaceGen (4):** the NPC's tint mask (slot 6,
  `facegendata/facetint/<plugin>/<id>.dds`), the same soft light.
- **HairTint (6):** the albedo times `lerp(1, tint, vertex green)`; hair
  takes no other vertex colour. The tint is the NIF's own hair tint colour
  (`hair_tint_color` in the extras, pack `mesh/16`), into which the
  Creation Kit bakes the NPC's hair colour for FaceGen heads. Older packs
  draw hair untinted (grey).
- **Blend and test at once** (FaceGen's shaved-hair layers: test at 0):
  glTF keeps only the mask, which drew the stubble opaque, black at the
  back of Faendal's head. Such materials blend and discard below the
  threshold.
- **Hair** of the FaceGen head (shapes named `Hair…`, hairlines included) is
  hidden under items covering slot 31 (hoods, helmets).

Not compared with the game side by side yet. The game also lights skin
with soft lighting and the subsurface map (`_sk`, slot 2), which are not
drawn, and multiplies FaceGen heads by a detail map (slot 3), which is not
applied.

## Checking

```sh
godot4.7 --path game --script res://tools/actor_check.gd -- --pack <pack> \
    [--clips mt_walkforward,mt_runforward,mt_idle] [--out <dir>]
```

builds the vanilla male (body, hands, feet, head) on the human skeleton,
prints the left foot's travel per clip (walk: 0.51 m stride, lifted to
0.22 m; idle: planted) and with `--out` renders a contact sheet per clip.
A pack with only `meshes/actors/character/` converts in about 6 s
(`bethconv convert --filter meshes/actors/character/ --no-records`).
`smoke_animation` covers the same on the test pack's three-bone actor.

```sh
godot4.7 --headless --path game --script res://tools/locomotion_check.gd -- --pack <pack>
godot4.7 --headless --path game --script res://tools/actor_walk.gd -- --pack <pack> \
    --cell RiverwoodSleepingGiantInn [--seconds 40] [--trace on]
godot4.7 --headless --path game --script res://tools/actor_walk.gd -- --pack <pack> \
    --world Tamriel --at 19458,-47900
```

`actor_walk.gd` builds a cell (or 3x3 exterior cells) and reports per actor
how long it idled and walked, how far it went from home and how low it got;
`--trace on` prints every actor's state, speed and clip each second. In the
viewer, `--wander on --shot-delay 12 --screenshot out.png` captures actors
mid-walk. `smoke_actors` walks the test pack's actor across its interior's
navmesh.

## Not done

- Skin: soft lighting, subsurface and the FaceGen detail map (see above).
- Bare feet (`malefeet_1.nif`) rendered white before the skin work, most
  likely the same full-strength specular; not checked again.
- Weapons and shields, carried or sheathed; inventory beyond the outfit.
- Root motion is used only as a speed: the body moves in a straight line
  at the clip's average speed, the clip's own turns are not applied.
  Behaviour graphs are not interpreted (per PLAN.md; clips are chosen by
  name): no turning or start/stop clips, no strafing, no blending beyond the
  cross-fade, additive clips (blend hint 1), float tracks
  (`hkVis`/`hkFade` slots), paired animations' second actor.
- AI: packages and schedules are run (`ai.md`); sitting and sleeping in
  furniture, swimming and
  avoidance (actors bump into each other) are not.
- Creatures that only fly, swim or hover (dragons, slaughterfish, wisps)
  have no walk and stand; those with only a run stand too.
- Wolves, bears, deer and other creatures without a named idle use the
  `mt_idle.hkx` file fallback; the mudcrab now gets `MainIdle`.
