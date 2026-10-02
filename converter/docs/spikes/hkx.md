# Spike: HKX animations

**Question:** can Skyrim's Havok animations be decoded without the Havok SDK,
and do the decoded clips play correctly on a converted skeleton in Godot?

**Budget:** 1 week · **Took:** ~1 day · **Result:** yes. Every animation file
in LE and SE decodes; walk, run, idle and an attack play correctly on the
converted body. Found one converter bug (skinned meshes placed 120 units low),
fixed in `mesh/14`.

## Criterion

Decode one idle and one walk cycle from `Skyrim - Animations.bsa` and play them
on a converted skeleton. Instead of two clips, every `.hkx` in both editions
was read and decoded, and four clips were played in Godot through the runtime
`GLTFDocument` path the engine uses for pack meshes.

## Setup

Python 3.14 prototype in [`hkx/`](hkx/), written from the file contents (no
Havok SDK, no third-party HKX code). Godot 4.7.2.rc1, RX 5700 XT. Game data:
the Steam LE and SE installs from `HANDOVER.md`.

| File | Purpose |
| --- | --- |
| `hkx.py` | Packfile reader; skeletons, bindings, annotations, spline-compressed and interleaved animations |
| `census.py` | Decode every `.hkx` under a folder and summarize classes, formats and fit |
| `make_glb.py` | bethconv's `skeleton.nif` GLB + skinned body GLBs + clips → one animated GLB |
| `godot_frames.gd` | Load that GLB at runtime, play each clip, save a contact sheet |
| `skin_check.py` | Compare a GLB's skinned bind pose with its node placement (found the bug below) |
| `size_estimate.py` | Pack size of clips as glTF channels against the spline data |

```sh
B=converter/build/linux-release/tools/bethconv-cli/bethconv
A="$SL/Skyrim Special Edition/Data/Skyrim - Animations.bsa"
$B scan --list 100000 --filter .hkx "$A" \
    | sed -E 's/^  (.*\.hkx) +Data\/.*/\1/' | grep '\.hkx$' > hkx.txt   # paths contain spaces
$B extract --source "$A" --from hkx.txt -o all-se -q
python3 census.py all-se
```

## The file format

Skyrim (LE and SE alike) uses binary packfiles, version 8,
`hk_2010.2.0-r1`. The only difference between editions is the layout rules in
the header: LE has 4-byte pointers, SE 8-byte. The `__types__` section is
empty, so member offsets are fixed per pointer size and live in `hkx.py`.

Three sections: `__classnames__` (CRC + name), `__types__` (empty),
`__data__`. Each section carries local fixups (pointer → same section),
global fixups (pointer → any section) and virtual fixups (object → class
name). The fixup tables are padded with `0xFF`, so their lengths are not
multiples of the entry size.

### Spline-compressed animation

Every animation in both editions is `hkaSplineCompressedAnimation` with
THREECOMP40 rotations. Per block, at `blockOffsets[b]`:

1. `maskAndQuantizationSize` bytes of masks: 4 per transform track
   (quantization, position, rotation, scale flags), then 1 per float track.
2. Transform data, track after track: position, rotation, scale. Each is a
   B-spline (`u16` control point count − 1, `u8` degree, `u8` knots,
   per-axis min/max floats, then 8- or 16-bit control points), a static value,
   or absent (identity). Rotations are aligned to their quantization's size
   before the control points.
3. Float tracks at `floatBlockOffsets[b]` (relative to the block): static
   (mask `0x03`, one float) or a spline with **16-bit** control points (mask
   `0x12`). No 8-bit float track exists, so which bit picks the quantization
   is unknown.

`transformOffsets` holds the start of every track in every block (only filled
for multi-block clips). Blocks share their boundary frame: block `b` starts at
frame `b * (maxFramesPerBlock − 1)`, and knots count frames within the block.
Splines are evaluated with de Boor's algorithm; rotations are blended
componentwise and normalized.

## Census

| | SE | LE |
| --- | ---: | ---: |
| `.hkx` files | 7,699 | 6,145 |
| Read | 7,699 | 6,145 |
| Animations | 6,126 | 4,982 |
| Frames decoded | 471,511 | 384,226 |
| Multi-block animations | 295 | 255 |
| Track offset mismatches (against `transformOffsets`) | 0 | 0 |
| Block decode ending ≥ 16 bytes before the next block | 0 | 0 |
| Rotation quantization | THREECOMP40 only | THREECOMP40 only |
| Frame rate | 30 fps (6,065), a few short or single-frame clips | same |
| `hkaDefaultAnimatedReferenceFrame` (extracted motion) | 13 | 12 |

Every block's decode ends within its 16-byte padding of where the next block
begins, and every multi-block track starts exactly where `transformOffsets`
says. A wrong guess about any layout rule would break both.

The other files are behaviour graphs (`hkb*`, 428 files), character and
project data, skeletons (with ragdolls, `hkp*`, and `hkaSkeletonMapper`), and
physics; they are not decoded.

LE and SE: of the 6,145 paths in both, 6,143 decode to identical poses. The two
others (`canine/animations/idlecombat1` and `2`) were re-authored for SE.

### Checks on the decoded data

- **Bone lengths:** across walk, run and idle, no bone's translation moves
  more than 6 units from the skeleton's reference pose except `NPC COM` (the
  body bobbing) and `Camera3rd`.
- **Loops close:** first and last frame differ by 0.05° (walk) and 0.32°
  (run).
- **Smoothness:** the largest step between frames is a toe flicking at the
  run's foot plant (49° in 1/30 s), continuous on both sides.
- **Out-of-range samples:** 10,448 samples in 74 SE clips have translations
  near 2e7 units. All are on prop attachment bones (`AnimObjectR` and the same
  slot in paired animations) that the clip carries no prop for: parked far
  away. The track offsets confirm the decode is in step around them.
- **Skeleton:** `skeleton.hkx`'s reference pose matches bethconv's
  `skeleton.nif` conversion to 3e-5 units and 0.06°. Its 99 bones are in game
  units (COM at 68.9 units). Bone names match the NIF's case-insensitively
  (`Weapon` vs `WEAPON`); the NIF lacks only `x_NPC LookNode`, `Translate`
  and `Rotate`.

## On screen

`make_glb.py` joined bethconv's `skeleton.nif`, `malebody_1`, `malehands_1`,
`malefeet_1` and `malehead` with four clips; `godot_frames.gd` played them in
Godot. Walk, run, idle and `1hm_attackright` all play correctly: legs
alternate, arms counter-swing, the feet stay on the floor (in place: root
motion is separate, below), and the attack swings overhead and recovers.

The first renders showed the body hanging 1.72 m below the skeleton, in
bethconv's own body GLB too. That was a converter bug.

## Converter bug: skinned meshes lost their placement

`nif_reader.cpp` built inverse bind matrices from `global_to_skin ∘
shape_to_global`, taking `shape_to_global` from nifly's
`GetNodeTransformToGlobal`. That function only finds `NiNode`s; a shape is a
`NiTriShape`/`BSTriShape`, so the lookup failed silently and returned
identity. Every skinned shape with its own transform lost it: vanilla bodies,
hands, feet and heads sit 120.35 units up, so they rendered 120 units low.
Statics are unaffected, and so were the skinned trees, whose shapes have no
offset.

Fixed by walking the shape's own parents (bounded like the scene walk).
`skin_check.py` now reports 0.000 units between skinned and placed vertices
for the SE and LE body parts, against 120.354 before. A unit test builds an LE
and an SE shape skinned to one bone and checks the matrix (fails without the
fix). Mesh recipe `mesh/14`, so packs reconvert their skinned meshes.

## Root motion

Clips are in place. The motion lives in text files, not in the HKX:

- `meshes/animationdata/<project>.txt` lists the project's clip generators:
  clip name, animation index, playback speed, and the clip's annotations.
- `meshes/animationdata/boundanims/anims_<project>.txt` holds per animation
  index: duration, translation keys (`time x y z`), rotation keys
  (`time x y z w`).
- SE also has `meshes/animationdatasinglefile.txt` with all projects in one
  file.

`MT_WalkForward` is animation 965: 93.4 units forward in 1.133 s (1.18 m/s).
`MT_RunForward` is 944: 222 units in 0.633 s (5.0 m/s). Mapping an index to
its `.hkx` path needs the character file's animation list
(`hkbCharacterStringData` in `characters/defaultmale.hkx`), which this spike
does not read.

## Size in a pack

All 6,126 SE clips:

| Stored as | Size |
| --- | ---: |
| glTF-style channels, every track every frame (as `make_glb.py` writes) | 1,026 MiB |
| Only channels that change (constant ones as one key) | 318 MiB |
| The HKX spline data, unchanged | 54 MiB |
| All `.hkx` files (including behaviours and skeletons) | 106 MiB |

So clips should not go into packs as sampled glTF. Keep the spline blocks and
evaluate them in the engine; the evaluator is a few hundred lines of C++ and
runs once per clip load (or per frame, if it ever matters).

## Recommendation

Go. For the converter (`animation/` module, all bytes through `SpanReader`):

1. Packfile reader for both pointer sizes; `hkaSkeleton`,
   `hkaAnimationBinding`, `hkaSplineCompressedAnimation` with annotations.
   Fuzz target. Pin the census (counts, offset check, LE/SE equality) in the
   corpus harness.
2. A clip asset (`.animfb`): binding, frame count, block layout, the spline
   data re-encoded as plain arrays (or kept as bytes and decoded in the
   engine), annotations, root motion from `boundanims`.
3. Behaviour project index: clip name → animation index → `.hkx` path, from
   the project text files and `hkbCharacterStringData`. Enough to find
   "MT_WalkForward" for an actor without interpreting behaviour graphs (PLAN.md
   says not to).

For the engine: build a Godot `Animation` from a clip asset at load time,
apply it to the skeleton from `skeleton.nif`, move the actor by the root
motion.

Not looked at: paired animations' second skeleton (tracks past 99), the 13
clips with `hkaDefaultAnimatedReferenceFrame`, additive blending (21 bindings
have blend hint 1), creature skeletons other than the human one, and the
first-person skeleton beyond its bone count.
