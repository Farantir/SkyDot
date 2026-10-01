# ESM4 plugins — observations

From running the record layer over vanilla SE/VR and a 612-plugin modded load
order. Sources: the [UESP file format page](https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format)
and observed bytes.

## HEDR `numRecords` counts records and groups

Documented as the number of records; it is `records + groups`. Skyrim.esm:
869,687 records + 50,494 groups = 920,181, the HEDR value. Holds for Update,
Dawnguard, HearthFires, Dragonborn and SkyrimVR too.

That makes it a free integrity check on the traversal: skipping a group or
misreading a size breaks the sum. `bethconv records` prints both numbers.

## The ESL flag is independent of the `.esl` extension

In the 612-plugin load order, 234 plugins have the light flag (`0x200` on TES4)
and only 3 use `.esl`. The rest are ESL-flagged `.esp` files.

So FormID width, load-order slot and compact indexing must come from the flag,
never the filename. Building a load order requires reading every plugin's TES4
header first.

## Group size includes the header; record size does not

`GRUP.groupSize` counts its 24-byte header; a record's `dataSize` does not.
Mixing them up puts the cursor 24 bytes into a record, where the next four
bytes usually look like a valid tag. Tested in
`tests/unit/test_record_plugin.cpp`.

## Group type 10 differs between Oblivion and Skyrim

Oblivion: Cell Visible Distant Children. Skyrim: Quest Children. `GroupType`
uses the Skyrim name. All eleven group types occur in vanilla + DLC;
`cell-children` (56,031) and `cell-temporary-children` (55,947) dominate.

## `XXXX` for fields over 64 KiB

Field sizes are `uint16`. For larger fields, an `XXXX` field holds a `uint32`
with the real size of the next field, whose own size is written as 0. Seen on
large `NAVM`/`NVNM` and `LAND` payloads. `read_field_header` handles it, so
callers never see `XXXX`. Two chained `XXXX`, or an `XXXX` whose size is not 4,
is rejected as corrupt.

## Compressed records

Flag `0x00040000`: a `uint32` uncompressed size, then a zlib stream (not raw
deflate, so `inflateInit`).

Common: 105,468 of 1,177,715 vanilla records and 251,671 of 2,726,709 modded
ones. `NAVM`, `LAND`, `CELL` and `NPC_` are nearly always compressed; `REFR`
never.

If the inflated size differs from the declared size, the record is skipped.

## Deleted records occur in mods, not in vanilla

Vanilla + DLC has none (flag `0x20`); the modded set has 392, mostly the
deleted-instead-of-disabled pattern xEdit cleaning fixes. The merge keeps them
as tombstones because a later plugin may override them again.

## Same-named BSAs are routine

Mod managers keep one folder per mod, and archive names repeat (two
`Skyrim - Patch.bsa`, two `SunHelmSurvival.bsa` in one list). `ArchiveSet`
therefore names sources `<mod-folder>/<archive>`.

That load order: 71 BSAs, 112,992 entries, 111,475 unique paths, 1,284
shadowed. All version 105.
