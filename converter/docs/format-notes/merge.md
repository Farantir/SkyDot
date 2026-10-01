# Merge

Collapses a load order into one set of forms. FormIDs are remapped once at
convert time; the engine never sees plugins or mod indices.

## Rule

The last plugin in the order to write a form wins. The rest of
`record/merge.hpp` is bookkeeping.

The merge needs only record headers, so it covers all records regardless of how
many types have field definitions.

## Measured

Four load orders, under ASan:

| Order | Plugins | Records | Forms | Overrides | Deleted | Injected | Unresolved | Errors |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Vanilla LE | 2 | 870,856 | 870,120 | 735 | 3 | 0 | 1 | 0 |
| Vanilla SE | 10 | 1,188,910 | 1,178,001 | 10,908 | 286 | 11 | 1 | 0 |
| Vanilla VR | 6 | 1,177,129 | 1,168,057 | 9,071 | 264 | 4 | 1 | 0 |
| Modded | 295 | 1,429,962 | 1,274,726 | 155,235 | 278 | 287 | 1 | 0 |

Record counts match `loadorder --verify` exactly. The unresolved record is the
`Skyrim.esm` GMST `0x0123C00E` described in `load-order.md`. No record lacks a
parent it should have.

## Integrity check

Pass one counts forms; pass two forwards only winning records. The two counts
must match, and do on all four orders. A wrong "who wins" decision still gives a
plausible count but breaks this equality.

## Two passes, payloads not stored

Pass one builds the index (~40 bytes per record, ~50 MB for SE). Pass two
reopens the plugins and passes each winning record's payload to a callback.
Storing payloads would cost 250 MB for `Skyrim.esm` alone; the second walk costs
10–17 s.

The callback also gets the record's own plugin's `FormContext`, because string
tables are per plugin and that information is gone after the merge. Tables are
loaded once per plugin in pass one.

## Injected records

A record in another plugin's FormID space that the owner never defines, e.g. a
DLC adding to `Skyrim.esm`'s space. Only known after the whole order is walked:
the flag starts set and is cleared when the owner shows up. SE has 11; the
modded list 287 (e.g. `Adamant.esp` adding KYWD records to `Update.esm`'s space).

## Deleted records are kept

A deleting override marks the form instead of dropping it, so "deleted" and
"never existed" stay distinguishable. Consumers filter. Vanilla deletions come
from DLC; `tests/unit/test_merge.cpp` covers the behavior.

## Parents

Each record stores the innermost enclosing group whose label is a form,
remapped through the record's own plugin.

- **Innermost:** an exterior REFR sits in both a WRLD-labelled
  `world_children` and a CELL-labelled `cell_children` group; the cell owns it.
  Interior cells cannot tell the difference, so an exterior fixture tests it.
- **Remapped:** the label is plugin-local like any FormID. Copying it gives a
  plausible but wrong parent.

Form-labelled group types: `world_children`, `cell_children`,
`cell_persistent_children`, `cell_temporary_children`, `topic_children`,
`quest_children`. The others hold a type tag, block number or packed grid
coordinates, which would silently decode as nonsense FormIDs.

## Type changes are reported

An override with a different record type keeps the original type and is added
to `problems()`. Never seen in the four orders.

## Unreadable plugins

`LoadOrder::build` already opens every plugin and reports unreadable ones, so
the merge never sees them. `MergeStats::unreadable` covers a file vanishing
between the two steps.

## Usage

```sh
bethconv merge --data <Data>                    # order from the folder
bethconv merge --data <Data> --list plugins.txt # a modded order
bethconv merge --data <Data> --types CELL REFR --sample 5
bethconv merge --data <Data> --find 0x00027D1C  # provenance of one form
```

Without `--source` it mounts every archive in the folder plus loose files, so
`strings/` resolves. Archives mount alphabetically, not in load order; the
command reports how many `strings/` paths have more than one provider:

| Install | Contested `strings/` paths |
| --- | --- |
| SE | 0 |
| LE | 3 |
| VR | 105 |

On VR the `Patch.bsa` and `Interface.bsa` copies disagree. Alphabetical order
happens to match the game's `sResourceArchiveList2`, so the merge reads the same
copy the game does. See `localized-strings.md`.
