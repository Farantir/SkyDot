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
  match bones by name, case-insensitively, and are recomputed against the
  skeleton's rest, so a part sits where its own NIF put it even where its
  bones differ slightly from the skeleton's. Binds naming a missing bone
  fall back to the root and are counted (`get_last_missing_bones`).
- `build_clip(asset, skeleton, skeleton_path)`: the asset's first clip as an
  Animation. Its splines are sampled once per frame (30 fps) at load, a
  position and a rotation track per bone, so playback costs what any Godot
  animation costs. Track i drives the binding's bone i, by index: build the
  skeleton from the file the clip was made for. Bones parked about 2e7 units
  away (prop slots the clip has nothing for) get no track. Annotations
  (`FootLeft`, `SoundPlay.…`, `HitFrame`) become markers; repeated texts get
  `#2`, `#3`.
- `describe(asset)`: skeleton names and per clip duration, frames, tracks and
  annotations, for tools.

Skeleton3D updates its global poses at the end of a frame: right after a
seek, compose `get_bone_pose` up the parents (as `game/tools/actor_check.gd`
does) or wait a frame.

## Placed actors (`SkydotWorld`)

Cells build their placed actors (ACHR) after their references; exterior
cells include the worldspace's persistent actors standing in them, and their
models are part of `get_exterior_resources`, so streaming loads them on the
workers first. `actors = false` (viewer: `--actors off`) builds none.

What an actor is built from is decided from `world.fb` alone
(`world/actors.cpp`, `get_actor_plan(ref)` shows it):

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
- **Idle.** In the behaviour graph's folder, `animations/male|female/mt_idle.hkx`,
  else `animations/mt_idle.hkx`, `idle.hkx`, `idle1.hkx` or `idlestand.hkx`.
  Played looping; one Animation per (clip, skeleton) for every actor.
- **Size and facing.** NPC height times the race's height for the sex; only
  the rotation about Z.

`game/tools/cell_actors.gd` prints the plans of an interior's actors and
builds it. In the Sleeping Giant Inn every actor stands dressed in its outfit
in the idle.

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

## Not done

- **Skin and face shading:** skin and FaceGen faces use the plain lighting
  shader, so they come out pale grey: Skyrim tints skin (SkinTint) and faces
  (the per-NPC tint mask in `facegendata/facetint/`) in the shader.
- Hair from the FaceGen head shows through helmets (slot 31 is not hidden).
- Bare feet (`malefeet_1.nif`) render untextured white although they use the
  body's texture, which renders on the body; not looked into yet.
- Weapons and shields, carried or sheathed; inventory beyond the outfit.
- Root motion (`meshes/animationdata/boundanims/`), behaviour graphs (not
  interpreted, per PLAN.md; clips are chosen by name), blending between
  clips, additive clips (blend hint 1), float tracks (`hkVis`/`hkFade`
  slots), paired animations' second actor.
- AI: packages, walking the navmesh, sitting in furniture, unlocking doors.
- Actors have no collision; the player walks through them.
- Creature idles: most creatures have `mt_idle.hkx` (wolf, bear, elk, deer,
  hare, chicken, cow, dog), the horse `idle.hkx`; the mudcrab none of the
  names tried, so it stands in its bind pose.
