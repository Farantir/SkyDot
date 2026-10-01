# Spike: ESM4 record coverage

**Question:** how much of Skyrim's record set does OpenMW's `esm4` cover, and
what is missing?

**Budget:** 1 week · **Took:** ~3 hours · **Result:** passed, with a different
approach

## Criterion

≥ 90% of record instances parse, plus a list of record types needing new code.

## Approach

Instead of porting OpenMW's `esm4` and counting its failures, the structural
layer was written from scratch. A plugin has two layers:

1. **Structure:** TES4 header, GRUP tree, record and field headers, zlib
   payloads. Independent of record type.
2. **Semantics:** what each field means per type. Most of the work.

`esm4` only helps with layer 2, and is Oblivion-oriented there. Layer 1 is
~600 lines, fully documented on UESP, and gives a measurement tool instead of
an estimate. It lives in `src/record/` (`headers.cpp`, `plugin.cpp`,
`histogram.cpp`) and reads only through `SpanReader`.

## Results

`bethconv records --stats`, Debug + ASan.

### Vanilla + DLC

| File | Records | HEDR | Matches |
| --- | ---: | ---: | :---: |
| Skyrim.esm | 869,687 + 50,494 groups | 920,181 | yes |
| Update.esm | — | 20,125 | yes |
| Dawnguard.esm | — | 102,268 | yes |
| HearthFires.esm | — | 19,017 | yes |
| Dragonborn.esm | — | 251,318 | yes |
| SkyrimVR.esm | — | 1,265 | yes |

1,177,715 records in 136,459 groups; 105,468 compressed; 313.5 MiB payload plus
469.3 MiB inflated; 0 structural errors; 12 s. HEDR equals records + groups in
every file (see `docs/format-notes/esm4-plugins.md`).

### 612 third-party plugins

2,726,709 records, 363,971 groups, 251,671 compressed, 1,073 MiB payload,
+1,021 MiB inflated, 29 s, 0 structural errors, 0 files failed to open.

Every record's fields tile its payload exactly (no gaps, overruns or bad
headers).

### Record types

119 types in vanilla + DLC; 120 with mods (`LENS`, from a lighting mod).

The planned priority list of 39 types covers 90.30% of record instances; the
other 81 types cover 9.70%. The largest uncovered types are `INFO` (41,180),
`DIAL` (19,750), `PACK` (7,605), `DLBR` (3,931), `QUST` (2,365) and `SCEN`
(2,139): dialogue, AI packages and quests, which were deferred anyway.

The per-type field inventory is not committed (it describes Bethesda's data);
`bethconv records --stats --top-fields 12` regenerates it.

## Consequences

1. The structural layer is ours. `esm4` is at most a reference for individual
   field layouts.
2. Field definitions are many small, independent per-type jobs.
3. By instance coverage the "ESM4 is bigger than estimated" risk does not
   trigger; by type count (81 > 30) it does, but most types share common fields
   (EDID, FULL, MODL, OBND, KWDA), so instance coverage is the better measure.
4. Compressed records are required: 9% of vanilla records.
