# Corpus harness

Runs the converter over real game installs and compares the results with
`corpus-expectations.json`. The file holds only counts, versions, flags, hashes
and the paths of entries known to be corrupt, never game data.

## Running

Point one or more variables at a `Data` folder:

```sh
export SKYRIM_DATA_LE="…/steamapps/common/Skyrim/Data"
export SKYRIM_DATA_SE="…/steamapps/common/Skyrim Special Edition/Data"
export SKYRIM_DATA_VR="…/steamapps/common/SkyrimVR/Data"

ctest --preset linux-debug-asan -R corpus
```

Unset installs are skipped with a warning and the run passes, which is what CI
does. The last line shows what was actually tested:

```
corpus harness: visited 3 install(s) skyrim-le skyrim-se skyrim-vr
```

Set `BETHCONV_CORPUS_REQUIRE=1` to make "visited nothing" a failure.

All three installs take about 410 s under ASan. The merge pin is the largest
part: it merges each install and writes `records.fb` and `world.fb` from the
result, reopens both and hashes their bytes (the two writers are
deterministic, so a hash moves only when their output does). The field
definitions and plugin walks come next.

Beyond the merge, the pins cover the LOD pass: `terrain-lod` converts
Tamriel's level-32 terrain LOD and its textures, `tree-lod` its tree list,
tree blocks and atlas.

## Regenerating expectations

After an intended behavior change:

```sh
./build/linux-debug-asan/tests/corpus/bethconv-corpus-snapshot \
    > tests/corpus/corpus-expectations.json
```

The existing file is also the input: it lists installs, plugins, archives and
known-bad entries. Only measured values are replaced; installs that are not
configured keep their old numbers.

Read the diff. Be suspicious of an unintended `records` or
`type_histogram_hash` change, and of a shrinking `expected_read_failures`
(corrupt entries do not heal).

## When the game is patched

Steam updates the installs, so a red run can mean the data changed.
`file_bytes` (each plugin's size) can only change with the data, and a mismatch
prints:

```
tests/corpus/test_corpus.cpp:135: warning:
  Dragonborn.esm is 64701824 bytes on disk, expected 64664159: this install has
  changed since the expectations were generated. Failures below are the data
  moving, not the reader -- see tests/corpus/README.md.
```

If present, regenerate. If not, the change is yours. It is a `CHECK`, so the
rest of the run still reports.

This happened on 2026-08-28 (SE buildid 24914197). Only one assertion failed:
the `payload_hash` of SE form `0x0010E38C`, whose size stayed 307 bytes. All
counts and histogram hashes stayed green, so only the byte-level form pin caught
it. `file_bytes` was added afterwards; a patch that rewrites payloads without
changing any size would still pass it. Manual check:

```sh
ls -la --time-style=long-iso "$SKYRIM_DATA_SE"/*.esm
python3 -c "import re,datetime;s=open('.../appmanifest_489830.acf').read();\
print(datetime.datetime.fromtimestamp(int(re.search(r'\"LastUpdated\"\s+\"(\d+)\"',s).group(1))))"
```

Timestamps newer than the expectations mean the data moved.

Regenerate with every install configured; otherwise unconfigured ones keep stale
values.

## What is pinned

| Field | Catches |
| --- | --- |
| `file_bytes` | the install being patched |
| `records`, `groups`, `compressed_records` | GRUP walk drift |
| `hedr_record_count` == records + groups | skipped groups or double visits |
| `errors` (must be 0) | vanilla failing to parse |
| `type_histogram_hash` | any per-type count |
| `record_types` | a type appearing or vanishing |
| `is_master` / `is_light` / `masters` | header flag handling |
| `field_coverage_bp` (minimum) | coverage regressions |
| archive `version`, `files` | v104/v105 handling, mount index |
| `unique_paths`, `shadowed` | path normalization and precedence |
| `expected_read_failures` | known-corrupt vanilla entries, both ways |
| texture `format`, `kind`, `width`/`height`, `faces`, `stored_levels`, `full_chain_levels`, `declared_bytes` | DDS header arithmetic |
| texture `outcome`, `output_bytes`, `output_hash` | bytes written by the mip fix |
| mesh `flavor`, `nif_stream` | which reader path ran |
| mesh `nodes`, `primitives`, `materials`, `skins`, `collision`, `joints`, `vertices`, `triangles` | dropped shapes, joints or collision |
| mesh `with_normals` / `with_tangents` / `with_uvs` / `with_colors` / `with_joints` | dropped attributes |
| mesh `warnings` | new warnings, or a Havok shape starting to decode |
| mesh `glb_bytes`, `glb_hash` | any change in writer output |
| `load_order.plugins` (name, index, flags, masters) | plugin placement |
| `load_order.normal_used` / `light_used` | slot usage |
| load-order problems (must be 0) | missing masters, ordering violations |
| merge `plugins`, `visited`, `form_count`, `collapsed`, `deleted`, `injected` | the collapse |
| merge `errors` / `unparented` / `unreadable` (must be 0) | merge failures |
| merge `unresolved` / `problems` (both 1) | a second broken reference |
| merge `type_counts_hash` | per-type totals after merging |
| merge `forms[].winner` / `owner` / `overrides` / `parent` / `flags` | provenance |
| merge `forms[].payload_bytes` / `payload_hash` | a different plugin winning |
| merge `forms[].editor_id_hash` / `name_hash` / `name_from_table` | string resolution, hashed |
| forms `failed` / `leftover` / `leftover_fields` / `unreadable` (must be 0) | definitions misreading bytes |
| forms `parsed`, `types_seen`, `type_counts_hash` | which records definitions ran on |
| forms `unhandled` / `unhandled_fields` / `unhandled_hash` | undecoded fields, by count and content |
| forms `string_tables` / `table_entries` / `resolved_strings` | that tables were actually mounted |
| forms `unresolved_strings` | indices missing from a mounted table |
| mesh `images` / `escaped_uris` | URI escaping |
| mesh / convert `glb_control_bytes` (must be 0) | control bytes in glTF JSON |
| mesh / convert `glb_json_parses` / `glb_unparseable` | JSON a strict parser rejects |
| convert `unique_paths`, `considered` | the filtered subset |
| convert `inputs`, `converted`, `deduped`, `deferred`, `deferred_kinds` | what was converted or skipped |
| convert `meshes` / `textures` / `scripts` | dispatch by kind |
| convert `distinct_assets`, `index_entries`, `source_bytes`, `asset_bytes`, `dedupe_saved_bytes` | pack bookkeeping |
| convert `failed` / `orphaned_assets` (must be 0) | failures, unindexed assets |
| convert `index_hash` | any asset hash changing |
| convert `manifest_hash` | plugin hashes (data-only, like `file_bytes`) |
| convert `report_hash` | warning text |
| view `failed` (must be 0) | incomplete view |
| view `image_refs` / `image_refs_resolved` / `image_refs_dangling` | escape/unescape agreement |
| view `index_entries`, `considered`, `meshes` / `textures` / `scripts` | filtered view contents |
| view `pulled_in` | textures added because a selected mesh needs them |
| view `written`, `mesh_links`, `linked`, `copied`, `bytes` | how entries were materialized |

## Texture pins

A full sweep takes 57 s for SE alone, so each install pins a few files covering
every branch: each pixel format, small and large cubemaps, the volume texture, a
single-level file, chains short by one and by five, and a path with a space (13
for SE, 9 for LE, 6 for VR). This catches arithmetic changes, not formats that
only occur in the unpinned files. Full sweep:

```sh
bethconv texture --inspect -q --source "$SKYRIM_DATA_SE"/Skyrim\ -\ Textures?.bsa
```

## Mesh pins

A full sweep takes 454 s (LE) and 603 s (SE), so each install pins 10–12 files:
LE and SE geometry, heavy skinning, a mesh without geometry, each collision
kind, vertex colors, a LOD mesh, a path with a space, the LE-format file in SE,
and the files producing warnings. About 6 s. Disabling collision, skinning or
`COLOR_0`, or changing the unit scale in its seventh digit, each fail the test.

Large files were tried and dropped: they cost 55 s and exercise no extra code.
Full sweep:

```sh
bethconv mesh --inspect --source "$SKYRIM_DATA_SE"/Skyrim\ -\ Meshes?.bsa
```

Two pinned meshes share a path in LE and SE: same IR counts, different GLB
hashes (SE uses half floats).

## Convert pin

A full `convert` is too slow under ASan, so two filtered runs per install are
pinned, with the filter stored next to the results:

- **`every-kind`** (`--filter critters`): all three asset kinds, `.hkx` as a
  deferred kind, five paths with spaces, two meshes with a control byte in a
  texture slot. 58 / 237 / 73 inputs.
- **`spaced-texture-slots`** (`--filter robedarkbrotherhood`): 48 meshes whose
  texture slots contain spaces plus the 13 textures they use, so 96 escaped URIs
  resolve. Identical on all installs.

About 13 s total. `records.fb` is not written; the merge and snapshot are pinned
separately.

The converter string is fixed to `bethconv-corpus`, not the version, because
asset names include it and every release would otherwise change every hash.
Mesh, texture and script settings are spelled out for the same reason.

### Assertions over every emitted GLB

- **No byte below 0x20 in a JSON chunk.** 67 vanilla SE meshes had a slot named
  `textures\<0x08>NOR`, and Blender rejected them.
- **Every JSON chunk parses strictly.** fastgltf writes non-finite floats as
  out-of-range numbers without an error (`meshes/actors/male/greybeardstatic.nif`).
  Python's `json` accepts those; `nlohmann` rejects them, so it is used here.

`glb_checked` is recorded so a run that checked nothing cannot pass.

Each install also pins the two meshes that triggered these bugs:
`meshes/actors/character/facegendata/facegeom/skyrim.esm/00013292.nif`
(control byte, `escaped_uris: 1`) and `meshes/actors/male/greybeardstatic.nif`
(NaN transform). Without the latter, reverting the NaN guards leaves the convert
pin green.

### Mutation checks

- Passing control bytes through in `append_escaped`: 11 assertions fail.
- Reverting the non-finite guards (`is_finite` in `nif_reader`, `json_number` in
  `gltf_writer`): `glb_json_parses` fails on `greybeardstatic.nif` only.
- Bumping the mesh fingerprint to `mesh/4`: only `index_hash` fails, which is
  why it is pinned.

## View pin

`bethconv view` reads the pack, so it is a second consumer of the writer's
output. It runs on each convert run's pack with `--filter meshes/`;
`pulled_in` counts textures included because a selected mesh needs them. The
JSON assertions are repeated because the view rewrites each GLB.

### `image_refs_dangling`

Over a full SE pack, 572 of 120,313 image references point to files the game
does not ship. Here the number is different: on a filtered pack most dangling
references are textures the filter excluded. The pin watches whether escaping
and unescaping agree, not Bethesda's missing files.

Breaking `uri_unescape` in `pack_view.cpp` fails five assertions in
`spaced-texture-slots` (resolved and dangling move by 96). `every-kind` stays
green, which is why the second filter exists.

## Field-definition pin

Runs every definition over every record of a defined type in the pinned
plugins, with all archives mounted. 48 s under ASan.

Must be zero: `failed`, `leftover` (a definition consuming less than the field
holds, which nothing else detects) and `unreadable`. `unhandled` is recorded as
a count and `unhandled_hash`. `string_tables` makes sure
`unresolved_strings: 0` is not just "no tables mounted".

### Mutation checks (against LE)

| Mutation | Fails |
| --- | --- |
| `read_object_bounds` reads five of six `int16`s | `leftover` (25,555), `leftover_fields` (21) |
| `parse_static` ignores `MNAM` | `unhandled` (+824), `unhandled_fields` (+1), `unhandled_hash` |
| string lookup always returns nothing | `string_tables`, `table_entries`, `resolved_strings` (`unresolved_strings` stays 0) |
| census skips `STAT` | `parsed` (−9,720), `types_seen` (38 → 37), `type_counts_hash` |

Not caught: shortening `Static`'s `DNAM`, because the branch ends in a
`read_verbatim` catch-all.

## Merge pin

The merge has no cheap subset: it walks the whole load order twice. About 58 s
for three installs.

- **Totals** (e.g. SE: 1,178,001 forms from 1,188,910 records).
- **`type_counts_hash`**: per-type totals after the merge.
- **Pinned forms**, 9–11 per install: never overridden, overridden six times,
  deleting override, injected, a REFR parented to its CELL, a light-space form,
  and one of each defined type.

`payload_hash` (FNV-1a of the winning payload) changes when a different plugin
wins even if all totals stay the same. The Tamriel WRLD `0x0000003C` shows the
installs differ: won by `Update.esm` on LE (one override),
`ccBGSSSE025-AdvDSGS.esm` on SE (six), `SkyrimVR.esm` on VR (five).

Strings are pinned as hashes so no game text is committed.

To pick forms for a new install, leave `forms` empty and run the snapshot tool;
it prints one candidate per class.

## Load-order pins

They pin placement, not flag reading: in vanilla the `.esl` extension and the
light flag always agree, so reading the extension passes here.
`tests/unit/test_load_order.cpp` covers that. A plugin taking the wrong slot
shifts every later index and shows up in the diff.

## Adding an install

Add an object to `installs` with `id`, `env`, `description`, the `plugins` and
`archives` to probe (relative to `Data`), and an empty
`expected_read_failures`. Zero the measured fields and run the snapshot tool.
Absent sections are skipped; `"forms": {}` enables the field census.

List every archive the install ships, in folder order (the later mount wins).
`Skyrim - Interface.bsa` holds the string tables, and on VR
`Skyrim - Patch.bsa` holds newer copies of 105 of them; without it, 25
`Update.esm` GMST names stay unresolved.

Find corrupt entries first:

```sh
bethconv scan --read-all "$SKYRIM_DATA_LE"/*.bsa
```

and copy the failures into `expected_read_failures`.
