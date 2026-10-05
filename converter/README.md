# bethconv

An ahead-of-time converter from a Bethesda game install to open formats: glTF
meshes, DDS textures, and a queryable world (cells, references, base objects).

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
  compression). 1.2M vanilla and 2.7M third-party records parse. 45 record
  types have field definitions.
- **Load order:** `plugins.txt`/`loadorder.txt`, ESL compact space, and
  plugin-local FormIDs remapped into one global space. See
  `docs/format-notes/load-order.md`.
- **Meshes:** NIF → GLB for LE and SE geometry, skins and collision. All 39,263
  vanilla NIFs convert and open in Godot and Blender.
- **Textures:** DDS passthrough, with short mip chains completed for Godot. All
  50,413 vanilla textures parse. `--max-texture-size N` makes larger textures
  smaller by dropping their top mip levels, without re-encoding;
  `--encode-uncompressed bc7|compact` block-compresses the textures the game
  stores uncompressed (bc7enc_rdo); compressed ones are never re-encoded. See
  `docs/format-notes/dds-textures.md`.
- **Scripts:** compiled Papyrus decoded completely (objects, states,
  functions, bytecode, line numbers) and stored as FlatBuffers; VMAD script
  data decoded on every record that carries it. All vanilla scripts decode;
  `bethconv script --dump` disassembles.
- **Animations:** Havok packfiles (`.hkx`, LE and SE) read without the Havok
  SDK: skeletons, spline-compressed and interleaved animations, bindings and
  annotations, kept as splines in `.animfb` assets. Every vanilla `.hkx`
  decodes; `bethconv animation` sweeps and samples. See
  `docs/spikes/hkx.md`.
- **Merge:** a load order collapsed into one set of forms with every override
  resolved. See `docs/format-notes/merge.md`.
- **Pack:** every NIF, DDS, PEX and HKX content-addressed into one blob with an
  index (`--store loose` for a file per asset), plus `manifest.json`,
  `vpath.idx` and a `report.json` listing every failure. See
  `formats/pack-format.md`. A full vanilla SE pack is 6 files and 20 GB,
  written in about 20 s on 12 cores, 47 s with `--jobs 1`. Assets are read,
  hashed and converted on `--jobs N` threads (default every core); the pack is
  the same bytes for any `N`.
- **Slow targets refused:** commands that write many files (`convert --store
  loose`, `view`, `mesh`, `texture`, `extract`) refuse a FUSE filesystem
  (NTFS through ntfs-3g) or a spinning disk unless given `--allow-slow-target`;
  a blob there only warns. A loose pack on an SMR disk behind ntfs-3g once
  hung the whole mount.
- **World:** `world.fb` in the pack: cells, their references with resolved
  base objects, model paths and lights, load doors, locks, linked refs,
  activate parents and scripts, ready for the engine. `bethconv cell`
  inspects it.
- **Installs and mods:** `bethconv detect` finds Steam installs (every
  library), their build and the game's `plugins.txt` (under Proton too);
  `convert --mo2 <instance>` converts a Mod Organizer 2 profile (also
  Wabbajack lists), mounting the mods in the profile's priority over Data,
  as MO2's virtual filesystem would. Creation Club plugins from `Skyrim.ccc`
  load after the masters. See `include/bethconv/install/`.
- **For front ends:** `detect`, `mo2`, `target`, `info`, `cell` and
  `convert` print JSON with `--json`; `convert` streams progress events. See
  `docs/cli-json.md`. The pack tool in `../engine/game/packtool/` uses them.
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
