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

Root motion (`meshes/animationdata/boundanims/`), behaviour graphs (not
interpreted, per PLAN.md; clips are chosen by name), blending between clips,
additive clips (blend hint 1), float tracks (`hkVis`/`hkFade` slots), paired
animations' second actor, NPC records and their placement.
