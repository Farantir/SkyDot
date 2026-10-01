# Spike: NIF → glTF

**Question:** does Skyrim geometry survive conversion to glTF, and do other
consumers read the result the same way?

**Budget:** 1 week · **Took:** ~1 day · **Result:** yes; found one writer bug
and one texture problem

> **Correction (2026-08-23):** this report says cubemaps must be converted to
> `Cubemap` resources. They don't: Godot 4.7.2 loads DDS cubemaps natively, and
> all 95 vanilla ones load. The cubemap failures seen here were most likely
> caused by `fix_mip_tail.py` appending a single tail to a six-face file. Current
> numbers are in [`../format-notes/dds-textures.md`](../format-notes/dds-textures.md).

## Criterion

Convert representative statics (rock, alpha tree, emissive lantern, skinned
body, collision, multi-material building) with nifly + fastgltf and open them in
Blender and Godot. Geometry, UVs, normals/tangents and material slots survive;
the `BSLightingShaderProperty` → glTF mapping is documented; a skinning path
exists.

Instead of 50 files, every NIF in vanilla LE and SE was converted, and a
350-file stratified sample was opened in both consumers.

## Setup

Godot 4.7.2.rc1, Blender 5.1, Linux, RX 5700 XT, 12 threads. Converter built
Debug + ASan, so timings are upper bounds.

## Conversion of every NIF

| | Files | Failed | Shapes | Vertices | Triangles | Skinned | Collision | GLB | Wall |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| LE `Meshes.bsa` | 17,216 | 0 | 62,591 | 27.4 M | 32.2 M | 21,422 | 11,735 | 2.4 GiB | 454 s |
| SE `Meshes0+1` | 22,047 | 0 | 81,471 | 36.2 M | 42.1 M | 26,940 | 15,548 | 3.1 GiB | 603 s |

No failures. The only diagnostics are 24 warnings for `bhkNiTriStripsShape`, an
LE-era collision shape not decoded yet; it is skipped and the mesh converts.

`meshes/creationclub/_shared/dungeons/ayleidruins/interior/triggers/artrigpressureplate01.nif`
is an LE-format NIF inside the SE archives, which is why the encoding is read
from each file.

## LE and SE give the same models

Converting the sample's paths from the LE archive instead:

```
224 of the 350 paths exist in LE (the rest are DLC or SE-only)
222 produce an identical (primitives, vertices, triangles, materials, skins)
  2 differ
```

The two (`fallforestferncluster03.nif`, `…04.nif`) were re-authored by Bethesda
for SE; `bethconv mesh --inspect` shows the same difference in the source files.

One LE case needs special handling in `nif_reader.cpp`: a skinned `NiTriShape`
may have no triangles of its own because they live in the `NiSkinPartition`.
nifly rebuilds them for `BSTriShape` but not for `NiTriShape`. Affects 70 LE
files, including every tree and armor piece.

## Two consumers

350 files, 35 each from `actors armor weapons architecture clutter landscape
plants dungeons effects furniture`, plus the 664 textures they reference.

### Godot 4.7.2

```
godot4.7 --headless --import      (cold, .godot and all sidecars removed)
  wall 9.8-10.3 s over three runs   peak RSS 802 MiB   exit 0   0 errors   0 warnings
```

| | |
| --- | ---: |
| Scenes that instantiate | 350 / 350 |
| `MeshInstance3D` / surfaces / materials | 1,250 / 1,250 / 1,250 |
| Materials with albedo | 1,203 |
| Materials with a normal map | 1,064 |
| Texture formats in memory | DXT1 545, DXT5 658 (blocks kept) |
| `Skeleton3D` / bones | 74 / 781 |
| Nodes with `bethconv` extras | 598 |

The 47 materials without albedo belong to editor markers and effect shaders
with an empty slot 0.

From the LE archive (224 models, 413 textures, no mip fix needed): 0 errors,
224/224 scenes, 769 materials, 731 albedo, 627 normal maps, 63 skeletons, 531
bones, DXT1 370 / DXT3 84 / DXT5 277. DXT3 only occurs in LE.

Extras arrive as node metadata: `node.get_meta("extras")["bethconv"]` gives
provenance on the root and collision on the node that owned the
`bhkCollisionObject`, so ragdolls can be read per limb.

Scale and orientation (world-space AABB after import):

| Model | Longest axis |
| --- | ---: |
| Nord Hero greatsword | 1.57 m |
| Nord Hero / wooden bow | 1.63 m |
| Falmer longsword | 1.11 m |
| 1st-person pickaxe | 0.64 m |

Correct sizes with `unit_scale = 0.0142875` at the root. The long axis lands on
Z, where NIF Y goes under the Z-up → Y-up rotation.

### Blender 5.1

```
blender --background --factory-startup --python blender_check.py -- proj/meshes
  350 files imported, 0 failed
  1,324 mesh objects, 557,419 vertices, 1,250 materials
  1,687 image datablocks, all with decoded pixels, 0 empty
  74 armatures / 781 bones   <- identical to Godot's count
  595 objects carrying the bethconv extras
  26 meshes with no UV layer, 0 meshes with no faces
```

The 26 meshes without UVs are emitter volumes, waterfall current planes and
bones, not rendered surfaces. Both importers report 74 armatures and 781 bones.

## Bug: image URIs relative to the wrong base

The first Godot import gave 1,687 warnings and untextured materials, while the
writer's tests passed and fastgltf read the files fine.

glTF resolves relative URIs against the document. `textures/weapons/x.dds` in
`meshes/weapons/y.glb` points to `meshes/weapons/textures/weapons/x.dds`. The fix
prefixes `../` for the source path's depth; extras keep pack-relative paths
because they name textures rather than point to them.
`tests/unit/test_gltf_writer.cpp` covers it, and
`docs/spikes/nif-gltf/referenced_textures.py` fails if a URI escapes the tree.

Reading a file back with the library that wrote it cannot find this kind of
bug; a separate consumer can.

## Godot needs full mip chains; SE textures stop early

With URIs fixed, Godot still rejected 658 of 664 textures:

```
Expected Image data size of 512x512x1 (DXT1 RGB8 with 10 mipmaps) = 174776 bytes,
got 174768 bytes instead.
```

`pickaxe02.dds` is 512×512 with `dwMipMapCount = 9` and nine levels, down to
2×2. Godot requires a chain to 1×1 and rejects the whole file. The file is valid
DDS and Blender reads it.

2,000-file uniform samples:

| | Complete | Short | Single level |
| --- | ---: | ---: | ---: |
| SE, 2,000 of 31,830 | 6 | 1,988 | 6 |
| LE, 2,000 of 18,577 | 1,975 | 0 | 25 |

1,936 of the short files miss only the final 1×1 level. The six complete SE
files are large tree billboards.

Format mix in the same samples:

| | SE | LE |
| --- | ---: | ---: |
| DXT5 / BC3 | 1,118 | 254 |
| uncompressed 32-bit | 653 | 0 |
| DXT1 / BC1 | 220 | 1,677 |
| DXT3 / BC2 | 0 | 46 |
| uncompressed 24-bit | 6 | 23 |
| BC7 / DX10 | 0 | 0 |

### Fix

Append the missing levels (one block each, repeating the last level) and fix
`dwMipMapCount`: a 4-byte edit plus 8–32 bytes, no re-encoding. Prototype:
`docs/spikes/nif-gltf/fix_mip_tail.py`.

| | Loaded as `Texture2D` | Failed |
| --- | ---: | ---: |
| SE, fixed | 1,997 / 2,000 | 3 |
| LE, untouched | 1,996 / 2,000 | 4 |

All failures are cubemaps (see the correction at the top). Loaded formats are
DXT1, DXT3, DXT5, RGB8 and RGBA8, so Godot keeps DDS blocks as the headless-bake
spike found.

## Consequences

1. Desktop textures stay passthrough but must have their mip chain completed;
   otherwise Godot drops ~99% of SE textures. Now in `src/texture/mip_tail.cpp`.
2. ~~Cubemaps must become `Cubemap` resources.~~ Not needed; the texture pass
   just has to recognize cubemaps and leave their face-major layout alone.
3. The texture pass must process many files per mount. Mounting SE's nine
   texture archives takes ~0.8 s; the 664 textures took nine minutes one process
   at a time and 4.3 s in one. `extract` gained `--from` and multiple paths.
4. Check every converted format with a consumer that did not write it,
   preferably two.
5. Collision stays in `extras`, not glTF geometry, and the axis/unit choice
   holds in both consumers.

## Open

- **Tangent handedness.** Both consumers read `TANGENT`, but correctness needs
  a rendered comparison with the game.
- **Vertex color alpha** means different things depending on shader flags. It is
  kept as is, not interpreted.
- **UV-less meshes** probably should not become `MeshInstance3D`; a scene
  construction question for the engine.
