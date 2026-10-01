# Skyrim LE vs. SE/VR on disk

Measured by mounting unmodded LE and SE installs and comparing the same virtual
paths. SE differs from LE on four independent layers, and only two of them are
marked by a version number, so a single "is SE" boolean is wrong somewhere.

| Layer | LE | SE / VR | How to tell |
| --- | --- | --- | --- |
| BSA | v104 | v105 | archive header |
| Plugin | TES4 `HEDR` 0.94 / 1.70 | 1.70 / 1.71 | per file, not per install |
| NIF | `NiTriShape` | `BSTriShape` | BS header version, not the version string |
| DDS | DXT1/3/5 | DXT1/5 + raw 32-bit | pixel format only |

## Plugin header version is per file

All eighteen masters across the three installs:

| Install | 0.94 | 1.70 | 1.71 |
| --- | --- | --- | --- |
| LE | `Skyrim.esm` | `Update.esm` | — |
| SE | — | `ccBGSSSE037-Curios.esl`, `ccQDRSSE001-SurvivalMode.esl` | the other eight |
| VR | — | all six | — |

SE's and VR's `Skyrim.esm` differ in header version (1.71 vs 1.70) and size
(249,752,131 vs 249,753,240 bytes) but have identical record-type histograms
and the same 869,687 records in 50,494 groups. The header version is recorded,
never used to choose a parser.

## The NIF version string does not identify the geometry blocks

`meshes/architecture/whiterun/wrbuildings/wrhouseshack02.nif` from both installs
has the same header, `Gamebryo File Format, Version 20.2.0.7`, but different
blocks:

```
LE  723,147 B   BSFadeNode NiTriShape NiTriShapeData NiAlphaProperty
                BSLightingShaderProperty BSShaderTextureSet BSXFlags
                bhkCollisionObject bhkRigidBody bhkMoppBvTreeShape
                bhkCompressedMeshShape bhkCompressedMeshShapeData

SE  351,432 B   BSFadeNode BSTriShape          NiAlphaProperty
                BSLightingShaderProperty BSShaderTextureSet BSXFlags
                bhkCollisionObject bhkRigidBody bhkMoppBvTreeShape
                bhkCompressedMeshShape bhkCompressedMeshShapeData
```

SE replaced `NiTriShape` + `NiTriShapeData` with one `BSTriShape` holding an
interleaved half-float vertex buffer. The mesh reader selects on the BS header
version and supports both; handling only `BSTriShape` would silently produce
empty meshes for every LE-era mod. Collision blocks are identical.

## Vanilla SE textures are not BC7

Stratified samples: 105 files across SE's nine texture archives, 100 from LE's
one.

| | LE (n=100) | SE (n=105) |
| --- | --- | --- |
| DXT1 (BC1) | 67% | 28% |
| DXT5 (BC3) | 29% | 51% |
| DXT3 (BC2) | 4% | — |
| uncompressed 32-bit | — | 21% |
| BC7 / DX10 header | 0 | 0 |

No DX10 headers at all, so no BC7, BC6H, BC5 or BC4. Desktop passthrough loses
nothing; re-encoding only matters for mobile/ASTC.

## The uncompressed files are terrain LOD diffuse maps

Every uncompressed file in the SE sample matches
`textures/terrain/<worldspace>/<worldspace>.<lod>.<x>.<y>.dds`. Diffuse/normal
pairs:

```
tamriel.4.-12.48.dds        raw32   256x256   349,648 B
tamriel.4.-12.48_n.dds      DXT5    256x256    87,520 B
blackreach.4.5.-5.dds       raw32   256x256   349,648 B
blackreach.4.5.-5_n.dds     DXT5    256x256    87,520 B
```

The diffuse ships raw, its normal map compressed. BC1 would cut these 8:1
(~43.7 KiB vs 341 KiB) on distant-only textures, so they are the one case where
re-encoding for desktop pays; select them by path pattern. The `.<lod>.` part
is the LOD level and `x`/`y` are cell coordinates, so they map directly onto the
cell grid.

## SE truncates mip chains; LE does not

Uniform random samples of 2,000 textures per install (SE of 31,830, LE of
18,577):

| | LE | SE |
| --- | ---: | ---: |
| DXT1 (BC1) | 1,677 | 220 |
| DXT5 (BC3) | 254 | 1,118 |
| DXT3 (BC2) | 46 | — |
| uncompressed | 23 | 662 |
| BC7 / DX10 header | 0 | 0 |
| mip chain reaching 1×1 | 1,975 | 6 |
| mip chain short | 0 | 1,988 |

1,936 of the short SE chains stop at 2×2. `dwMipMapCount` matches the data, so
the files are valid DDS and Blender reads them. Godot requires a chain to 1×1
and rejects the whole file:

```
Expected Image data size of 512x512x1 (DXT1 RGB8 with 10 mipmaps) = 174776 bytes,
got 174768 bytes instead.
```

The difference is one DXT1 block. The texture pass appends the missing levels
and fixes `dwMipMapCount`: a 4-byte header edit plus 8–32 bytes, no decoding.
That takes the SE sample from 6 to 1,997 of 2,000 loading in Godot
([NIF → glTF spike](../spikes/nif-gltf.md)). LE needs nothing.

The rest are cubemaps (`DDSCAPS2_CUBEMAP`): 3 in the SE sample, 4 in LE. They
need a Godot `Cubemap`, not a `Texture2D`.

## One LE-format NIF ships in the SE archives

`meshes/creationclub/_shared/dungeons/ayleidruins/interior/triggers/artrigpressureplate01.nif`
in `Skyrim - Meshes1.bsa` has user version 2 = 83, i.e. LE geometry: one file in
22,047. Inferring the encoding from the install would break exactly this mesh.
