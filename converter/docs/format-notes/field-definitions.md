# Field definitions

The record layer (`esm4-plugins.md`) walks plugins and hands out payloads; this
layer decodes them for 45 record types. Sizes were measured with
`bethconv records --field-sizes <TYPE>`, and every definition is checked with
`bethconv forms` over real installs.

## Files

| File | Types | Scope |
| --- | --- | --- |
| `record/forms.{hpp,cpp}` | STAT DOOR LIGH CELL WRLD REFR | placing a city |
| `record/forms_object.*` | TXST ACTI CONT MISC MSTT FURN FLOR TREE KEYM ALCH AMMO WEAP PROJ IDLM LVLN | cell contents |
| `record/forms_world.*` | LTEX IMGS CLMT WTHR REGN LCTN LAND NAVM NAVI ACHR SPGD | world layers |
| `record/forms_game.*` | GMST GLOB CLAS FACT ENCH SPEL NPC_ | game data |
| `record/forms_actor.*` | ARMO ARMA OTFT LVLI RACE | what actors look like and wear |

The list lives in `defined_types()`. A type listed there without a census
dispatch entry is a hard error; `tests/unit/test_forms.cpp` walks the whole set.

## What the census measures

- **parsed / failed:** whether the bytes decode at all.
- **leftover:** a field that held more than its definition consumed. This
  catches structs that are too short.
- **no definition:** fields not decoded yet. Not an error; the to-do list.

## Results

| Corpus | Records | Failed | Leftover | Strings resolved / unresolved |
| --- | --- | --- | --- | --- |
| SE, five masters | 1,064,097 | 0 | 0 | 17,756 / 0 |
| SE, + 4 CC + `_ResourcePack.esl` | 1,072,300 | 0 | 0 | 17,756 / 0 |
| LE `Skyrim.esm` (header 0.94) | 783,317 | 0 | 0 | 13,085 / 0 |
| VR, six masters | 1,063,518 | 0 | 0 | 17,572 / 25 |
| Modded, 612 third-party plugins | 2,427,811 | 0 | 0 | 35,278 / 0 |

VR's 25 come from mounting the wrong copy of `update_english.strings`; see
`localized-strings.md`.

### Pinned in the corpus harness

`tests/corpus/` runs the census on every run and requires zero failures, zero
leftover and no plugin that fails to open. It records:

| Install | Plugins | Parsed | Types seen | Unhandled fields | Strings resolved / unresolved |
| --- | --- | --- | --- | --- | --- |
| LE | 2 | 783,585 | 38 of 38 | 76,178 across 60 pairs | 13,085 / 0 |
| SE | 10 | 1,072,300 | 38 of 38 | 115,099 across 65 pairs | 18,590 / 0 |
| VR | 6 | 1,063,518 | 38 of 38 | 112,857 across 65 pairs | 17,615 / 0 |

String counts differ from the table above because the harness mounts every
archive, not just the data folder and `Interface.bsa`.

`unhandled` is pinned as a count and as an FNV-1a hash over the per-type field
census, so a new definition changes both and a definition that stops consuming
a field changes the hash.

Cost: 48 s under ASan for all three installs. Limitation: a definition ending in
a `read_verbatim` catch-all can never produce leftover bytes, so shortening
`Static`'s `DNAM` goes unnoticed, while shortening `read_object_bounds` by two
bytes shows up 25,555 times across 21 (type, field) pairs.

### The modded run covers non-localized plugins

Only 10 of the 612 modded plugins have string tables. In the other 602, `FULL`,
`DESC`, `SHRT` and rank titles are inline text rather than an index. Vanilla has
no such plugin, so this run is the only real-data coverage of that path. All 38
types occur in it.

## Scripts (VMAD)

`record/vmad.{hpp,cpp}` decodes the script part of VMAD: script names and
property values, both object layouts (format 1 FormID first, format 2 FormID
last) and versions 2-5 (status bytes from 4 on). Decoded on every defined type
that carries it, including REFR, DOOR, LIGH and TREE (332 scripted TREEs in the
FUS list, none in vanilla). Every VMAD in the three installs and the FUS list
decodes with no failures or leftover bytes. Fragment data (QUST, INFO, PACK,
SCEN, PERK) is not decoded; none of those types is defined.

## Two patterns

**Fields that open a sublist.** `RDAT` (REGN entry), `RNAM` (FACT rank), `ATKD`
(NPC_ attack), `TINI` (tint layer), `CSDT` (sound set). Following fields belong
to the last opener. Push on the opener, append to `back()` on followers, and
emplace a default if a follower comes first, since malformed files do that.

**Sections switched by marker fields.** RACE's fields come in sections:
`NAM0` opens head data, `NAM1` body data, `NAM3` the behaviour graph, and
within each the empty `MNAM` and `FNAM` switch between male and female. The
same tag means different things per section (`INDX` + `MODL` is a body part
under `NAM1`, `MODL` the behaviour file under `NAM3`), so the parser carries
the section and sex as state. RACE reads what building an actor needs
(skeleton per sex, body parts, behaviour, skin, heights) and keeps every
other field whole in `other`, so its definition claims all 60 field types
without decoding tints, morphs and movement. Every RACE of the three installs
and the 985 in the FUS list parse with nothing left over.

**A field typed by another field.** GMST's `DATA` type comes from the first
letter of its editor id: `b` bool, `i` int, `f` float, `s` string, `u`
unsigned. `EDID` is not guaranteed to precede `DATA`, so `DATA` is stored raw
and interpreted after the walk; `raw` is kept either way.

All 1,659 vanilla game settings (SE's ten plugins and LE `Skyrim.esm`) have a
known prefix and 4-byte `DATA`. `u` does not occur. String settings go through
`read_lstring` like `FULL`.
