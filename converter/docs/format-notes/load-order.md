# Load order, plugin indices and FormIDs

Measured over 622 plugins (a 619-mod Wabbajack list for Skyrim VR plus the three
vanilla installs) and the load order that list uses. Two common modding beliefs
turn out to be wrong.

```sh
B=build/linux-release/tools/bethconv-cli/bethconv
$B loadorder --data "$SKYRIM_DATA_SE" --verify
$B loadorder --data <MO2 virtual Data> --list "<profile>/plugins.txt" --verify
```

## A FormID's high byte indexes the plugin's own master list

In plugin P with masters `M[0..n)`, high byte `i` means:

| `i` | means |
| --- | --- |
| `i < n` | plugin `M[i]` |
| `i == n` | P itself |
| `i > n` | broken |

The same four bytes mean different records in different files, so the
converter remaps once and the engine never sees a mod index.

Every record FormID remapped in four load orders:

| Load order | records | own | overrides | unresolved |
| --- | ---: | ---: | ---: | ---: |
| vanilla LE | 870,856 | 870,120 | 735 | 1 |
| vanilla SE | 1,188,910 | 1,177,990 | 10,919 | 1 |
| vanilla VR | 1,177,129 | 1,168,053 | 9,075 | 1 |
| modded, 295 plugins | 1,429,962 | 1,274,439 | 155,522 | 1 |

The unresolved one is always the same: a GMST in `Skyrim.esm`, `0x0123C00E`,
with index 1 in a plugin that has no masters. It is reported, not guessed.
Mods have more (`VREquip.esp`: eleven records at index 9 with six masters).

## 0xFE never appears on disk

No `0xFE` high byte in any of the 622 plugins, 236 of them light. Light plugins
store ordinary FormIDs against their master list; the compact form is assigned
at load time:

```
normal:  IIoooooo          I = index 0x00..0xFD, o = 24-bit object index
light:   FE Lll ooo        L/l = 12-bit light index, o = 12-bit object index
```

That leaves 254 normal slots: `0xFE` marks light plugins and `0xFF` is the
save game's. None of the 97 light plugins with records of their own uses an
object index above `0xFFF`; `LoadOrder::resolve` rejects one that does instead
of truncating it into a collision.

## The light flag is in the header, not the filename

| | count |
| --- | ---: |
| plugins | 622 |
| light-flagged | 236 |
| named `.esl` | 6 |
| `.esl` without the light flag | 0 |
| `.esl` without the ESM flag | 1 (`CC'sEnhancedOreVeinsSSE-HearthfirePatch.esl`) |

In vanilla the flag and the extension agree on all ten plugins, so the corpus
harness cannot catch code that reads the extension (verified by trying it).
`tests/unit/test_load_order.cpp` covers that case.

## Masters do not always load first

In the modded list, four ESM-flagged plugins come after non-ESM ones
(`Sentinel - Distribution Framework.esp`, `EnchantmentArtExtender.esl`,
`BetterAnimals.esl`, `DynDOLOD.esm`):

| ordering | master-after-dependent violations |
| --- | ---: |
| `loadorder.txt` as listed | 1 |
| stable ESM-flag partition | 3 |
| (ESM flag or `.esl`) partition | 3 |
| `plugins.txt`, active only, implicit masters first | 0 |

Sorting masters forward creates violations: `DynDOLOD.esm` depends on two
`.esp` files. `LoadOrder::build` keeps the listed order and reports late
masters. The only reordering is hoisting the implicit masters, which the game
always loads first.

## `plugins.txt` and `loadorder.txt` differ

Both: one plugin per line, `#` comments, CRLF, UTF-8 BOM.

| | `plugins.txt` | `loadorder.txt` |
| --- | --- | --- |
| active marker | `*` prefix | none |
| unmarked line | inactive | active |
| lists base masters | no (MO2) | yes |
| read by the game (SE) | yes | no |

A file with no `*` is a `loadorder.txt`; reading it as `plugins.txt` activates
nothing. `PluginList::marks_active` records which one it is.

MO2 leaves `Skyrim.esm`, `Update.esm`, the three DLC and `SkyrimVR.esm` out of
`plugins.txt`, which shifts every index by five or six if not corrected. They
are prepended in game order, filtered by what exists on disk.

## Without a list file

`LoadOrder::from_directory` does what the game does when generating
`plugins.txt`: implicit masters, then ESM-flagged, then the rest, each oldest
first with the name as tiebreak. Timestamps rarely survive copying, so on a
Steam install this is effectively alphabetical per block. Diagnostic use only;
real conversions should get the user's list.

## The resolved modded order

295 plugins, no missing masters, no ordering violations, no duplicates.

| | |
| --- | ---: |
| normal indices | 142 of 254 |
| light indices | 153 of 4,096 |
| largest master list | `Synthesis.esp`, 40 |
| overrides | 155,522 (10.9%) |

A large modlist already uses over half the normal slots, so the 254 limit is
real and the light flag is what keeps lists under it.
