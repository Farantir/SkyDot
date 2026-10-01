# Spike: headless bake

**Question:** can Godot's importer run fully headless at the scale of a full
bake?

**Budget:** 1 week · **Took:** ~3 hours · **Result:** yes, and textures need no
import at all

## Criterion

Unattended; extrapolated full bake under 2 h on a mid-range desktop; no editor
step; imported files pack into a `.pck` that mounts.

## Setup

Godot 4.7.2.rc1, Linux, RX 5700 XT (RADV), 12 threads.

Fixtures (`docs/spikes/headless-bake/`, no game data):

- 1,000 synthetic `.glb` statics, 1,007,552 triangles total, with normals, UVs
  and a material.
- 1,000 `.ktx2` (Basis ETC1S + mips), 40 encodes copied to 1,000 names.
- 200 real Skyrim `.dds` from a local install (2.1 GiB, up to 4096²), not
  committed.

## Results

### Import runs headless

```
godot4.7 --headless --import
  wall 26.01 s   peak RSS 778 MB   CPU 96%   exit 0   no manual step
```

Only the `.glb` files were imported (1,000 `.scn` + `.md5` in
`.godot/imported/`). `.ktx2` and `.dds` got no `.import` file: Godot loads them
directly at runtime
(`ResourceLoader.get_recognized_extensions_for_type("Texture2D")` lists `dds`,
`ktx`, `ktx2`).

A `.glb` imports as `.scn` (`PackedScene` with `MeshInstance3D`), not
`.mesh`/`.ctex`.

### DDS keeps its blocks; Basis KTX2 does not

Same numbers with a real Vulkan device and headless:

| Source | Loads without import | In memory | VRAM (1024²) | Load time |
| --- | :---: | --- | ---: | ---: |
| `.dds` BC7 | yes | `BPTC_RGBA` | 1.00 MB | ~1.2 ms |
| `.dds` BC3/BC1 | yes | `DXT5` / `DXT1` | — | ~1.2 ms |
| `.ktx2` Basis ETC1S | yes | `RGBA8` (decompressed) | 5.33 MB | ~36 ms |

Basis KTX2 is transcoded to RGBA8: 5.3× the VRAM and ~30× the load time.

### Modded textures are almost all block-compressed

3,000 of 21,305 loose `.dds` in a modded install:

| Format | Share |
| --- | ---: |
| DXT5 / BC3 | 73.4% |
| BC7 (DX10) | 14.1% |
| DXT1 / BC1 | 9.8% |
| DXT3 / BC2 | 1.5% |
| BC4 | 0.7% |
| uncompressed | 0.5% |

### Pack and mount

```
PCKPacker: 1,200 files -> 2,123 MB .pck
load_resource_pack() from a separate, empty project:  4 ms
  DDS loaded from the pack:            1.23 ms each
  imported .scn loaded from the pack:  instantiates to Node3D > MeshInstance3D
```

Import, pack, mount and load all work without the editor.

## Full-bake estimate

26 ms per mesh, single process (`--import` does not parallelize):

| Meshes | Time |
| ---: | --- |
| 30,000 (about vanilla) | ~13 min |
| 60,000 | ~26 min |
| 150,000 (heavily modded) | ~65 min |

All under 2 h. Running several processes would help if needed.

## Consequences

1. **Desktop textures are DDS passthrough**, not DDS → decode → Basis → KTX2.
   Re-encoding costs hours (basisu ~4.7 s per 2048² texture), loses quality
   (BC3 → BC7 on lossy data) and is slower at runtime.
2. **Mobile/standalone (ASTC) will need real re-encoding**, which is expensive.
3. **Only meshes are baked** (`.glb` → `.scn`).

## Open

Whether KTX2 with an explicit `vkFormat` (e.g. `VK_FORMAT_BC7_UNORM_BLOCK`)
loads with its blocks intact. `headless-bake/dds2ktx2.py` tries it and Godot
rejects the output, but the script's hand-written Data Format Descriptor is
unverified, so the file is probably malformed. Checking needs a reference file
from KTX-Software's `ktx create`. Not blocking: DDS works.
