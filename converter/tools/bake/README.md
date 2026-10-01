# `tools/bake` — pack to Godot `.pck`

Runs headless Godot over a pack and writes a `.pck`. Importer defaults are
pinned in `project.godot`.

```
./tools/bake/bethconv-bake.sh --pack path/to/pack --out build/bethconv.pck
```

`--bethconv` and `--godot` (or `$BETHCONV`, `$GODOT`) select the binaries;
`--filter`, `--limit` and `--from` (a list of virtual paths, e.g. from
`bethconv cell <pack> <cell> --models`) are passed to `bethconv view` for a
partial bake;
`--keep --work DIR` keeps the intermediate project.

## Steps

1. **`bethconv view`** into a scratch project. Required: glTF resolves image
   URIs relative to the document, so importing a content-addressed pack directly
   gives meshes without materials.
2. **`godot --headless --import`** with the pinned defaults.
3. **`bake.gd`** saves each imported mesh as `res://meshes/<virtual path>.scn`
   and writes the `.pck` with `PCKPacker`.
4. **`verify.gd`** mounts the `.pck` in an empty project and counts what loads.

## Decisions

**Only meshes are imported.** Godot loads `.dds`, `.ktx` and `.ktx2` directly,
so they go into the `.pck` unchanged. A `.glb` imports as `.scn`.

**Scenes are saved under virtual paths.** `--import` writes
`.godot/imported/<name>.glb-<md5>.scn`, hashed over the source path. Re-saving
lets the engine compute a scene path from a record's MODL
(`clutter/apple01.nif` → `res://meshes/clutter/apple01.scn`).

**`PCKPacker`, not `--export-pack`.** Exporting needs export templates matching
the editor version (the installed ones are 4.3 against a 4.7 editor).
`PCKPacker` ships with the editor and controls exactly what goes in.

## Test pack result

With a pack from `bethconv-testpack` (no game data needed):

| | |
| --- | ---: |
| GLBs imported to `.scn` | 4 / 4 |
| files in the `.pck` | 8 |
| scenes loading in a fresh project | 4 / 4 |
| meshes / surfaces | 4 / 4 |
| surfaces with albedo | 4 / 4 |
| surfaces with a normal map | 4 / 4 |

This includes a mesh whose path contains a space.

## Reproducibility

Two bakes of the same pack differ in 143 bytes (about 20 per scene) because
Godot assigns resource ids without a seed. Check a `.pck` by loading it, not by
hash. The pack itself is reproducible: `ctest -R testpack` builds it twice and
compares.
