# DDS textures and what Godot requires

Every `.dds` header in unmodded LE and SE (18,580 and 31,833 files) was read,
and samples of the output were loaded in Godot and Blender.

```sh
B=build/linux-debug-asan/tools/bethconv-cli/bethconv
$B texture --inspect -q --source "$SKYRIM_DATA_LE/Skyrim - Textures.bsa"
$B texture --inspect -q $(for i in 0 1 2 3 4 5 6 7 8; do
      echo --source "$SKYRIM_DATA_SE/Skyrim - Textures$i.bsa"; done)
```

## Summary

Desktop textures are passed through with one edit: completing short mip
chains. It applies to 99.0% of SE and 0% of LE, and costs a 4-byte header patch
plus a median of 11 appended bytes. Everything else, including cubemaps and the
volume texture, is copied unchanged and loads.

## Vanilla contents

| | SE (31,833) | LE (18,580) |
| --- | ---: | ---: |
| DXT5 / BC3 | 17,507 | 2,517 |
| uncompressed 32-bit | 10,047 | 1 |
| DXT1 / BC1 | 4,090 | 15,379 |
| DXT3 / BC2 | 0 | 493 |
| uncompressed 24-bit | 188 | 189 |
| L8 | 1 | 1 |
| BC7 or any DX10 header | 0 | 0 |
| cubemaps | 57 | 38 |
| volume textures | 1 | 1 |
| unparseable headers | 0 | 0 |
| total size | 16.3 GiB | 2.25 GiB |

No BC7 anywhere in vanilla. A third of SE's files are uncompressed terrain LOD
diffuse maps. The one volume texture is `textures/effects/noisevolume.dds`
(128×128 L8), identical in both.

## A modded load order

619-mod Wabbajack list (71 BSAs plus loose files): 27,508 textures, 39.6 GiB,
no header failures.

| | count |
| --- | ---: |
| DXT5 | 17,048 |
| uncompressed 32-bit | 3,992 |
| BC7 (DX10 header) | 3,487 |
| DXT1 | 2,157 |
| DXT3 | 555 |
| BC4U | 261 |
| uncompressed 24-bit | 7 |
| BC6H | 1 |
| cubemaps | 134 |
| short mip chains | 258 |

BC7 is 13% of a modded setup, so the parser needs its DXGI table. Mod authors
export complete chains; only 0.9% need the fix.

This run also exposed a bug: `ArchiveSet` reopened loose files by their
lowercased path and lost 5,480 files under folders like `textures/!_Rudy_Misc/`.
Fixed; the numbers above are from after the fix.

## Short mip chains in SE

| | Chain to 1×1 | Short | No chain |
| --- | ---: | ---: | ---: |
| SE | 106 | 31,522 | 204 |
| LE | 18,376 | 0 | 203 |

The SE repack introduced this. The files are valid DDS and Blender reads them,
but Godot computes the expected size from the dimensions, requires a chain to
1×1 whenever one is declared, and rejects the whole file otherwise:

```
Expected Image data size of 512x512x1 (DXT1 RGB8 with 10 mipmaps) = 174776 bytes,
got 174768 bytes instead.
```

Files with no chain (`dwMipMapCount` 0 or 1) are accepted and left alone.

### The fix

Set `dwMipMapCount` and append the missing levels; change nothing else. Each
missing level is one block (a few for oblong surfaces), filled by repeating the
last stored level, which already averages its area. No decode or re-encode.

On a 1-in-16 SE sample (1,990 files, 1,054.7 MiB) this appended 21,896 bytes in
total.

### Cubemaps store faces one after another

`docs/spikes/nif-gltf/fix_mip_tail.py` appends one tail at the end of the file,
which is right for 2D textures. A cubemap stores six complete chains in a row,
so that leaves every face short and makes `dwMipMapCount` wrong for all six.
`src/texture/mip_tail.cpp` works per face, and `tests/unit/test_mip_tail.cpp`
checks that each face's new levels come from that face.

## Cubemaps need no conversion (Godot 4.7.2)

| | Loaded | As |
| --- | ---: | --- |
| SE cubemaps | 57 / 57 | `Cubemap`, 6 layers |
| LE cubemaps | 38 / 38 | `Cubemap`, 6 layers |
| volume texture | 1 / 1 each | `ImageTexture3D` |

Godot loads DDS at runtime through a `ResourceFormatLoader`, without the import
pipeline. All vanilla cubemaps already have full chains, so the fix skips them.
The texture pass only has to recognize them and keep 2D logic away.

## Verification

A 1-in-16 SE sample (1,990 files) converted with and without the fix
(`--no-fix`) and loaded in Godot 4.7.2:

| | Loaded | Failed |
| --- | ---: | ---: |
| fixed | 1,990 (1,985 `ImageTexture`, 4 `Cubemap`, 1 `ImageTexture3D`) | 0 |
| unfixed | 23 | 1,967 |

1,967 equals the number of short chains in the sample.

Blender 5.1 loads 400 files from each tree without errors, with identical pixel
totals (208,840,784), so the fix changes nothing for a tolerant reader.

A 1-in-16 LE sample (1,162 files) converts to byte-identical trees with and
without the fix.

## Header fields read

Source: Microsoft's [Programming Guide for
DDS](https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide),
`DDS_HEADER`, `DDS_PIXELFORMAT`, `DDS_HEADER_DXT10` and
[`DXGI_FORMAT`](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format).

| Offset | Field | Use |
| ---: | --- | --- |
| 0 | magic `DDS ` | must match |
| 4 | `dwSize` | must be 124 |
| 12, 16 | `dwHeight`, `dwWidth` | surface size |
| 24 | `dwDepth` | >1 means volume |
| 28 | `dwMipMapCount` | patched by the fix |
| 76 | `DDS_PIXELFORMAT.dwSize` | must be 32 |
| 80 | `dwFlags` | FOURCC vs. RGB/LUMINANCE/ALPHA |
| 84 | `dwFourCC` | `DXT1`…`DXT5`, `ATI1`/`ATI2`, `DX10` |
| 88 | `dwRGBBitCount` | bytes per pixel (uncompressed) |
| 112 | `dwCaps2` | `CUBEMAP` (0x200) + face bits, `VOLUME` (0x200000) |
| 128 | `DDS_HEADER_DXT10` | `dxgiFormat`, `resourceDimension`, `miscFlag`, `arraySize` |

Not read: `dwFlags` (offset 8) and `dwPitchOrLinearSize` (offset 20). Writers
disagree on both, neither is needed, and the fix keeps the top-level size.

Rejected: wrong header size, a dimension of 0 or above 65,536, more mips than
the surface allows, sub-byte pixel formats, unknown FOURCC or DXGI formats, and
files shorter than their declared levels. None occur in the 50,413 vanilla
textures.

Copied without rewriting: volume textures (depth halves too, so their sizes are
not computed) and DX10 texture arrays.

## Limiting the size

`convert --max-texture-size N` (`texture/mip_drop.hpp`) keeps every texture
within N pixels on its longer side by dropping its top mip levels: the smaller
levels are already in the file, so nothing is decoded or re-encoded. Only
dwHeight, dwWidth, dwPitchOrLinearSize (when set) and dwMipMapCount change;
cubemaps are cut face by face. The mip-tail fix then runs on the smaller file.
A texture over the limit without a chain, or whose chain stops before a level
that fits, is passed through and listed as a warning.

Measured on vanilla SE (2026-10-01): the first 2,000 texture paths at 512 px:
300 cut, 930 MiB saved, none kept larger; all 1,999 files of a `view` load in
Godot, the largest at 512. All 42 `textures/cubemaps/` at 32 px: 9 cut, 41 load
through `SkydotPack.load_texture` as a `Cubemap`, one is a 2D texture, none
failed.

## Compressing uncompressed textures

`convert --encode-uncompressed bc7|compact` (`texture/bc_encode.hpp`,
bc7enc_rdo) block-compresses textures the game stores uncompressed, level by
level, through their channel masks. `bc7` makes everything BC7 (DX10 header,
format 98); `compact` uses BC1 for opaque textures that are not normal maps
(`_n`, `_msn`) and BC7 for the rest. Cubemaps and volumes are left alone and
reported. Already compressed textures are never re-encoded.

Measured on vanilla SE, all of `textures/` (release build, 12 threads,
2026-10-01):

| | Time | Store | Encoded |
| --- | ---: | ---: | ---: |
| keep | 16 s | 15,304 MiB | |
| compact | 29 s | 14,373 MiB | 2,780 (12 left: cubemaps, the volume) |
| bc7 | 37 s | 14,476 MiB | 2,780 |

The 10,047 uncompressed paths are 2,780 distinct files, mostly small terrain
LOD tiles, so vanilla gains about 0.8 GiB; mods with uncompressed 2K and 4K
textures gain far more. Quality against the uncompressed originals, 150 terrain
paths decoded by Godot: BC7 mean 70.0 dB PSNR, worst 49.3 dB; compact (149 of
them BC1) mean 69.4 dB, worst 42.5 dB. Every file loads in Godot as BPTC or
DXT1.

