# bethconv

An ahead-of-time converter from a Bethesda game install to open formats: glTF
meshes, DDS textures, and a queryable record snapshot.

It is a library and a CLI, and it does not depend on any engine. It is built
alongside a Godot engine, but the output works just as well from Bevy, Unity,
O3DE or a Blender script.

## What this is not

- **Not a way to play Skyrim.** It converts data. Playing needs an engine.
- **No SKSE plugins.** They are native DLLs that hook the original executable;
  there is nothing to hook here. That rules out SkyUI and many other core mods.
- **No Havok behavior graphs.** Nemesis, Pandora, DAR/OAR and MCO will not
  work. Raw clips can be extracted; state machines have to be written
  engine-side.
- **No Bethesda assets are redistributed.** You point the tool at your own
  install and it converts on your machine. There are no prebuilt packs.

## Status

Converts a whole vanilla SE install into a pack in under a minute. Working, and
tested against real installs:

- **Archives:** BSA v104/v105 and loose files mounted as one virtual filesystem,
  with load-order precedence and conflict reporting.
- **Records:** the ESM4 container (GRUP tree, record and field headers,
  compression). 1.2M vanilla and 2.7M third-party records parse. 38 record
  types have field definitions.
- **Load order:** `plugins.txt`/`loadorder.txt`, ESL compact space, and
  plugin-local FormIDs remapped into one global space. See
  `docs/format-notes/load-order.md`.
- **Meshes:** NIF → GLB for LE and SE geometry, skins and collision. All 39,263
  vanilla NIFs convert and open in Godot and Blender.
- **Textures:** DDS passthrough, with short mip chains completed for Godot. All
  50,413 vanilla textures parse. See `docs/format-notes/dds-textures.md`.
- **Scripts:** compiled Papyrus decoded completely (objects, states,
  functions, bytecode, line numbers) and stored as FlatBuffers; VMAD script
  data decoded on every record that carries it. All vanilla scripts decode;
  `bethconv script --dump` disassembles.
- **Merge:** a load order collapsed into one set of forms with every override
  resolved. See `docs/format-notes/merge.md`.
- **Record snapshot:** the merged world written to an mmap-able `records.fb` and
  read back zero-copy, with form, editor-id, type, child and cell-grid indices.
  `bethconv verify --against` re-runs the merge and compares. See
  `docs/format-notes/snapshot.md`.
- **Pack:** every NIF, DDS and PEX content-addressed into one blob with an
  index (`--store loose` for a file per asset), plus `manifest.json`,
  `vpath.idx` and a `report.json` listing every failure. See
  `formats/pack-format.md`. A full vanilla SE pack is 7 files and 20 GB,
  written in under a minute.
- **Slow targets refused:** commands that write many files (`convert --store
  loose`, `view`, `mesh`, `texture`, `extract`) refuse a FUSE filesystem
  (NTFS through ntfs-3g) or a spinning disk unless given `--allow-slow-target`;
  a blob there only warns. A loose pack on an SMR disk behind ntfs-3g once
  hung the whole mount.
- **World:** `world.fb` in the pack: cells, their references with resolved
  base objects, model paths and lights, load doors, locks, linked refs,
  activate parents and scripts, ready for the engine. `bethconv cell`
  inspects it.
- **View:** `bethconv view` rebuilds the virtual directory tree from a pack,
  adding glTF images back, so Godot or Blender can open the meshes. Choose a
  subset with `--filter`, `--from` or `--limit` (`--all` for everything).

`docs/spikes/` holds the measurements the design decisions rest on.

## Building

Needs CMake ≥ 3.28, Ninja, a C++23 compiler (GCC 14+, Clang 18+, MSVC 2022) and
[vcpkg](https://github.com/microsoft/vcpkg).

```sh
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset linux-debug-asan
cmake --build --preset linux-debug-asan
ctest --preset linux-debug-asan
```

Presets: `linux-debug-asan` (default; sanitizers on because the input is
untrusted), `linux-release`, `linux-fuzz`, `windows-release`.

## Licence

GPL-3.0-or-later, because nifly is GPL-3. See `THIRD_PARTY.md`.
