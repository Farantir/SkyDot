# Record snapshot (`records.fb`)

The merged world (`merge.md`) written to disk, mmap-able and read zero-copy.
Indices are built at convert time: form id → form, editor id → form id,
type → forms, parent → children, and a per-worldspace cell grid.

## Container

```
records.fb
├── [64 bytes]  header        "BETHSNAP", format version, section offsets, blob hash
├── [n bytes]   payload blob  winning records' field bytes, concatenated
└── [m bytes]   FlatBuffer    identity, provenance, indices
```

The payloads are outside the FlatBuffer because FlatBuffers use 32-bit offsets
and cannot exceed 2 GiB. Release build:

| Order | Forms | Payload | Index | Total | Write |
| --- | ---: | ---: | ---: | ---: | ---: |
| Vanilla LE | 870,120 | 305 MiB | 43.6 MiB | 348 MiB | 2.1 s |
| Vanilla SE | 1,178,001 | 527 MiB | 60.5 MiB | 588 MiB | 3.5 s |
| Vanilla VR | 1,168,057 | 526 MiB | 59.9 MiB | 586 MiB | 3.5 s |
| 295-plugin modded order | 1,184,892 | 532 MiB | 61.2 MiB | 593 MiB | 3.7 s |

Vanilla SE alone is a quarter of the limit, and FlatBuffers fails at the limit
with an assert or a wrapped offset. So payloads use 64-bit offsets into the
blob and only the index (about 55 bytes per form) must fit.

(The modded row is a different reconstruction of that load order than the one
in `merge.md`, so its numbers differ.)

The blob comes first because it is streamed during the merge's second pass; the
index can only be built afterwards. The header records both offsets.

## `Form` is a struct

A FlatBuffers struct is fixed-size and inline, so `forms` is one contiguous,
binary-searchable array: 40 bytes per form, 47 MiB for SE, versus about 130 MiB
as tables. Structs cannot hold strings or vectors, so editor ids and payloads
are referenced by offset.

## Indices

| Index | Shape | Query |
| --- | --- | --- |
| `forms` | struct array sorted by FormID | form 0x00027D1C |
| `editor_ids` + `editor_id_forms` | parallel arrays sorted by name | `WhiterunDragonsreach` |
| `types` | `{type, [forms]}` sorted by type | every CELL |
| `children` | `{parent, [forms]}` sorted by parent | contents of a cell |
| `worlds` | `{world, [{key, cell}]}` sorted by world, then key | cell at (5, −1) in Tamriel |

`children` duplicates each form's `parent` field on purpose; without it, cell
contents need a scan of 1.18M forms, and cell streaming asks constantly.

Grid key: `(int64(x) << 32) | uint32(y)` from XCLC. Both axes go negative.

### Editor ids are read generically

EDID is a plain string on every type that has one, so the writer finds it by
tag instead of parsing the record. The index therefore covers all types, not
just defined ones. LE: 89,980 editor ids over 870,120 forms (693,333 are REFRs,
which usually have none). SE: 122,904; modded: 130,598.

## The persistent cell is not a grid square

Each worldspace has a persistent cell holding its persistent references (15,197
in LE's Tamriel). Its XCLC is (0, 0), same as the real cell at (0, 0). Indexed
naively, one hides the other — in LE, `0x00000D74` (persistent) versus
`0x000099A2` (`BleakwindBasinExterior01`) — and all counts still add up.

Only the record header's persistent flag (`0x400`) tells them apart. Such cells
are left out of the grid index but are otherwise normal forms.
`SnapshotStats::grid_duplicates` counts remaining collisions; it is 0 on all
three installs. Found by `bethconv verify`.

## Reading a snapshot is reading untrusted input

The file may be half-written, copied or edited. Two mechanisms:

- the header and every blob slice go through `io::SpanReader`;
- the FlatBuffer goes through `flatbuffers::Verifier` before any accessor runs.

The verifier covers the index, not the blob, so blob slices are bounds-checked
separately. The form count is stored in both the header and the index; `open`
rejects a mismatch. The blob hash is checked by `verify --deep`, not by `open`.
`tests/fuzz/fuzz_snapshot.cpp` fuzzes all of it.

## `verify`

Cheapest first:

1. **Form array:** sorted, unique, each form findable by id, payloads inside the
   blob.
2. **Indices:** type entries have that type; sampled parents list their
   children.
3. **Cells:** N cells spread across the array, each parsed as CELL and each
   child as REFR; every exterior cell looked up again through the grid.
4. **`--against <Data>`:** re-run the merge and compare every form's
   provenance.

Vanilla SE, release: write 3.5 s, `verify --deep --against` 3.7 s:

```
1178001 forms from 10 plugins
form array   0 out of order, 0 not findable by id, 0 with an unreadable payload
type index   952048 forms across the defined types, 0 disagree
cells        20 checked, 1192 children, 0 REFR would not parse
grid         49 worldspaces, 2000 cells looked up, 0 did not come back
round-trip   0 absent from the snapshot, 0 with different provenance
verify: clean
```

## Usage

```sh
bethconv snapshot --data <Data> -o records.fb
bethconv snapshot --data <Data> --list plugins.txt -o records.fb
bethconv verify records.fb                              # cheap checks
bethconv verify records.fb --deep --against <Data>      # everything
bethconv verify records.fb --find 0x00027D1C            # one form
```

`snapshot` mounts every archive in the folder so `strings/` resolves.

## Missing

- **Per-language string files.** Resolved text lives in the payloads, so a
  snapshot is tied to the language it was built with.
- **Migration.** A `format_version` mismatch is refused; there is no upgrade.
- **16-bit `Form.winner`/`owner`.** Enough for 254 normal + 4,096 light plugins.
