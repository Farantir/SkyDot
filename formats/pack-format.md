# Pack format v5

The interface between converter and engine. The converter writes packs; the
engine reads only packs. Everything below is stable in v5. The last section
lists what is not promised.

v5 stores assets in one blob with an index (or, on request, as loose files)
and drops glTF images from meshes: the engine loads packs directly, without a
Godot import or bake. v4 adds LOD: terrain and object LOD (`.btr`, `.bto`) become meshes, and
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

`k_pack_format_version` (`converter/include/bethconv/pack/vpath_index.hpp`) is 5. It
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
  assets.idx                         content hash -> offset, size, kind
  assets-0001.blob                   every asset (.glb, .dds, .pexfb, .lodfb bytes)
  vpath.idx                          virtual path -> content hash
  report.json                        every skipped, failed and warned input
```

With `--store loose`, `assets.idx` and the blob are replaced by
`assets/<bb>/<hash><ext>`, one file per asset in a two-level fanout. The
manifest's `store` key says which; readers support both. An asset-less pack
has an index with no entries and an empty blob (or an empty `assets/`).

`records.fb` and `world.fb` may be absent (asset-only pack). The `records` and
`world` keys in `manifest.json` decide: if present, the file must exist and
match its `hash`.

There is no `.ktx2`: the headless-bake spike showed DDS passthrough works (see
`converter/docs/spikes/headless-bake.md`).

## `manifest.json`

Keys in this order (`nlohmann::ordered_json`, two-space indent, trailing
newline):

| Key | Content |
| --- | --- |
| `pack_format_version` | `5` |
| `converter` | writer name and version |
| `language` | language used to resolve strings |
| `textures` | how textures were converted: `max_size` (largest side in pixels, 0 for full size; larger textures lost their top mip levels), `complete_mip_chains`, and `uncompressed` (`keep`, `bc7` or `compact`: what happened to textures the game stores uncompressed; BC7 files carry a DX10 header, format 98); absent when textures were not converted |
| `input` | optional: what the pack was converted from, so it can be converted again: `kind` (`data` or `mo2`), `edition`, `data` (the Data folder), `plugin_list`, and for `mo2` also `mo2_instance`, `mo2_profile`, `mods`. Local paths; nothing reads it but front ends |
| `load_order` | plugin filenames in order |
| `source_hashes` | per mounted archive or directory: `name`, `kind`, `bytes`, optional `hash` |
| `records` | `file`, `forms`, `bytes`, `hash`; absent without a snapshot |
| `world` | `file`, `cells`, `refs`, `bases`, `bytes`, `hash`; absent without a snapshot |
| `assets` | `distinct`, `index_entries`, `meshes`, `textures`, `scripts`, `lod`, `bytes` (written by this run), `dedupe_saved_bytes` |
| `store` | `layout` (`blob` or `loose`); for a blob also `index` and `blob` (file names); `bytes` (all stored assets) |
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

The payload blob follows the header at `blob_offset` 64; the FlatBuffer (the
index) follows the blob at `fb_offset`, padded with zero bytes to a multiple of
8, because FlatBuffers reads its fields in place. A reader refuses a
misaligned `fb_offset` (snapshots written before 2026-10-01 have one). The
FlatBuffer has its own identifier, `BSN1`; the outer magic names the
container, the inner one the schema.

### Schema

`formats/schema/records.fbs` is normative. `Form` is a struct (47 MiB instead of
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

A plain FlatBuffer (identifier `BWD1`), schema `formats/schema/world.fbs`, with its
own `format_version` (6; 3 added scripts, locks, linked refs, activate parents,
primitives and base flags, 4 quests, globals, placed actors and plugins, 5
navmeshes, 6 cloud layers, weather data, precipitation and weather
regions). Written during a merge pass, so every FormID in it is
global: resolved through the winning plugin's master list.

- `cells`, sorted by id: editor id, worldspace (0 for interiors), DATA flags,
  grid, water height, decoded XCLL lighting (92-byte form only), lighting
  template, the cell's references (sorted by id) and load doors, its terrain,
  and whether it is its worldspace's persistent cell.
- A reference: base, position and rotation in Skyrim space (Z-up, game units,
  radians), scale, flags (initially disabled, persistent, enable-opposite,
  activated only by its activate parents), enable parent. Deleted references
  are omitted. Placed actors (ACHR) are listed separately.
- Per cell, sorted by id: its navmeshes (NAVM's NVNM): vertices in Skyrim
  space, triangles with their neighbours per edge and flags (water, door,
  preferred), edge links into other navmeshes (portals across cell borders,
  ledges), and the triangles in front of doors. The search grid and the
  cover triangle list are left out.
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
- `climates` and `weathers`, sorted by id: CLMT's weather chances, sun
  times (hours), sun and glare textures, night sky model, volatility and
  moons with their phase length; WTHR's colour table (17 colours by sunrise,
  day, sunset, night), fog distances, directional ambient, its 29 cloud
  layers (texture, speed, colour and alpha by time of day, enabled; layer i
  is drawn on the i-th shape of `meshes/sky/clouds.nif`), DATA (wind,
  transition, sun glare and damage, precipitation and thunder timing,
  classification, lightning colour), its precipitation and aurora model.
  Worldspaces name their climate.
- `precipitations`, sorted by id: SPGD, rain or snow particles (speeds,
  sizes, atlas, box size, density; units mostly undocumented).
- `regions`, sorted by id, only those with a weather list (REGN RDAT type 3):
  worldspace, polygons in game units, weathers with chance and gating
  global, priority and override flag.
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

## Asset store

Two layouts hold the same assets under the same names.

**Blob** (the default). `assets.idx`, little-endian:

| Field | Type |
| --- | --- |
| magic | `BCAI` |
| version | u32, 1 |
| generation | u32: the blob is `assets-<generation, 4 digits>.blob` |
| count | u32 |
| blob_bytes | u64: bytes of the blob the index covers |
| entries | count × { hash: 32 bytes, offset: u64, size: u64, kind: u8, 7 zero bytes } |

Entries are sorted by hash bytes; kind is 0 mesh, 1 texture, 2 script, 3 lod.
Every entry lies inside `blob_bytes`, and nothing follows the last entry; a
reader refuses an index that breaks either. Entries start on 16-byte
boundaries, in conversion order (sorted virtual paths), so the blob is
byte-identical for the same input.

A run appends to the blob and replaces `assets.idx` at the end (write, then
rename). Bytes past `blob_bytes` are left by an interrupted run; the next run
truncates them. `--prune` copies the referenced entries into the next
generation's blob, switches the index to it, then deletes the old blob.

The blob exists because packs are written to whatever disk has room. About
70,000 loose files on an SMR disk behind ntfs-3g once hung the whole mount
until a hard reset; one large file is written sequentially, and engines map it.

**Loose** (`--store loose`): `assets/<first two hex>/<hex><ext>`.

### The content hash

Name: `BLAKE3-256(source bytes ‖ converter version ‖ settings)` as 64 lowercase
hex digits.

Each component is prefixed with its 64-bit length; otherwise bytes `ab` +
settings `c` would hash like `a` + `bc`.

Hashing the input means dedupe happens before conversion (a duplicate mesh is
never parsed), and names only change when the settings string says the output
changed.

### Settings fingerprints

Per kind, mentioning only settings that affect that kind:

| Kind | Extension | Fingerprint | Current |
| --- | --- | --- | --- |
| mesh | `.glb` | `mesh/<n>;` flags, unit scale as `%.9g` | `mesh/13` |
| texture | `.dds` | `texture/<n>;` flags, `;max=<px>` when limited, `;encode=<mode>` unless `keep` | `texture/1` |
| script | `.pexfb` | `script/<n>;decoded` | `script/2` |
| lod | `.lodfb` | `lod/<n>;decoded` | `lod/1` |

`%.9g` keeps floats identical across machines. The leading number is bumped
whenever a writer's output changes (a changed option already changes its
fingerprint, e.g. `refs=` when pack meshes lost their images): `mesh/2` percent-encoded control bytes in
texture paths, `mesh/3` kept non-finite floats out of JSON, `mesh/8` added
controllers, particle systems and hidden nodes to the extras
([`format-notes/nif-animation.md`](format-notes/nif-animation.md)), `mesh/9`
every drag modifier with its axis, `mesh/10` rigid body fields in the
collision extras (below), `mesh/11` compressed-mesh triangles that follow a
chunk's strips (they were dropped), `mesh/12` cylinder, strips and plane
collision shapes, `mesh/13` the texture of sky shaders (stars); the list in
`ConvertOptions::mesh_settings` has the rest. Without a bump, dedupe would keep
reusing stale assets.

Fingerprint bumps rename assets; format bumps make readers refuse the pack.

## Script assets

A `.pexfb` is one compiled Papyrus script, decoded: a FlatBuffer (identifier
`BPX1`, schema `formats/schema/script.fbs`, its own `format_version`, 1) holding the
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

A `.lodfb` is a FlatBuffer (identifier `BLD1`, schema `formats/schema/lod.fbs`, its
own `format_version`, 1). The source layouts are in
`include/bethconv/pack/lod_asset.hpp`; the converter refuses a file with
counts larger than itself, levels that are not powers of two, or bytes left
over, except after a `.btt`'s declared blocks (six Solstheim files in SE and
VR; skipped with a warning). Every LOD file of the three vanilla installs
converts.

## `vpath.idx`

Tab-separated, sorted, one line per virtual path, after two comment lines:

```
# bethconv vpath index v5
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

### Texture references in meshes

A pack's GLBs have no `images` or `textures`: glTF resolves image URIs against
the document, which a content-addressed store has no layout for, and Godot's
runtime glTF loader fails on URIs it cannot open. Each material names its
textures in `extras.bethconv.texture_slots` (`"0"` diffuse, `"1"` normal,
`"2"` glow, and so on, each with a `role` and a `path` in the `vpath.idx`
form). The engine resolves those itself.

`bethconv view` adds glTF images back for other consumers: slot 0 as base
colour, slot 1 as normal map unless `model_space_normals`, slot 2 as emissive
when `has_glowmap`, with URIs relative to the view.

### Collision in meshes

Havok collision is never glTF geometry (which would render). The node a
`bhkCollisionObject` hangs off carries `extras.bethconv.collision`, an array
with one entry per leaf shape (MOPP trees, list and transform shapes are
unwrapped):

| Key | Meaning |
| --- | --- |
| `kind` | `box`, `sphere`, `capsule`, `cylinder`, `convex_vertices`, `compressed_mesh`, `mesh`, or `unsupported` |
| `block`, `node` | Havok block name; the owning node's name |
| `layer` | Skyrim collision layer (1 static, 2 animated static, 4 clutter, 13 terrain, 15 non-collidable, ...) |
| `motion_type`, `quality_type` | `hkMotionType`; `hkpCollidableQualityType` (0 fixed, 1 keyframed, 2 to 7 moving, 8 character, 9 keyframed reporting) |
| `mass`, `friction`, `restitution` | the rigid body's values (mass in kg) |
| `havok_material` | the shape's material id |
| `transform` | `translation`, `rotation` (quaternion): a `bhkRigidBodyT`'s transform composed with every transform shape above the leaf |
| `half_extents` | box |
| `radius` | sphere, capsule, cylinder; a box's or convex hull's convex radius |
| `point_a`, `point_b` | capsule and cylinder end points (a cylinder's convex radius is already added to its radius and ends) |
| `vertices` | flat `x, y, z` list: convex hull points, or mesh vertices |
| `indices` | mesh triangles, three per triangle. `compressed_mesh`: big triangles, then per chunk its strips and the plain list after them; `mesh` (`bhkNiTriStripsShape`): its strips parts' triangles |

A `bhkPlaneShape` (one vanilla mesh) becomes `convex_vertices`: the flat
polygon where the plane cuts its bounding box. Flat hulls need thickening in
engines that cannot build a hull without volume, as Havok's do through their
convex radius.

Everything is in the owning node's frame but in **Havok units**: multiply
lengths (including translations) by 69.99124 to get game units, the node's
own unit. `motion_type` is unreliable on its own: vanilla static architecture
often says `5` (box stabilized); `quality_type` is the one to read. Vanilla SE:
static architecture is layer 1 with quality 0, animated statics layer 2 with
quality 1, clutter layer 4 with quality 4. (nif.xml numbers the quality from 1
for fixed; the data does not fit that.)

### Path encodings differ between `vpath.idx` and GLB URIs

In `vpath.idx` and in extras a path is unescaped: lowercase, forward slashes.

In a GLB written outside a pack (`bethconv mesh`), or added by the view, it is
a URI reference (RFC 3986 §4.2), produced in three steps:

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

## Loading in an engine

No import step. The engine maps the blob, reads a mesh's GLB bytes and builds
its scene with a runtime glTF loader (Godot's `GLTFDocument`), and loads DDS
bytes as they are, block-compressed. Measured on 4,115 Riverwood meshes: about
1 ms per mesh against 1.7 ms for a baked `.scn`, 0.1 ms per texture.

Earlier versions baked packs into a Godot `.pck` through `--import`. It read
about 16 times its input and wrote five small files per mesh, which on a slow
disk took longer than the conversion and on an SMR disk behind ntfs-3g hung
the machine.

## Determinism

- No wall-clock time anywhere in a pack. Timings go to the terminal.
- No hash-table iteration order in files. `vpath.idx` and all `report.json`
  arrays are sorted.

## Not promised in v5

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

Paths relative to `converter/`; schemas in `formats/schema/`.

| Piece | Implementation |
| --- | --- |
| layout, dedupe, manifest, report | `include/bethconv/pack/pack_writer.hpp` |
| `assets.idx`, the blob, loose files | `include/bethconv/pack/asset_store.hpp` |
| `vpath.idx`, asset paths, kinds | `include/bethconv/pack/vpath_index.hpp` |
| content hash | `include/bethconv/pack/content_hash.hpp` |
| `records.fb` container and reader | `include/bethconv/pack/snapshot.hpp` |
| `records.fb` schema | `formats/schema/records.fbs` |
| `world.fb` writer and reader | `include/bethconv/pack/world.hpp` |
| `world.fb` schema | `formats/schema/world.fbs` |
| script assets | `include/bethconv/pack/script_asset.hpp`, `formats/schema/script.fbs` |
| PEX decoding | `include/bethconv/script/pex.hpp` |
| consumer using only this document | `src/pack/pack_view.cpp` |
| complete pack without game data | `tools/testpack/` (`bethconv-testpack`) |
| pack loaded in Godot | `engine/extension/src/assets/` |
