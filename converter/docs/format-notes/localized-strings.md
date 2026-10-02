# Localized strings: `.STRINGS`, `.DLSTRINGS`, `.ILSTRINGS`

Where a localized plugin's text lives. Measured on the vanilla tables in
`Skyrim - Interface.bsa` (SE) and `Data/Strings/` (LE) with `bethconv strings`
and `bethconv forms`. Format source:
[UESP](https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format), checked against the
bytes.

## A string index only means something with its plugin

`#0000001` is "The Ratway Vaults" in `Skyrim.esm` and "Auriel's Bow" in
`Dawnguard.esm`. The merge discards which plugin a record came from, so strings
are resolved at or before the merge. `FormContext` carries `localized` and
`strings` per plugin for that reason.

## Layout

```
uint32  count
uint32  dataSize
count × { uint32 stringId; uint32 offset }      ← directory
dataSize bytes                                   ← data
```

`offset` is relative to the data section. In all three vanilla English tables
`8 + count*8 + dataSize` equals the file size; shorter files are rejected.

| File | Entry |
| --- | --- |
| `.STRINGS` | NUL-terminated |
| `.DLSTRINGS`, `.ILSTRINGS` | `uint32` length including the NUL, then the bytes |

Reading `length` bytes instead of `length - 1` keeps the NUL; `length == 0` must
not underflow.

## Not in the wiki

- **The directory is not sorted by offset.** Do not pair the Nth run with the
  Nth entry.
- **Duplicates are shared.** 8,601 of 30,301 entries in
  `skyrim_english.strings` reuse an earlier offset.

  | Table | Entries | Distinct strings |
  | --- | --- | --- |
  | `skyrim_english.strings` | 30,301 | 21,700 |
  | `skyrim_english.dlstrings` | 2,686 | 2,578 |
  | `skyrim_english.ilstrings` | 34,427 | 32,396 |

- **The three tables have disjoint ids.** A wrong guess of the table still
  misses, though, so the kind is never guessed.

## Encoding is UTF-8, and LE has bad bytes

Not Windows-1252:

| Table | Entries | Invalid UTF-8 |
| --- | --- | --- |
| SE `skyrim_english.{strings,dlstrings,ilstrings}` | 67,414 | 0 |
| SE `skyrim_german.strings` | 30,301 | 0 |
| SE `skyrim_french.ilstrings` | 34,427 | 0 |
| SE `skyrim_russian.strings` | 30,301 | 0 |
| LE `Skyrim_English.STRINGS` | 30,301 | 4 |
| LE `Skyrim_English.DLSTRINGS` | 2,686 | 3 |

Windows-1252 applies to Oblivion-era files and to text embedded in
non-localized plugins. Invalid sequences are replaced with U+FFFD and counted,
because one invalid byte would break the JSON and glTF output.

Validation follows Unicode 15.0 table 3-7 (well-formed UTF-8): overlongs,
surrogates and values above U+10FFFF are rejected. Each case is tested in
`tests/unit/test_strings.cpp`.

## Where the tables are

| Install | Location |
| --- | --- |
| SE | inside `Skyrim - Interface.bsa`; no `Data/Strings/` |
| LE | loose in `Data/Strings/`, e.g. `Skyrim_English.STRINGS` |
| Creation Club, `_ResourcePack` | each in its own `.bsa` |

Lookups go through `ArchiveSet`, so both layouts work, case is ignored, and a
loose translation override wins as in the game.

Naming, checked on every vanilla and CC plugin:

```
strings/<plugin stem, lowercased>_<language>.<ext>
```

`Skyrim.esm` → `strings/skyrim_english.strings`, `_ResourcePack.esl` →
`strings/_resourcepack_english.strings`. Only the last dot starts the
extension.

## Every vanilla master is localized

All eighteen masters across LE, SE and VR, including LE's `Skyrim.esm` (header
0.94). No vanilla file uses FULL-as-text, so that path, common in mods, is only
covered by `tests/unit/test_strings.cpp`.

## Resolution results

`bethconv forms --source <Data> --source <Interface.bsa>`, 38 types defined (45 today):

| Set | Resolved | Unresolved |
| --- | --- | --- |
| SE, five masters | 17,756 | 0 |
| SE, masters + 4 CC + `_ResourcePack.esl` | 17,756 | 0 |
| LE `Skyrim.esm` | 13,085 | 0 |
| VR, six masters | 17,572 | 25 |

These depend on the mount. With the whole data folder, SE's ten plugins resolve
18,590 and VR's six resolve 17,615, none unresolved.

## VR's two `update_english.strings` differ

VR ships `strings/update_english.strings` in two archives:

| Source | STRINGS | DLSTRINGS | ILSTRINGS | The 25 GMST names |
| --- | --- | --- | --- | --- |
| `Skyrim - Interface.bsa` | 700 | 201 | 122 | absent |
| `Skyrim - Patch.bsa` | 726 | 202 | 122 | present |
| loose `Data/Strings/` | 726 | 202 | 122 | present |

The 25 unresolved above are `Update.esm` GMST `DATA` fields, read from the
`Interface.bsa` copy. A whole-folder mount picks `Patch.bsa` (later
alphabetically, and last in VR's `sResourceArchiveList2`), matching the game;
the corpus harness does that and sees 0 unresolved on all installs.

So an unresolved index may mean the wrong archive won, not a missing string.
The index is kept and `LString::is_id` stays true. VR has 105 contested
`strings/` paths, SE none (see `merge.md`).

`SkyrimVR.esm`'s tables are only in `Skyrim_VR - Main.bsa`
(`skyrimvr_english.strings`, 107 entries, plus DLSTRINGS with 100). Mounting all
archives resolves its 18 localized fields.
