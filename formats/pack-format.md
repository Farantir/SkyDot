# Pack format v4

The interface between converter and engine. The converter writes packs; the
engine reads only packs. Everything below is stable in v4. The last section
lists what is not promised.

v4 adds LOD: terrain and object LOD (`.btr`, `.bto`) become meshes, and
LOD settings and tree LOD (`.lod`, `.lst`, `.btt`) become `.lodfb` assets of
a new kind, `lod`. v3 decodes scripts: a script asset is a `.pexfb` FlatBuffer instead of the
original `.pex`. v2 added `world.fb` (cells, references and base objects,
decoded and with global FormIDs) and the manifest's `world` key, and corrected
v1's claim that FormIDs inside `records.fb` payloads are global: they never
were.

## Overview

A pack is a directory. Assets are named by a hash of their source bytes;
`vpath.idx` maps game paths to those hashes. `records.fb` holds the load order
already merged into one set of forms, so the engine never sees plugins, mod
indices or overrides. `world.fb` holds what the engine needs to build cells. No file contains a timestamp, and the same input gives a
byte-identical pack.

## Versioning

`k_pack_format_version` (`include/bethconv/pack/vpath_index.hpp`) is 4. It
appears in `manifest.json`, `report.json` and, as `k_snapshot_format_version`,
the `records.fb` header. Each is checked separately so a reader can say which
file it cannot read.

Readers refuse unknown versions; they never read what they recognize and skip
the rest. `Snapshot::open` returns `ErrorKind::unsupported` with both numbers.

Writers bump the version whenever the meaning of anything here changes, even if
parsers would not notice. Settings fingerprints (below) are separate: they
rename assets without changing the format.

## Layout

```
pack/
  manifest.json                      what the pack is and what built it
  records.fb                         the merged world, mmap-able
  world.fb                           cells, references, base objects
  assets/<bb>/<hash>.glb|.dds|.pexfb|.lodfb  content-addressed, two-level fanout
  vpath.idx                          virtual path -> content hash
  report.json                        every skipped, failed and warned input
```

`assets/` always exists; an asset-less pack has an empty `assets/` and a
`vpath.idx` with only its two header lines.

`records.fb` and `world.fb` may be absent (asset-only pack). The `records` and
`world` keys in `manifest.json` decide: if present, the file must exist and
match its `hash`.

There is no `.ktx2`: the headless-bake spike showed DDS passthrough works (see
`docs/spikes/headless-bake.md`).

## `manifest.json`

Keys in this order (`nlohmann::ordered_json`, two-space indent, trailing
newline):

| Key | Content |
| --- | --- |
| `pack_format_version` | `4` |
| `converter` | writer name and version |
| `language` | language used to resolve strings |
| `load_order` | plugin filenames in order |
| `source_hashes` | per mounted archive or directory: `name`, `kind`, `bytes`, optional `hash` |
| `records` | `file`, `forms`, `bytes`, `hash`; absent without a snapshot |
| `world` | `file`, `cells`, `refs`, `bases`, `bytes`, `hash`; absent without a snapshot |
| `assets` | `distinct`, `index_entries`, `meshes`, `textures`, `scripts`, `bytes`, `dedupe_saved_bytes` |
| `deferred` | extension → count of inputs not converted by this version |
| `report` | `file`, `failed`, `warnings` |

`load_order` is provenance only; the engine must not depend on it. A form's
`winner` and `owner` index into it for `bethconv verify` and debugging.

## `records.fb`

```
[ 64-byte header ][ payload blob ][ FlatBuffer ]
```

The blob comes first because it is written while the merge streams; the index
is built after. Payloads are outside the FlatBuffer because FlatBuffers use
32-bit offsets (2 GiB max) and vanilla SE's payloads alone are 527 MiB. The
blob uses 64-bit offsets; only the index must fit.

### Header

Little-endian, 64 bytes, magic `BETHSNAP`, then `format_version`, `fb_offset`,
`fb_bytes`, `blob_offset`, `blob_bytes`, `form_count`, `blob_hash`, and
reserved space for later fields.

`blob_hash` is FNV-1a over the blob. It detects truncation, not tampering.
`verify --deep` checks it; `open` does not.

The FlatBuffer has its own identifier, `BSN1`, 64 bytes in. The outer magic
names the container, the inner one the schema.

### Schema

`src/pack/records.fbs` is normative. `Form` is a struct (47 MiB instead of
~130 MiB as tables at 1.18M forms), so editor ids and payloads are referenced
from it. `forms` is sorted by `id`, unique and binary-searchable.

Indices: `forms` (id → form), `editor_ids`/`editor_id_forms` (parallel arrays
sorted by name), `types` (type → sorted forms), `children` (parent → sorted
forms), `worlds` (worldspace → exterior cells sorted by
`(int64(x) << 32) | uint32(y)`).

`children` duplicates each form's `parent` so cell contents need no scan.

### Payloads are the record's field block, verbatim

A form's payload is the winning record's entire field block: every field in file
order with its header, decoded or not.

- Undecoded fields are present and findable by tag. There are many: REFR alone
  has 43 undecoded field types (`XRGD` 8,601 occurrences, `INAM` 4,525), CELL
  9, WRLD 11. Keeping them means decoding them later needs no format change.
- Compressed records are stored inflated.

Payloads are not normalized. FormIDs inside them are plugin-local, as written
by the winning plugin, and the pack does not carry the master lists needed to
resolve them; use `world.fb` for anything that needs resolved references.
(Only each form's own id and `parent` are global.) String fields keep the
`.STRINGS` index; `manifest.json`'s `language` names the table, which the pack
does not include.

`tests/unit/test_snapshot.cpp` checks this for compressed and uncompressed
records carrying `XRGD`, `VMAD`, `XAPR` and an unknown tag. A writer that kept
only decoded fields would pass every other snapshot test.

## `world.fb`

A plain FlatBuffer (identifier `BWD1`), schema `src/pack/world.fbs`, with its
own `format_version` (4; 3 added scripts, locks, linked refs, activate parents,
primitives and base flags, 4 quests, globals, placed actors and plugins). Written during a merge pass, so every FormID in it is
global: resolved through the winning plugin's master list.

- `cells`, sorted by id: editor id, worldspace (0 for interiors), DATA flags,
  grid, water height, decoded XCLL lighting (92-byte form only), lighting
  template, the cell's references (sorted by id) and load doors, its terrain,
  and whether it is its worldspace's persistent cell.
- A reference: base, position and rotation in Skyrim space (Z-up, game units,
  radians), scale, flags (initially disabled, persistent, enable-opposite,
  activated only by its activate parents), enable parent. Deleted references
  are omitted. Placed actors (ACHR) are listed separately.
- Per cell, sorted by reference: scripts (VMAD), locks (XLOC), linked
  references (XLKR), activate parents (XAPR) and primitive volumes (XPRM).
- Scripts, on references and bases: name, status, and properties by name with
  their type, status and values. Object properties are global FormIDs, with an
  alias index when they name a quest alias. Status as stored (UESP: 0 local,
  1 inherited, 2 removed, 3 inherited and removed).
- `bases`, sorted by id: every record with a model (`MODL`; for ARMO the world
  model `MOD2`, else `MOD4`, since its `MODL` is an armature FormID) as a
  normalized virtual path (`meshes/...`) or with scripts, and every LIGH with
  its DATA/FNAM values. DOOR bases carry their flags (automatic, hidden,
  minimal use, sliding). INFO, PACK, SCEN and PERK scripts are not included:
  they need fragment data and the systems that run it.
- `quests`, sorted by id: QUST's flags, priority, type, story manager event,
  scripts, the fragment script with its stage fragments (sorted by stage and
  log entry), stages (sorted) with their journal entries, objectives with the
  aliases they target, and aliases in fill order: name, flags, how they are
  filled (forced reference or location, unique actor, another quest's alias,
  created object; else a condition count) and their scripts. Conditions are
  counted, not carried.
- `globals`, sorted by id: GLOB's kind and value.
- `actors`, sorted by reference: ACHR's NPC_, cell, position, rotation and
  flags (as a reference's).
- `plugins`, in load order: each active plugin's name and FormID prefix
  (`0xII000000`, or `0xFEIII000` for a light plugin), so scripts can name a
  form by plugin and object id.
- `worlds`, sorted by id: WRLD with parent and parent flags (bit 0: the land
  comes from the parent), DATA flags, default land and water height, water
  type and bounds.
- `land_textures`, sorted by id: LTEX with its TXST's diffuse and normal map
  as virtual paths, and its specular value.
- `waters`, sorted by id: WATR's visual values (opacity, shallow, deep and
  reflection colours, fresnel, reflectivity, fog distance, three noise layers
  with wind direction, speed, scale and amplitude) and its noise textures.
  Cells name theirs (XCWT); 0 means the worldspace's.
- `climates` and `weathers`, sorted by id: CLMT's weather chances and sun
  times (hours); WTHR's colour table (17 colours by sunrise, day, sunset,
  night), fog distances and directional ambient. Worldspaces name their
  climate.
- A cell's terrain (LAND): VHGT's height offset and 33 x 33 deltas as stored
  (heights are 8 units per step, summed down column 0 and then along each
  row), VCLR vertex colours, and per quadrant a base layer and additional
  layers with their VTXT opacity (vertex index in the quadrant's 17 x 17 grid,
  opacity 0..255). Normals are not stored: computed from the heights they
  match VNML to about a degree.

A worldspace's persistent cell has grid (0, 0) like the real cell there, but
holds the persistent references of the whole worldspace; place those by
position.

A reference whose base is not in `bases` places something without a model
(markers without meshes, sounds, etc.). The engine converts coordinates: the
same Z-up → Y-up rotation and 0.0142875 m per unit the mesh writer applies.

## `assets/` and the content hash

Name: `BLAKE3-256(source bytes ‖ converter version ‖ settings)` as 64 lowercase
hex digits, stored at `assets/<first two hex>/<hex><ext>`.

Each component is prefixed with its 64-bit length; otherwise bytes `ab` +
settings `c` would hash like `a` + `bc`.

Hashing the input means dedupe happens before conversion (a duplicate mesh is
never parsed), and names only change when the settings string says the output
changed.

### Settings fingerprints

Per kind, mentioning only settings that affect that kind:

| Kind | Extension | Fingerprint | Current |
| --- | --- | --- | --- |
| mesh | `.glb` | `mesh/<n>;` flags, unit scale as `%.9g` | `mesh/9` |
| texture | `.dds` | `texture/<n>;` flags | `texture/1` |
| script | `.pexfb` | `script/<n>;decoded` | `script/2` |
| lod | `.lodfb` | `lod/<n>;decoded` | `lod/1` |

`%.9g` keeps floats identical across machines. The leading number is bumped
whenever a writer's output changes: `mesh/2` percent-encoded control bytes in
texture paths, `mesh/3` kept non-finite floats out of JSON, `mesh/8` added
controllers, particle systems and hidden nodes to the extras
([`format-notes/nif-animation.md`](format-notes/nif-animation.md)), `mesh/9`
every drag modifier with its axis; the list in
`ConvertOptions::mesh_settings` has the rest. Without a bump, dedupe would keep
reusing stale assets.

Fingerprint bumps rename assets; format bumps make readers refuse the pack.

## Script assets

A `.pexfb` is one compiled Papyrus script, decoded: a FlatBuffer (identifier
`BPX1`, schema `src/pack/script.fbs`, its own `format_version`, 1) holding the
PEX's string table, user flags and objects with their variables, properties,
states and functions. Names stay indices into the string table; docstrings are
dropped. Each function keeps its instructions as opcode bytes and a flat
argument list (a call's count is implied by the next instruction's offset) and
its source line numbers when the PEX had debug info.

The converter decodes every `.pex` completely and refuses one that is wrong
anywhere: a string index outside the table, an unknown opcode or value type, a
jump outside its function, an object size that disagrees with its content, or
bytes left over. The FlatBuffer verifier only checks structure, so a reader
must still bounds-check indices; a pack written by this converter never
violates them. Scripts keep their virtual paths (`scripts/defaultlever.pex`),
so class `DefaultLever` is found at `scripts/defaultlever.pex`.

All 10,015 LE, 14,302 SE and 13,507 VR scripts and the FUS list's 5,352
decode; the corpus harness pins the counts and a hash over the assets.

## LOD

A worldspace's distant LOD, as the game ships it (for Tamriel):

- `lodsettings/tamriel.lod` → a LOD asset with `settings`: the south-west
  cell of the LOD grid, how many cells it spans and the finest and coarsest
  quad size (4 and 32 cells in vanilla).
- `meshes/terrain/tamriel/tamriel.L.X.Y.btr` → a mesh: the terrain of the
  quad of level L whose south-west cell is (X, Y). Its vertices are relative
  to that corner (the NIF has no translation for it) and it is textured by
  `textures/terrain/tamriel/tamriel.L.X.Y.dds` with a model-space normal map
  (`…_n.dds`). A second shape at a fixed height is the LOD water.
- `meshes/terrain/tamriel/objects/tamriel.L.X.Y.bto` → a mesh: the objects of
  that quad, placed in world space by its own node transform.
- `meshes/terrain/tamriel/trees/tamriel.lst` → a LOD asset with `tree_types`:
  billboard sizes and their rectangles in
  `textures/terrain/tamriel/trees/tamrieltreelod.dds`.
- `meshes/terrain/tamriel/trees/tamriel.4.X.Y.btt` → a LOD asset with
  `trees`: the tree instances of a level-4 quad, in world space.

A `.lodfb` is a FlatBuffer (identifier `BLD1`, schema `src/pack/lod.fbs`, its
own `format_version`, 1). The source layouts are in
`include/bethconv/pack/lod_asset.hpp`; the converter refuses a file with
counts larger than itself, levels that are not powers of two, or bytes left
over, except after a `.btt`'s declared blocks (six Solstheim files in SE and
VR; skipped with a warning). Every LOD file of the three vanilla installs
converts.

## `vpath.idx`

Tab-separated, sorted, one line per virtual path, after two comment lines:

```
# bethconv vpath index v4
# virtual path\tcontent hash\tkind\twinning source
meshes/clutter/apple01.nif\t3f9c…\tmesh\tSkyrim - Meshes0.bsa
```

`kind` is `mesh`, `texture`, `script` or `lod`. An unknown kind means a newer writer;
say so rather than guess.

Text because it is for debugging mod overrides: people read it and tools diff
it. About 20 MB for vanilla SE, 3% of the pack.

Several paths mapping to one hash is normal (dedupe); each gets a line.

`include/bethconv/pack/vpath_index.hpp` implements the format and both writer
and reader use it.

### Path encodings differ between `vpath.idx` and GLB URIs

In `vpath.idx` a path is unescaped: lowercase, forward slashes.

In a GLB it is a URI reference (RFC 3986 §4.2), produced in three steps:

1. `to_uri`: backslashes to slashes, lowercase (the `vpath.idx` form).
2. `uri_escape`: percent-encode everything outside `pchar` except `/`. `:` is
   escaped too, because a colon in the first segment would read as a scheme.
   49 vanilla texture paths contain spaces; 67 vanilla GLBs referenced a
   FaceGen slot literally named `textures\<0x08>NOR`, now `textures/%08nor`.
3. `shield_percents`: `%` → `%25`. `fastgltf::URI` decodes escapes on
   construction and writes the decoded form, so this cancels that decode.

To get the `vpath.idx` key from a URI: undo the doubling, then percent-decode
once. When comparing, escape before, never after.

GLBs placed outside the pack root also carry a `../` prefix for their depth.
`pack_view.cpp` is a consumer that uses only what this document promises.

## `report.json`

Same formatting as the manifest. Keys: `pack_format_version`, `totals`,
`bytes`, `assets`, `failures` (uncapped; `vpath`, `stage`, `kind`, `detail`),
`warnings`, `deferred`.

`failures` is uncapped because large broken load orders are exactly where it is
needed. `kind` is an `io::ErrorKind` name, for counting by class. Warnings do
not fail a file and are only visible here.

## Bake

`tools/bake/` runs headless Godot over a pack and produces a `.pck`. It builds on
the pack; a pack is complete without it.

- A `.glb` imports as a `.scn` (`PackedScene` with `MeshInstance3D`). Only
  meshes are imported: Godot loads `.dds`, `.ktx` and `.ktx2` directly, so
  textures pass through.
- A `.pck` is keyed by virtual path: `clutter/apple01.nif` becomes
  `res://meshes/clutter/apple01.scn`, so the engine derives a scene path from a
  base record's MODL without a lookup.

A `.pck` is not reproducible: Godot assigns resource ids without a seed, and
two bakes differ by about 20 bytes per scene. Test a `.pck` by loading it; pin
the pack.

## Determinism

- No wall-clock time anywhere in a pack. Timings go to the terminal.
- No hash-table iteration order in files. `vpath.idx` and all `report.json`
  arrays are sorted.

## Not promised in v4

- **Resolved text.** There is no per-language string file; names cannot be
  resolved from a pack alone.
- **A view.** `bethconv view` builds a derived tree for glTF-only consumers. It
  is optional; put nothing in a pack that only the view can find.
- **Decoded fields.** Only that the bytes are present.
- **All Havok shapes.** `bhkCylinderShape`, `bhkNiTriStripsShape` and
  `bhkPlaneShape` are `CollisionKind::unsupported` with their block name.
  Consumers must handle that kind.
- **All image references resolving.** 572 of 120,313 in vanilla SE point to
  files the game does not ship. `bethconv view` lists them.
- **Finite binary data.** Non-finite floats are kept out of JSON, but a NaN
  vertex reaches the BIN chunk unchanged. Renderer behavior is untested.
- **Migration.** A version mismatch is refused; there is no upgrade tool.

## Code

| Piece | Implementation |
| --- | --- |
| layout, dedupe, manifest, report | `include/bethconv/pack/pack_writer.hpp` |
| `vpath.idx`, asset paths, kinds | `include/bethconv/pack/vpath_index.hpp` |
| content hash | `include/bethconv/pack/content_hash.hpp` |
| `records.fb` container and reader | `include/bethconv/pack/snapshot.hpp` |
| `records.fb` schema | `src/pack/records.fbs` |
| `world.fb` writer and reader | `include/bethconv/pack/world.hpp` |
| `world.fb` schema | `src/pack/world.fbs` |
| script assets | `include/bethconv/pack/script_asset.hpp`, `src/pack/script.fbs` |
| PEX decoding | `include/bethconv/script/pex.hpp` |
| consumer using only this document | `src/pack/pack_view.cpp` |
| complete pack without game data | `tools/testpack/` (`bethconv-testpack`) |
| pack loaded in Godot | `tools/bake/` |
