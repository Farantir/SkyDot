# SkyDot architecture

This document explains how SkyDot is put together and how its parts work
inside, in much more detail than the READMEs. It describes the tree as of
2026-10-04. The format specification is `formats/pack-format.md`.
The subsystem notes in `converter/docs/` and `engine/docs/` hold the
measurements; this file links them together and does not repeat their
numbers.

Contents:

1. [What SkyDot is](#1-what-skydot-is)
2. [Repository map](#2-repository-map)
3. [Rules that shape the code](#3-rules-that-shape-the-code)
4. [Build systems and toolchains](#4-build-systems-and-toolchains)
5. [The converter (bethconv)](#5-the-converter-bethconv)
6. [The pack: the contract between the halves](#6-the-pack-the-contract-between-the-halves)
7. [The engine extension (skydot)](#7-the-engine-extension-skydot)
8. [The Godot project (GDScript)](#8-the-godot-project-gdscript)
9. [Walkthroughs](#9-walkthroughs)
10. [Threading model](#10-threading-model)
11. [Versioning](#11-versioning)
12. [Testing](#12-testing)
13. [Where to find things](#13-where-to-find-things)
14. [Glossary](#14-glossary)

---

## 1. What SkyDot is

SkyDot runs Skyrim's game data in Godot 4. It has two halves that never link
against each other:

```
  the user's game install                 the user's machine only
  (Data/, BSA archives, ESM/ESP/ESL,      nothing Bethesda-made is ever
   MO2 instance, plugins.txt)             in git or distributed
            |
            |  bethconv  (converter/, C++23, a CLI and a static library)
            v
  a pack directory                         formats/pack-format.md (v6)
  (manifest.json, records.fb, world.fb,    FlatBuffers schemas in
   assets.idx + assets-0001.blob,          formats/schema/
   vpath.idx, report.json)
            |
            |  skydot  (engine/extension/, C++20 GDExtension)
            v
  Godot scene tree                         engine/game/ (GDScript):
  (cells, terrain, lights, actors,         pack tool, cell viewer,
   physics, navigation, sky, scripts)      measurement tools
```

- **bethconv** reads Bethesda formats (BSA archives, ESM records, NIF meshes,
  DDS textures, PEX scripts, HKX animations, LOD files). It merges the load
  order into one set of forms and writes a *pack*: glTF meshes, DDS textures
  passed through, decoded scripts and animations as FlatBuffers, and two
  FlatBuffer databases (`records.fb` with every merged form, `world.fb` with
  what the engine needs to build places).
- **skydot** reads only packs. It is a GDExtension, a shared library loaded
  by Godot, that registers classes such as `SkydotPack`, `SkydotWorld` and
  `SkydotPapyrus`. GDScript in `engine/game/` uses those classes to build a
  viewer and a pack-converting GUI.

The split exists for three reasons: legal (the engine never touches game
files), robustness (hostile mod files crash a separate process, not the
game), and speed (all the slow parsing happens once, ahead of time).

---

## 2. Repository map

Line counts are approximate (tracked and untracked source, 2026-10-04).

```
SkyDot/
├── README.md, CONTRIBUTING.md, LICENSE (GPL-3.0-or-later)
├── ARCHITECTURE.md                   this file
├── formats/                          ~2,000 lines: THE CONTRACT
│   ├── pack-format.md                the pack, versioned (v6)
│   ├── schema/*.fbs                  records, world, script, lod, animation
│   └── include/skydot_formats/       C++ headers both halves share (vocabulary only)
├── converter/                        bethconv
│   ├── include/bethconv/<layer>/     public headers (~7,900 lines)
│   ├── src/<layer>/                  implementation (~22,400 lines)
│   ├── tools/bethconv-cli/           the `bethconv` command (~4,600 lines)
│   ├── tools/testpack/               synthetic test pack generator (~2,000)
│   ├── tests/                        unit, corpus, fuzz, testpack (~22,200)
│   ├── docs/                         ADRs, format notes, spikes (~4,100)
│   ├── extern/                       submodules: nifly, bc7enc_rdo
│   └── cmake/, CMakePresets.json, vcpkg.json
├── engine/                           skydot
│   ├── extension/src/assets/         pack mounting, asset cache (~1,600)
│   ├── extension/src/data/           world.fb as read: WorldData, helpers, ActorPlan (~900)
│   ├── extension/src/build/          CellBuilder, model decoration, reference helpers (~2,100)
│   ├── extension/src/world/          SkydotWorld and its queries (~1,900)
│   ├── extension/src/render/         materials, terrain, water, grass, LOD, weather, effects (~6,500)
│   ├── extension/src/physics/        collision bodies, the player (~1,250)
│   ├── extension/src/actors/         actors, their animation, locomotion, places (~1,600)
│   ├── extension/src/nav/            navigation regions (~270)
│   ├── extension/src/ai/             AI packages and SkydotAi (~2,300)
│   ├── extension/src/vm/             Papyrus VM and its binding (~4,100)
│   ├── extension/src/register_types.*  GDExtension entry point
│   ├── game/                         Godot project: pack tool, viewer, tools (~5,900)
│   ├── game/shaders/                 the shaders, as text files the render/ code assembles (~1,200)
│   ├── tests/smoke/                  headless Godot tests (~2,400)
│   ├── docs/                         subsystem notes (~1,100)
│   └── extern/                       submodules: godot-cpp, flatbuffers
├── tools/ci/                         repository guards, pre-commit hook
├── tools/packtool.sh                 launcher for the pack tool
└── .github/workflows/ci.yml          one workflow, seven jobs
```

Planning documents (`PLAN.md`, `RESEARCH.md`, `HANDOVER.md`,
`TOOLS-REQUIREMENTS.md`, `COMPARISON-SHOTS.md`) are kept *outside* the
repository, one directory up, because they contain machine-specific paths.

---

## 3. Rules that shape the code

The `CONTRIBUTING.md` files state these rules. They explain much of what the
code looks like:

| Rule | Effect on the code |
| --- | --- |
| **Clean room**: never decompile the game. Sources are UESP, xEdit definitions and observed bytes. | Every non-obvious constant cites its source in a comment, often with how many vanilla records were measured ("12 bytes on all 9,720 STATs"). |
| **No Bethesda bytes in git** | `tools/ci/check-no-game-data.sh` rejects game file extensions and large binaries. Tests use synthetic builders (`converter/tests/support/*_builder.hpp`) and a generated test pack. |
| **All untrusted input through `SpanReader`** | Converter parsers never do pointer arithmetic on file buffers. `tools/ci/check-raw-access.sh` greps the parser directories for `reinterpret_cast` (also as a `static_cast` pair through `void*`), `memcpy` and `.data() +`. |
| **Malformed input is never fatal** | Parsers return `ParseResult<T>` (`std::expected<T, ParseError>`). A bad file becomes a line in `report.json`; it never aborts the run. Exceptions are only for programmer errors. |
| **Deterministic output** | No timestamps; sorted indices; same input gives a byte-identical pack (`converter/tests/testpack/determinism.cmake` checks it). |
| **Engine reads packs only** | No ESM/NIF/BSA/DDS parser in `engine/`. If the engine needs data, the pack format grows, versioned in `formats/`, and both halves change in one commit. |
| **GPL-3, no AGPL** | nifly is GPL-3; the Papyrus VM was written without reading skymp's AGPL VM. |
| **Warnings are errors** | `-Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast ...` on GCC/clang, `/W4 /WX` on MSVC, in both halves. |

---

## 4. Build systems and toolchains

### Converter

- CMake ≥ 3.28, Ninja, **C++23** (ADR `converter/docs/adr/0001-cxx23.md`:
  only for `std::expected`), vcpkg in manifest mode (`converter/vcpkg.json`,
  pinned by `builtin-baseline`).
- Dependencies: rsm-bsa (archives), nifly (NIF, git submodule), fastgltf (glTF
  writing), FlatBuffers (snapshot and world database, `flatc` from vcpkg),
  BLAKE3 (content hashes), zlib and LZ4 (decompression), nlohmann-json
  (manifest, report, CLI JSON), CLI11 (argument parsing), bc7enc_rdo (BC7/BC1
  encoding, submodule), Catch2 (tests).
- Presets: `linux-debug-asan` (the default: ASan+UBSan, because the input is
  hostile), `linux-release`, `linux-fuzz` (clang, libFuzzer),
  `windows-release`.
- `src/CMakeLists.txt` builds one static library, `bethconv_core`, and runs
  `flatc` on every schema in `formats/schema/` at build time, so generated
  headers cannot drift from the schemas. FlatBuffers types are kept out of
  most public headers (pImpl in `pack/snapshot.hpp`); `pack/world.hpp` includes
  the generated `world_generated.h` because its structs hold the schema's flag
  enums (`wfb::RefFlags`, `wfb::CellFlags`, ...), so that header and
  FlatBuffers are public to `bethconv_core`'s users. `formats/include/` is on
  the include path of both halves.
- `tools/` builds the `bethconv` CLI and `bethconv-testpack` on top of the
  library.

### Engine

- CMake ≥ 3.28, Ninja, **C++20**, Python 3 (godot-cpp's binding generator),
  Godot 4.7 on `PATH`.
- godot-cpp and FlatBuffers are git submodules (no vcpkg). `flatc` is built
  from the submodule and generates readers for `world`, `script`, `lod` and
  `animation` from the same `formats/schema/*.fbs` files.
- The library builds straight into `engine/game/bin/`
  (`libskydot.linux.template_debug.x86_64.so`), where
  `game/skydot.gdextension` finds it. Debug presets enable godot-cpp's hot
  reload, so rebuilding with the editor open reloads the extension.
- Presets: `linux-debug`, `linux-release`, `windows-debug`,
  `windows-release`. Release presets build `template_release`, which only an
  exported game loads.

### CI (`.github/workflows/ci.yml`)

1. `guards`: the two repository checks.
2. `converter` × 3: GCC 14 + ASan/UBSan, clang 19 release, MSVC release.
   The GCC job uploads the generated test pack as an artifact.
3. `engine` × 3 (after converter): GCC, clang, MSVC debug builds; the smoke
   tests run headless against the downloaded test pack.

---

## 5. The converter (bethconv)

### 5.1 Layers

The library is layered. Each layer only uses the layers below it:

```
                      tools/bethconv-cli   tools/testpack
                               \              /
                                v            v
   pack/      convert, inputs, pack_writer, asset_store, content_hash,
              vpath_index, snapshot_writer/reader (records.fb), world (world.fb),
              script_asset, lod_asset, animation_asset, pack_view
     |           |             |              |             |
     v           v             v              v             v
   mesh/      texture/      script/       animation/     record/
   nif_reader dds           pex           hkx            plugin, headers, field_reader,
   nif_contr. mip_tail      pex_script    animation_data forms_*, vmad, conditions,
   gltf_writer mip_drop                                  strings, load_order, merge,
   mesh_ir    bc_encode                                  form_census, histogram
     |           |             |              |             |
     +-----------+------+------+--------------+-------------+
                        v
   install/   game_install (Steam, registry), mo2, vdf, mount_plan
   archive/   archive_set (virtual filesystem), vpath
                        |
                        v
   io/        span_reader, parse_error, mapped_file, deflate, byte_writer,
              span_stream, output_target, json_text
```

Public headers live in `converter/include/bethconv/<layer>/` and the
implementation in `converter/src/<layer>/`. The namespaces match the layers:
`bethconv::io`, `bethconv::archive`, `bethconv::install`, `bethconv::record`,
`bethconv::mesh`, `bethconv::texture`, `bethconv::script`,
`bethconv::animation`, `bethconv::pack`.

### 5.2 `io/`: reading hostile bytes safely

- **`SpanReader`** (`io/span_reader.hpp`) is the one primitive every parser
  uses. It is a non-owning cursor over `std::span<const std::byte>`, three
  words wide and cheap to copy. Every read checks the bounds and returns a
  `ParseResult<T>`:
  - `get<T>()` / `peek<T>()` for trivially copyable values (byte-swapped on
    big-endian hosts; aggregates are refused there at compile time);
  - `tag()` for FourCCs, never swapped;
  - `bytes(n)`, `chars(n)`, `fixed_string(n)`, `zstring()`, `bzstring()`,
    `wstring()` for the string shapes Bethesda uses;
  - `array<T>(count)` with an overflow-safe size check;
  - `subreader(n)` / `subreader_at(off, n)` give a child reader that
    *cannot* read outside its range, so nested structures need no size
    arithmetic. Error offsets stay absolute within the original file
    (`base_`), so `report.json` can say "file.nif+0x1a4: truncated".
- **`ParseError`** (`io/parse_error.hpp`) has an origin, an absolute offset,
  a coarse `ErrorKind` (truncated, out_of_range, unterminated, bad_magic,
  bad_value, unsupported, too_large, corrupt) and free-text detail.
  `ParseResult<T>` is `std::expected<T, ParseError>`.
- **`MappedFile`** maps a whole file read-only (POSIX `mmap` or Win32
  `MapViewOfFile`, shared for write/delete on Windows so a pack can be read
  while it is being written). `Skyrim.esm` is 250 MB and is never copied
  into the heap.
- **`deflate`**: `inflate_exact` (zlib: compressed records, v104 BSAs) and
  `lz4_decompress_exact` (LZ4 frames: v105/SE BSAs). Both refuse output
  larger than 256 MiB (decompression bombs) and output whose size differs
  from the declared one. The LZ4 loop replaces rsm-bsa's own, which hangs
  forever on truncated frames (found by `fuzz_bsa`).
- **`ByteWriter`**: an append-only little-endian buffer for writing glTF
  binary chunks; it keeps byte-level casts out of the parser directories.
- **`SpanStream`**: a `std::istream` over a span, because nifly reads from
  streams. Also `write_file`.
- **`OutputTarget`**: finds out what storage a path lives on (Linux
  `/proc/self/mountinfo` and `/sys/dev/block`): FUSE, rotational, zoned. The
  CLI refuses to write many small files to slow targets, after a loose
  pack on an SMR disk behind ntfs-3g once hung the machine.

### 5.3 `install/`: finding the game and the mods

- **`game_install`**: finds Steam roots, parses `libraryfolders.vdf`
  (`install/vdf.*`, a small Valve KeyValues parser) and
  `appmanifest_<appid>.acf` (72850 LE, 489830 SE/AE, 611670 VR). It reads the
  Windows registry where it exists and finds the game's own `plugins.txt`
  (under Proton, inside the app's prefix). `creation_club_plugins` reads
  `Skyrim.ccc`.
- **`mo2`**: reads a Mod Organizer 2 instance as plain files:
  `ModOrganizer.ini` (QSettings encoding: `@ByteArray(...)`, escapes,
  `%BASE_DIR%`), profiles, `modlist.txt` (first line = highest priority;
  `+` enabled, `-` disabled, `*` unmanaged, `_separator` dividers),
  `plugins.txt`/`loadorder.txt`. A Windows-written instance also opens on
  Linux, which handles Wabbajack's "Stock Game" folder.
- **`mount_plan`**: decides *what* to mount and *in which order*, following
  the game's rules. Archives sit below all loose files. Data's archives go
  first, then mod archives in the load order of the plugin that owns them
  (`<plugin>.bsa`, `<plugin> - *.bsa`). Archives that no loaded plugin names
  are reported, not mounted. Loose folders go in this order: Data, then
  mods from lowest priority up, then MO2's `overwrite/`. `mount()` assigns
  increasing priorities in that order.

### 5.4 `archive/`: one virtual filesystem

`ArchiveSet` (`archive/archive_set.*`) mounts BSA/BA2 archives (through
rsm-bsa, wrapped so its exceptions become `ParseError`s) and loose
directories. At mount time it builds one index:
`unordered_map<vpath, vector<Provider>>`, kept sorted best-first:

1. a higher priority wins;
2. at equal priority, a loose file beats an archived one;
3. otherwise the later mount wins.

`resolve(path)` gives the winner and the shadowed providers (conflict
reporting comes for free). `read(path)` returns the bytes, decompressed
(zlib for v104, LZ4 for v105, chunk-wise for BA2) or copied from a mapped
loose file. On Linux, loose files are reopened under their on-disk
spelling, because the virtual path is lowercased.

`normalize_vpath` is the canonical form of a game path: lowercased in ASCII
only (locale-independent, matching Bethesda's hashing), forward slashes, no
leading or duplicate separators, no `.` segments. It lives in
`formats/include/skydot_formats/vpath.hpp`, which the engine includes too, so
the converter's index and the engine's lookups cannot spell a path
differently (`archive/vpath.hpp` re-exports it). `is_safe_relative` guards
every write of a vpath under an output directory.

### 5.5 `record/`: plugins, records, the load order and the merge

**The ESM4 container.** A plugin is a TES4 header record followed by a tree
of GRUPs (groups) containing records; each record payload is a list of
fields (subrecords). `record/headers.*` reads the three fixed headers: the
24-byte record header, the 24-byte group header and the 6-byte field header,
including the `XXXX` escape for fields over 64 KiB. `record/types.hpp`
defines `FormId` (with ESL helpers), `RecordFlag` and `GroupType`.

**The walk.** `Plugin::open` maps the file and parses TES4 (version, masters,
ONAM overrides, flags: ESM, ESL, localized). `Plugin::scan(RecordSink&)`
walks the GRUP tree recursively, depth-capped at 32. It inflates compressed
records, gives every record a `RecordContext` (its header plus the stack of
enclosing groups) and calls `sink.on_record(ctx, reader)`. A malformed
record abandons its enclosing group and the walk carries on with the parent;
a sink may ask to stop.

**Field definitions.** `record/field_reader.hpp` has readers for the
recurring field shapes (zstring, localized `LString`, OBND, FormID arrays,
MODL/MODT/MODS, destruction stages, keywords, containers, magic effects).
`walk_fields(data, type, ctx, dispatch)` iterates the fields and lets a
per-type lambda claim the known ones. Unknown fields go to a `FieldTally`
(the census), and fields read only partly are reported as "leftover" (a
bug in a definition). The per-type structs and `parse_*` functions are in
`forms.hpp` (STAT, DOOR, LIGH, CELL, WRLD, REFR), `forms_object.hpp`,
`forms_world.hpp`, `forms_game.hpp` (quests, packages, weathers, ...) and
`forms_actor.hpp` (NPC_, RACE, ARMO, ARMA, ...). FormIDs stay
plugin-local in these structs; remapping happens once, in the merge.
`vmad.*` decodes attached scripts and their properties (and quest
fragments), and `conditions.*` decodes CTDA conditions with a table of
condition functions taken from xEdit.

**Localized strings.** Localized plugins store 4-byte indices instead of
text. `record/strings.*` reads `.STRINGS`/`.DLSTRINGS`/`.ILSTRINGS` from
the mounted archives. Strings are resolved per plugin during the merge,
because an index only means something in its own file.

**Load order** (`record/load_order.*`). The order comes from `plugins.txt`
or `loadorder.txt` (BOM, CRLF and `*` active markers handled), or from file
times. Implicit masters (Skyrim.esm, Update.esm, DLC) and Creation Club
plugins are hoisted to the front. The order assigns normal indices 0x00–0xFD
and light (ESL) indices 0x000–0xFFF in the 0xFE space. Whether a plugin is
light comes from its header flag, never its filename.
`LoadOrder::resolve(plugin, local)` maps a raw FormID through that plugin's
own master list into the global space. Problems (missing masters, a master
loading after its dependant, too many plugins) are reported and never stop
the build.

**Merge** (`record/merge.*`). `MergedWorld::build(order)` is pass one. It
opens each plugin in load order, resolves every record's FormID and parent
(the innermost group labelled with a form: the CELL for a REFR, not the
WRLD), and keeps one `MergedRecord` per global FormID: type, parent,
winner (the last plugin to write it), owner, override count, flags,
deleted, injected. There are no payloads (about 40 bytes per record).
`for_each_record(sink)` is pass two. It re-walks every plugin and forwards
only the winning records' payloads, inflated, together with that plugin's
`FormContext` (localized flag, string tables). This keeps memory near 50 MB
for SE instead of holding the payloads.

### 5.6 `mesh/`: NIF → glTF

```
  NIF bytes --SpanStream--> nifly::NifFile --nif_reader--> mesh::Model (IR)
                                              nif_controllers     |
                                                                  v
                                            gltf_writer (fastgltf) --> GLB
```

- `mesh_ir.hpp` is a plain-struct intermediate representation: nodes with
  transforms, shapes with vertex streams, skins, materials (shader type,
  nine texture slots, Bethesda flags), collision (Havok shapes, layers,
  quality types), animation clips, particle systems, extras. It exists so
  that the questions are about the model, not about nifly's API.
- `nif_reader.cpp` walks the NIF scene graph (depth-capped at 256), turns
  matrices into quaternions robustly (Shepperd's method; NIF matrices drift),
  replaces NaN transforms, fixes nifly's swapped tangents and bitangents,
  maps NiAlphaProperty to glTF alpha modes, reads skins, and reads Havok
  collision (`bhk*`). `nif_controllers.cpp` turns controllers and
  sequences into clips (transforms, visibility, shader variables, UV
  scrolling, emission) and particle systems into a data block.
- `gltf_writer.cpp` writes the GLB. It adds one root node
  (`bethconv_z_up_to_y_up`: −90° about X, ×0.0142875 m per game unit), so
  buffer values stay exactly as in the NIF. What PBR can express goes into
  standard glTF; everything else goes into `extras.bethconv` (texture slots
  by role, shader flags, effect falloff, billboards, draw order, add-on node
  indices, collision, clips, particles). Pack meshes carry *no glTF images*:
  texture paths are in the extras, and the engine resolves them through
  `vpath.idx`. `pack_view` adds images back for Blender and the Godot editor.

### 5.7 `texture/`: DDS kept as DDS

Godot loads DDS block formats directly, so textures pass through untouched
except for three optional edits on the header and the mip chain, without
decoding:

- `mip_tail`: SE textures stop their mip chains before 1×1 and Godot rejects
  them. Missing levels are appended by repeating the last level's block, and
  `dwMipMapCount` is corrected. Cube maps are fixed face by face.
- `mip_drop` (`--max-texture-size`): drops the largest levels. The smaller
  ones are already in the file, so this is lossless below the limit.
- `bc_encode` (`--encode-uncompressed bc7|compact`): block-compresses only
  textures stored uncompressed (mostly terrain LOD), level by level, using
  bc7enc_rdo on several threads. Textures that are already compressed are
  never re-encoded.

### 5.8 `script/`: PEX → `.pexfb`

`script/pex.*` (header and string table, enough to identify a file, also
from Fallout 4) and `pex_script.*` (everything: objects, variables,
properties, states, functions, bytecode, debug line numbers) decode
big-endian PEX. `pack/script_asset.*` writes the result as a FlatBuffer
(`formats/schema/script.fbs`), so the engine's VM never parses PEX.

### 5.9 `animation/`: Havok without the SDK

`animation/hkx.*` reads Skyrim's Havok packfiles (hk_2010.2.0-r1; LE with
4-byte pointers, SE with 8-byte). Members sit at fixed offsets per pointer
size. It reads skeletons, bindings, spline-compressed and interleaved
animations with annotations, behaviour character names and clip
generators; everything else is counted and skipped. Clips stay splines,
requantized to 16 bits (54 MiB for all vanilla clips, against 1 GiB
sampled). `animation_data.*` parses the `animationdata` text files (clip
ids, playback speed, root motion). Both become `.animfb` assets
(`formats/schema/animation.fbs`).

### 5.10 `pack/`: assembling a pack

**`prepare_inputs()`** (`pack/inputs.cpp`) decides what the install is before
`convert()` runs. From an `InputSpec` (the Data folder, a plugin list file, an
MO2 instance and profile, or explicit sources) it builds the mount plan and
the load order (with the Creation Club plugins), mounts the plan, and fills the
manifest's `input` record. The load-order problems, mount failures and
unloaded archives come back for the caller to print; the CLI's `convert`
calls it and only prints. `string_fetch(set)` hands a mounted set to the merge
as its string-table source.

**`convert()`** (`pack/convert.cpp`) runs everything over one mounted
install:

```
convert(set, order, options)
 ├─ PackWriter::create(out)                       opens/creates the pack dir
 ├─ if write_records:
 │   ├─ MergedWorld::build(order)                 merge pass 1 (index)
 │   ├─ write_snapshot(world, ...)  -> records.fb merge pass 2 (payloads)
 │   └─ write_world(world, ...)     -> world.fb   merge pass 3 (WorldSink)
 ├─ work = every vpath in the mount (filtered, sorted, limited)
 ├─ for each vpath (sequential):
 │   ├─ kind_of(extension) -> mesh | texture | script | lod | animation | deferred
 │   ├─ bytes = set.read(vpath)
 │   ├─ slot = writer.reserve(vpath, kind, bytes, source)   <- content hash
 │   ├─ if slot.already_present: writer.reuse(slot); continue  (dedupe)
 │   ├─ converted = convert_<kind>(bytes, vpath, options)   (pack/asset_conversion.cpp)
 │   └─ writer.warn(...) per warning; then writer.fail(...) or writer.store(slot, ...)
 └─ writer.finish(manifest)  -> assets.idx, vpath.idx, manifest.json, report.json
```

- **Content hash** (`pack/content_hash.*`): BLAKE3 over the length-prefixed
  *source bytes*, converter version and a per-kind *settings fingerprint*
  (`ConvertOptions::mesh_settings()` and so on, e.g. `mesh/19;collision=1;…`).
  Because the hash covers the input, duplicates are found before any
  conversion, and an unchanged file in a rebuild costs one hash. Any option
  that changes the output must be part of the fingerprint, or rebuilds reuse
  stale assets. When the writer's output changes, the leading number
  (`mesh/19`) is bumped.
- **Asset store** (`pack/asset_store.*`): by default one append-only blob
  `assets-<gen>.blob` plus `assets.idx` (magic `BCAI`, generation, count,
  covered blob bytes; entries `hash[32], offset, size, kind`, sorted by
  hash). The index is replaced atomically at the end; bytes past the
  covered length are leftovers of an interrupted run and are overwritten.
  `--prune` compacts into the next generation's blob. `--store loose` writes
  `assets/<bb>/<hash>.<ext>` instead.
- **`vpath.idx`** (`pack/vpath_index.*`): a sorted, tab-separated text
  index, `virtual path \t content hash \t kind \t winning source`. Text, so
  people and tools can diff mod overrides.
- **`manifest.json`**: pack format version, converter, language, input
  (Data folder or MO2 instance and profile, so the pack tool can rebuild),
  texture profile, load order, source hashes (plugins always, archives on
  request), summaries of records and world.
- **`report.json`**: every failure (vpath, stage, error kind, detail), every
  warning, and unconverted files counted by extension.
- **`records.fb`** (`pack/snapshot_*`): a 64-byte header (`BETHSNAP`,
  version), then a blob of every winning record's field bytes (64-bit
  offsets, because SE's payloads alone are 527 MiB), then a FlatBuffer index
  (form → type, flags, parent, plugin, payload span; editor-id, type, child
  and cell-grid indices). It is memory-mappable and verified on read.
  `bethconv verify --against` re-runs the merge and compares. The engine
  currently reads only its header.
- **`world.fb`** (`pack/world.cpp`, `pack/world/`, `pack/world_file.cpp`): the
  engine's database; see 5.11.
- **LOD assets** (`pack/lod_asset.*`): `.lod` settings, `.lst` tree types and
  `.btt` tree instances decoded into `.lodfb`. `.btr`/`.bto` LOD meshes are
  NIFs and convert as meshes.
- **`pack_view`**: rebuilds a browsable directory tree from a pack (GLBs
  with images added, DDS hard-linked or copied) for Blender and the Godot
  editor. It is derived and disposable.

### 5.11 `world.fb`: how the engine's database is made

`write_world` (`pack/world.cpp`) runs one more merge pass with a `WorldSink`
(`pack/world/sink.*`). FormIDs *inside* payloads are plugin-local, and only
during the merge is it known which plugin each winning record came from,
which decides how to resolve them (`CollectContext::global(merged, local)`,
`pack/world/context.*`, shared by every collector).

1. **Collect.** `WorldSink::on_record` switches on the record type and hands
   the record to the collector that owns it. There is one per domain, in
   `pack/world/`, each with `collect()` for its types, its own id-keyed maps
   of `World*` structs, and `write_*` functions for its tables:
   `places` (CELL, REFR, ACHR, LAND, NAVM, LGTM), `bases` (LIGH, MATO, ADDN,
   TXST, LTEX, GRAS, and any other type with a model or scripts as a generic
   base), `environment` (WRLD, WATR, CLMT, WTHR, SPGD, REGN, IMGS), `quests`
   (QUST, GLOB), `actors` (NPC_, RACE, ARMO, ARMA, OTFT, LVLI, LVLN) and `ai`
   (PACK, FLST). NPC_, ARMO and LVLN are also read as generic bases, from a
   copy of the reader; INFO is skipped. Each handler parses with the
   `record::parse_*` function, resolves FormIDs, and stores the result. A
   REFR's extras (scripts, locks, linked refs, activate parents, primitives,
   light overrides, load-door links) are stored per parent cell.
2. **Derive.** Interior lighting is resolved against its lighting template
   (`resolve_lighting`, XCLL inherit flags), default package
   lists are expanded, and so on. What a writer needs from another domain
   (the NPCs' package lists, from `ai`) is passed in by const reference.
3. **Write.** Every collection is written sorted by id, so the engine can
   binary-search it, and the root `World` table gets `format_version`
   (`k_world_format_version`, 10 in the working tree). FlatBuffers lays bytes
   out in the order tables, strings and vectors are created, so the sequence
   of collector calls in `write_world` is part of what makes the file's bytes
   reproducible.

`WorldFile` (`pack/world_file.cpp`, the reading side) is used by the CLI
(`bethconv cell`) and the tests. It copies FlatBuffer tables back into the
`World*` structs. The engine does *not* use it; it reads the FlatBuffer
directly.

### 5.12 The CLI (`tools/bethconv-cli/`)

`main.cpp` sets up the CLI11 app and calls one `register_<name>` per
subcommand: `convert`, `detect`, `mo2`, `target`, `info`, `cell`, `view`,
`verify`, `scan`, `extract`, `mesh`, `texture`, `script`, `animation`,
`records`, `forms`, `strings`, `merge`, `snapshot`, `loadorder`, `probe`. Each
lives in `commands/<name>.cpp` with its args struct, its `cmd_*` function and
its options; what several share (mounting, the load order `merge`,
`snapshot` and `verify` agree on, the output-target check, the exit status) is
in `common.*`. `front_end.*` holds the commands a graphical front end needs
(`detect`, `mo2`, `target`, `info`), the output-target verdict
(`check_target`), the JSON shapes and `k_json_version`. With `--json`,
`detect`, `mo2`, `target`, `info`, `cell` and `convert` print
machine-readable JSON (`converter/docs/cli-json.md`). `convert --json` streams
one event per line (`start`, `progress` per phase, `result`/`error`); the pack
tool reads that stream.

---

## 6. The pack: the contract between the halves

`formats/pack-format.md` is the specification; this is the short version.

| File | Written by | Read by the engine | What |
| --- | --- | --- | --- |
| `manifest.json` | `PackWriter::finish` | `SkydotPack::open` | versions, inputs, load order, summaries; the engine checks `pack_format_version == 6` and the index counts |
| `vpath.idx` | `PackWriter` | `PackStore::read_index` | game path → content hash, kind, winning source |
| `assets.idx` + `assets-NNNN.blob` | `AssetStore` | `PackStore::open_blob` (mmap) | asset bytes by hash |
| `records.fb` | `write_snapshot` | header only | every merged form with its raw fields |
| `world.fb` | `write_world` | `SkydotWorld::open` -> `WorldData::open` (mmap) | cells, refs, bases, terrain, weather, quests, actors, AI, ... |
| `report.json` | `PackWriter` | pack tool | failures and warnings |

Asset kinds and their bytes:

| Kind | Extension in the pack | Content | Engine reader |
| --- | --- | --- | --- |
| `mesh` | `.glb` | glTF 2.0 binary; Bethesda data in `extras.bethconv` | Godot `GLTFDocument` in `AssetCache::load_scene` |
| `texture` | `.dds` | DDS, block formats kept | `Image::load_dds_from_buffer`, cube maps split per face |
| `script` | `.pexfb` | `script.fbs` | `vm::ScriptClass` |
| `lod` | `.lodfb` | `lod.fbs` | `SkydotLod` |
| `animation` | `.animfb` | `animation.fbs` | `SkydotAnimation`, locomotion |

Coordinates in `world.fb` are Skyrim's: Z-up, game units, radians. Meshes
carry the conversion in their root node; the engine converts placements
(`engine/docs/coordinates.md`).

---

## 7. The engine extension (skydot)

The sources under `engine/extension/src/` are grouped by what depends on what,
bottom up: `data/` (world.fb as read), `nav/`, `render/`, `physics/`,
`actors/`, `build/` (cell building), `world/` (`SkydotWorld`), then `ai/` and
`vm/` on top, with `assets/` for the pack. `extension/CMakeLists.txt` lists
each and the few places where the layering is not clean yet (`render/lod` and
`render/weather` take a `SkydotWorld`; `assets/` includes `physics/` and
`actors/`).

### 7.1 Entry point and classes

`register_types.cpp` exports `skydot_library_init` and registers its classes
at the SCENE level:

| Class (Godot base) | File | Role |
| --- | --- | --- |
| `SkydotPack` (RefCounted) | `assets/pack.*` | mounts a pack, checks every version and count, loads models, textures and bytes, opens `SkydotWorld` |
| `SkydotModel` (Resource) | `assets/model.*` | a converted mesh as a node-tree template, instanced by duplication |
| `SkydotWorld` (RefCounted) | `world/world.*`, `world/queries*.cpp`, `build/cell_builder.*` | the class scripts talk to: world.fb queries, cell and exterior building, resources; holds a `WorldData` and an `ActorPlacement` (7.3) |
| `SkydotMaterials` (RefCounted) | `render/materials.*` | Skyrim-style shader materials, fog sync, shader warm-up |
| `SkydotAnimator` (Node) | `render/animator.*` | plays the clips in a model's extras |
| `SkydotParticles` (Node3D) | `render/particles.*` | NiParticleSystem as GPUParticles3D |
| `SkydotFlicker` (Node) | `render/flicker.*` | flickering and pulsing lights |
| `SkydotBillboard` (Node) | `render/billboard.*` | NiBillboardNode |
| `SkydotLod` (Node3D) | `render/lod.*` | distant terrain, object and tree LOD |
| `SkydotWeather` (Node3D) | `render/weather.*` | time of day, weather, sky, clouds, sun, moons, stars, precipitation, lightning |
| `SkydotImageSpace` (CompositorEffect) | `render/image_space.*` | IMGS as a compositor effect (tone mapping, saturation, tint, contrast, eye adaptation) |
| `SkydotDynamicBody` (RigidBody3D) | `physics/collision.*` | movable clutter |
| `SkydotPlayer` (CharacterBody3D) | `physics/player.*` | the walking player |
| `SkydotActor` (SkydotPlayer) | `actors/actor.*` | a placed NPC that walks paths, opens doors |
| `SkydotAnimation` (RefCounted) | `actors/actor_animation.*` | skeletons and clips from `.animfb`, skinning onto skeletons |
| `SkydotAi` (RefCounted) | `ai/ai.*`, `packages.*` | AI packages against a game clock |
| `SkydotPapyrus` (RefCounted) | `vm/papyrus.*`, `vm/quests.cpp`, `vm/save.cpp` | the Papyrus VM bound to the world |

Non-Godot helpers: `AssetCache`, `PackStore`, `TerrainBuilder`,
`WaterMaterials`, `ModelCollision`, `NavIndex`, `ActorPlan`, `Locomotion`,
packages, and the whole `vm::` namespace (`vm.*`, `script_class.*`,
`value.hpp`, `save.cpp`), which does not depend on Godot at all.

### 7.2 Assets: from pack bytes to Godot resources

```
SkydotPack::open(dir)
  ├─ manifest.json: pack_format_version == 6, store kind, counts
  ├─ records.fb header (BETHSNAP, version) if listed
  ├─ world.fb exists if listed
  └─ PackStore: vpath.idx (header line, 4 tab fields, 64-hex hashes)
                assets.idx + blob (mmap; every entry inside the blob)  | loose dir
       │
       ▼
AssetCache (shared_ptr; shared by pack, world, LOD, materials)
  request(vpath)  -> queued for 1–4 std::thread workers (half the cores, max 4)
  get(vpath)      -> cached, or loaded *on the calling thread* (never waits
                     for a worker: a worker making GPU resources may itself
                     wait for the main thread)
  load(key):
     mesh     -> GLTFDocument.append_from_buffer + generate_scene
                 -> SkydotModel (template + ModelCollision + the textures
                    its materials name, loaded on the same worker)
     texture  -> DDS -> ImageTexture | Cubemap (faces split by header edit)
     "clip:<clip>|<skeleton>" -> SkydotAnimation::build_clip_for (sampled
                 Animation, so actors don't stall the main thread)
  trim()          -> drop entries nothing outside the cache holds
```

`SkydotModel::instantiate()` duplicates the template's node tree. Meshes,
materials and animations are shared, and nothing is read back from the GPU
(which `PackedScene::pack` would do).

Headless runs (tests) load on the calling thread, because Godot's headless
renderer creates resources without locking.

### 7.3 `SkydotWorld`: the world database and the builder

`SkydotWorld` (`world/world.*`) is the class GDScript talks to, and little
more: its bound methods forward to three parts it holds, and `open` makes them
point at the new file.

```
SkydotWorld ── data_       std::shared_ptr<const WorldData>: the file and its indexes
            ├─ placement_  ActorPlacement: where SkydotAi has moved actors
            ├─ builder_    CellBuilder: builds cells; owns jobs, caches, BuildOptions
            │               └─ decorator_  Decorator: what is done to a placed model
            └─ ref_cells_  queries::RefCellIndex: reference -> cell, built on first ask
```

The read-only calls (`list_cells`, `get_cell`, `get_ref_info`, `get_sky`,
`get_quest`, ...) are free functions over a `const WorldData&` in
`world/queries_*.cpp`, declared in `queries.hpp`, so a query cannot change
the world and a method of `SkydotWorld` that is not const is one that
builds, loads or fills a cache. (The CONST flag of `build_cell`,
`begin_cell`, `begin_exterior`, `continue_build*`, `build_ref`,
`build_actor`, `get_cell_resources`, `get_exterior_resources`,
`get_locomotion` and `get_ref_cell` is therefore gone from ClassDB; nothing
else in the class list changed.)

**`WorldData`: the file, read-only.** `SkydotWorld::open(path)` refuses a
second open (a world is opened once; `SkydotPack::open_world` makes a new
object), then builds a fresh `WorldData` and keeps it only if it opened. Its
`open` memory-maps `world.fb` (`assets/mapped_file.*`, the mapping the asset
blob uses; `res://` and `user://` paths are globalized first), verifies it
with the FlatBuffers verifier, checks `format_version` (8-10 accepted) and
builds every index once:

- `exteriors_`: (world, x, y) → exterior cell;
- `persistent_`: a worldspace's persistent cell references bucketed by the
  grid square they stand in, so an exterior build includes them;
- `doors_`, `activate_children_`, `enable_children_`, `enable_parents_`,
  `navmeshes_`, `cell_actors_`, `persistent_actors_`, `actor_of_` (NPC →
  its lowest placed actor).

It also offers the lookups the rest of the engine uses (`cell_ptr`,
`base_ptr`, `world_ptr`, `exterior_ptr`, `water_ptr`, `land_world`,
`water_type`, `door_ptr`, `initially_disabled`, ...). Nothing in it changes
after `open`, so it is shared by `const` reference: `SkydotWorld::data()`
hands it to `SkydotAi` and `SkydotWeather`, which have no other access to
the world's internals. A closed `WorldData` (before `open` succeeds) answers
every query with nothing.

Still built lazily on first use: `ref_cells_` in `SkydotWorld` (reading
every reference takes most of a second on the SE pack, so not in `open`), and
in `CellBuilder` the locomotion clips, grass models and the terrain builder,
in `Decorator` the add-on node models and projected (MATO) materials. Each
belongs to the object that fills it, which is not const where it changes.

**`ActorPlacement`: where actors are.** The editor's places come from
`WorldData`; the ones `SkydotAi` has moved actors to are kept here
(`places_` by actor, bucketed by the interior cell or grid square they
fall in). `actors_in_cell`/`actors_in_grid` answer which actors stand in a
place *now*: those placed there and not moved away, plus those moved in. The
bound `set_actor_place`, `clear_actor_place(s)` and `get_actor_place` of
`SkydotWorld` forward to it; C++ callers use `SkydotWorld::placement()`.

Lookups by id are flatc's `LookupByKey` (the schema marks the sorted vectors'
ids `(key)`); `lookup` in `data/fb_search.hpp` also tolerates an absent
vector, and `first_at_least` there finds the run of one reference in the
vectors that can hold several (links, activate parents, primitives).

**Queries for scripts and tools** return `Dictionary`/`Array`: `list_cells`,
`get_cell`, `get_refs`, `get_base`, `get_door`, `get_ref_info`, `pick_ref`
(ray against physics bodies, then model bounds; `build/refs.cpp`, it reads
no world data), `get_quest`, `get_actor`, `get_navmesh`, `get_sky` (weather
colours at an hour), and more.

**Building is incremental.** `CellBuilder` (`build/cell_builder.*`) builds
cells; a cell can take tens of milliseconds, so it is split into a cheap
begin and budgeted continue calls:

```
begin_cell(id) / begin_exterior(world, x, y)       immediate:
   exterior: terrain (TerrainBuilder: one mesh per quadrant, layer blending,
             normals using neighbour cells' edges, collision shape),
             water plane (WaterMaterials by WATR), grass,
   both:     navigation regions (nav/navmesh.cpp),
   both:     a BuildJob {refs to place, actors later, stats, kept resources}
             registered under the root's instance id
continue_build(root, budget_usec)                   repeated per frame:
   place_refs: place_ref() for each ref until the budget is spent
               (at least one per call)
   then, once: which actors stand here *now* (ActorPlacement::actors_in_cell/
               grid, which honour SkydotAi's moves), place_actor() each within budget
   done:       "skydot_stats" meta on the root; the job is dropped
continue_build_static(root, budget)                 refs only (preloading)
```

`build_cell`/`build_exterior` are the same with an unlimited budget.
`get_cell_resources`/`get_exterior_resources` list every model, texture,
land texture and actor clip a place needs; `request_cell`/`request_exterior`
queue them on the asset cache and return how many are still pending, so a
caller begins a build only when nothing would load on the main thread.

**`place_ref`** (`CellBuilder`): placing one reference:

1. Skip it if it is initially disabled, following the enable-parent chain
   with its "opposite" flags (up to 16 deep).
2. Look up its base. If the base has a model and is not an editor marker:
   - instance the `SkydotModel` and apply the reference transform
     `C·T·C⁻¹` (`skyrim_transform`);
   - decorate it (`Decorator::decorate`, `build/decoration.cpp`): one
     ordered table of steps, each checking its own `BuildOptions` setting:
     1. reset the NIF root's own transform (the game ignores it);
     2. apply BSOrderedNode draw order (`sorting_offset`);
     3. with Skyrim materials: replace materials (`SkydotMaterials::apply`);
     4. water surfaces (cell water type);
     5. projected/directional material (STAT DNAM → MATO);
     6. with Skyrim materials and effects: add-on nodes (ADDN models such
        as candle flames; each goes through the same table, without steps
        4-6 and 9-11);
     7. billboards;
     8. with effects: `SkydotAnimator` (clips) and particles;
     9. tag the node with ref/cell/activatable metadata for picking and
        scripts;
     10. mark plain doors for actors;
     11. collision bodies last, so material and effect passes never see them.
3. If the base is a light: an `OmniLight3D` or `SpotLight3D` whose radius
   and fade combine the LIGH record with the reference's own XRDS/XLIG
   overrides, shadow flags (or all shadows if configured), negative lights,
   and an optional `SkydotFlicker` child.

### 7.4 Rendering

- **Materials** (`render/materials.*`). Godot's glTF importer produces
  `StandardMaterial3D`s; `SkydotMaterials::apply` replaces each with a
  `ShaderMaterial` from one of three families, driven by
  `extras.bethconv`:
  - *lighting* (BSLightingShaderProperty): albedo, a normal map whose alpha
    is the specular mask, Blinn-Phong specular, glow, environment map,
    vertex colours, alpha test or blend, skin/hair/face tints, model-space
    normals;
  - *effect* (BSEffectShaderProperty): unshaded, greyscale-to-palette,
    falloff, blend modes;
  - *refraction*.

  The shader code is in files (the next bullet); C++ prepends
  `shader_type`, `render_mode` and `#define` lines per feature set, adds the
  fog and ambient, and caches a `Shader` per variant. `warm_up()` creates
  every variant up front. Converted materials are cached per source
  material, so instances share them.
- **Shader files** (`game/shaders/`, `render/shader_source.*`). Every
  shader's text lives in `engine/game/shaders/`, not in C++ strings:
  `.gdshader` for a whole shader, `.gdshaderinc` for a fragment the C++ puts
  after other text, `.comp` for the image space's GLSL. `shader_source::load`
  reads `res://shaders/<name>` through `FileAccess` once per process, cuts
  the `/* ... */` header comment each file starts with (it says which C++
  assembles the file), and reports a missing file with `push_error` and an
  empty shader. The code that assembles variants is unchanged:
  `lighting_code`, `effect_code`, `with_game_fog` and `with_game_ambient` in
  `materials.cpp` (the fog and ambient lines they insert are still C++),
  `TerrainBuilder::shader_code` (two `%LAYER_...%` marker lines in the file
  are filled per layer count), `WaterMaterials::shader_code`, `lod_code` in
  `lod.cpp`, and `weather.cpp`, `particles.cpp` and `image_space.cpp`. The
  files are plain text, not imported, so they work headless and ride in the
  `.pck` (the `.comp` files need a non-resource filter in an export preset).
  `SkydotMaterials.shader_sources()` returns the final code of every shader
  the engine can produce, and `game/tools/shader_dump.gd` prints a hash of
  each: moving shader text is proved by identical dumps before and after
  (`smoke_shaders` checks that none is empty). The next step is Godot's own
  `#include` and `#define`; that changes the code strings, so only
  `--from-shot` re-renders can verify it (`game/shaders/README.md`).
- **Gamma-space lighting.** Light colours and blending follow the game's
  gamma-space arithmetic (`set_game_light`). The
  `SkydotImageSpace` compositor effect then applies the game's ISHDR
  tone-mapping maths and hands Godot linear colour. Without that pass the
  gamma-space numbers would show too bright.
- **Fog.** The engine's shaders compute the game's fog formula themselves
  (`with_game_fog`) and write `FOG`. Values reach them through the
  Environment via `SkydotMaterials::sync_fog`, called every frame.
- **Terrain** (`render/terrain.*`): LAND heights and normals, VTXT layer
  blending per vertex, land textures by LTEX → TXST; one mesh per quadrant.
- **Water** (`render/water.*`): WATR noise layers, depth colour, refraction,
  Fresnel to the sky or a short screen-space reflection march, sun glint.
- **LOD** (`render/lod.*`, `engine/docs/lod.md`): a quadtree over the
  worldspace's LOD grid (4/8/16/32-cell quads), terrain and object LOD
  meshes, tree billboards as MultiMesh. A per-cell mask texture lets the
  LOD shaders discard fragments over loaded full-detail cells.
- **Weather** (`render/weather.*`, `engine/docs/weather.md`): region or
  climate weather choice, cross-fades, sky gradient, 29 cloud layers on
  `clouds.nif`, sun, moons with phases, stars, GPU rain and snow, lightning.
  It also owns the outdoor clock.
- **Grass** (`render/grass.*`): GRAS per land texture, grid
  placement by density, slope and water limits, MultiMesh per type and cell.
- **Effects**: `SkydotAnimator` plays unnamed clips on the shared clock
  (every copy flickers in step) and named sequences on request (`Open`,
  `Close`); `SkydotParticles` builds GPU particles with a process shader
  that follows Gamebryo's modifiers; `SkydotFlicker`; `SkydotBillboard`.

### 7.5 Physics, player and picking

`engine/docs/physics.md` has the details. The physics engine is Jolt.
`ModelCollision` is parsed once per model on the loader thread from the mesh
extras. Its shapes are created on first use on the main thread and shared.
Per instance, `attach` makes `StaticBody3D`s (fixed), `AnimatableBody3D`s
(keyframed or animated: doors, gates) or one `SkydotDynamicBody` (movable
clutter, frozen until touched). `SkydotPlayer` is a flat-bottomed cylinder
with step-up, swimming, flying and holding. `pick_ref` casts the view ray
against bodies first, then model bounds.

### 7.6 Navigation

`nav/navmesh.*` turns each NAVM into a `NavigationRegion3D`. Portals
between cells are joined by Godot's edge connection margin (1 m, set in
`project.godot`), ledges become `NavigationLink3D`s, and the cell size is
0.01 m so nearby vertices are not merged (`engine/docs/navigation.md`).

### 7.7 Actors

Building an actor (`engine/docs/actors.md`):

1. `data/actors.cpp` makes an `ActorPlan` from `world.fb` alone: it follows the
   template chains, picks deterministically from leveled lists, and
   determines race and sex, skeleton (`.hkx`), outfit armor, skin addons
   where nothing worn covers the slot, the FaceGen head, and weight
   variants.
2. `SkydotAnimation` builds the `Skeleton3D` from the skeleton asset and
   moves each converted body part's skinned meshes onto it.
3. `actors/locomotion.cpp` finds the idle, walk and run clips by name in the race's
   behaviour project, with root-motion speed from `animationdata`. Clips
   come from the asset cache (`clip:` keys, sampled on workers).
4. The result is a `SkydotActor` (a `SkydotPlayer` steered by itself): it
   walks navigation paths with the animation speed matched to its motion,
   opens plain doors in its way, and wanders when no package drives it.

### 7.8 AI packages

`ai/packages.*` reads PACK records from `world.fb`. For each actor it
picks the first package whose schedule (PSDT) and conditions (CTDA) pass,
and flattens its procedure tree (Sequence, Stacked, Random, Simultaneous)
into steps. `SkydotAi` runs them against its clock:

- for *built* actors it steers `SkydotActor` (travel, sandbox, sit/sleep at
  furniture, patrol along linked refs, lock/unlock doors);
- for *unbuilt* persistent actors it moves their stored place
  (`ActorPlacement::set_place`), in slices (`begin_placing`,
  `settle_actors`), so later builds put them where their schedule says;
- actors arriving in the shown space appear at the door they come through.

See `engine/docs/ai.md`.

### 7.9 Papyrus

The VM is in `engine/extension/src/vm/` and documented in
`engine/docs/papyrus.md`.

- **`vm::ScriptClass`** loads a `.pexfb`, checks every index the interpreter
  will follow, and resolves identifiers once (register, variable, `self`,
  `::State`).
- **`vm::Vm`** holds instances (scripts attached to forms, variables per
  class in the inheritance chain, the current state), threads with explicit
  frame stacks (no native recursion), a cooperative deterministic scheduler
  (100,000 instructions per thread per update), latent natives (wait for
  seconds or for a named event with a timeout), update timers, a seeded RNG,
  and save/load of the complete state (`save.cpp`). Dispatch looks a method
  up in the attached script derived from the static class, in the current
  state first, then the default state, up the chain.
- **`SkydotPapyrus`** binds the VM to the pack and `SkydotWorld`. It loads
  classes from script assets, attaches base and reference scripts with VMAD
  property values (`attach`, `attach_cell`, `attach_built` with `OnLoad`),
  handles trigger volumes (XPRM boxes and spheres, `OnTriggerEnter`/`Leave`
  via `update_actor`), and runs quests (`vm/quests.cpp`: aliases as handle
  objects, stages and fragments, objectives, globals). Natives that change
  the world never touch nodes. They update SkydotPapyrus's own view (enabled,
  open, locked, blocked) and emit signals (`enable_changed`, `open_changed`,
  `lock_changed`, `play_animation`, `havok_impulse`, `motion_type_changed`,
  `activate_requested`, `trigger`, `quest_started`/`quest_stage`/
  `objective_changed`, `message`, `error`); the game layer (the viewer)
  applies them to nodes.

### 7.10 Coordinates

Skyrim is Z-up in game units; Godot is Y-up in metres. The conversion is a
−90° rotation about X and 0.0142875 m per unit
(`formats/include/skydot_formats/units.hpp`, with the cell size and the Havok
scale; scripts read them as `SkydotWorld.unit_scale()` and
`SkydotWorld.CELL_UNITS`, so no script spells them). A Skyrim point (x, y, z)
becomes (x, z, −y)·s. A reference's rotation is `Rx(−x)·Ry(−y)·Rz(−z)`, and it
is placed with `C·T·C⁻¹` (`engine/docs/coordinates.md`). An exterior cell is
4,096 units square; its grid square is `floor(pos / 4096)`.

---

## 8. The Godot project (GDScript)

`engine/game/project.godot`: Forward+, Jolt physics, raised clustered-light
and shadow-atlas limits (Forward+ has no per-object light limit; the
busiest vanilla interior has 122 lights), navigation cell size 0.01 m and a
1 m edge connection margin. The main scene is the pack tool; the viewer is
`res://viewer/cell_viewer.tscn`.

### 8.1 Pack tool (`game/packtool/`)

A GUI for people without a terminal. It drives the `bethconv` CLI as a
child process (`bethconv.gd`):

- `query(args, callback)`: short commands (`detect`, `mo2`, `target`,
  `info`, `cell`) on a `Thread`; the last JSON line is the result.
- `start(args)`: `convert --json` through `OS.execute_with_pipe`, with one
  reader thread per pipe; stdout lines are JSON events, stderr lines are
  log text. Signals: `event`, `log_line`, `finished`.
- `convert_page.gd`: pick a detected install, optionally an MO2 instance and
  profile, an output folder (checked live with `bethconv target`), texture
  options; then progress and the result.
- `packs_page.gd`: packs written or found (size, stale blob bytes,
  failures); rebuild with the recorded inputs, delete, or open in the
  viewer.
- `ui.gd`: small helpers that build controls in code. There are no `.tscn`
  pages besides the root.

### 8.2 Cell viewer (`game/viewer/`)

`cell_viewer.gd` is the composition root and the frame loop: about 300
lines of code, and the documentation of every option in its header. It opens
the pack and world, creates `SkydotPapyrus`, `SkydotAi` and
`SkydotImageSpace`, makes the parts below, starts the start-game quests and
enters the place the options name (an interior, at the arrival spot of the
door leading in, or an exterior). Each part is a class with typed state and a
small API. None has a `_process` of its own: the root calls them in the order
the frame needs, as the one script did before.

| File | Class | What it owns |
| --- | --- | --- |
| `viewer_settings.gd` | `ViewerSettings` | the options, parsed once into typed fields from the command line or, with `--from-shot`, a shot's JSON (the command line wins); `SHOT_FORMAT` |
| `game_clock.gd` | `GameClock` | the time of day. Outside `SkydotWeather` runs it, inside `SkydotAi`'s clock does; the clock hands it over when a place is left (`release_weather`), every frame (`sync`) and for T (`shift`), and says it for shots (`describe`) |
| `player_rig.gd` | `PlayerRig` | the camera and the `SkydotPlayer`, yaw and pitch, `place_camera`, following the eyes, the walking checks (land lift, fall-through catch) |
| `player_input.gd` | `PlayerInput` | keyboard and mouse. Movement, look and jump go to the rig; every other key is a `command` signal. Keys are InputMap actions declared in `game/project.godot` (the same logical keys as before), so a gamepad or VR shell can map its own events to them |
| `world_streamer.gd` | `WorldStreamer` | the exterior: dropping, loading and finishing cells nearest first within `--build-budget` µs, the LOD and its masking, the camera's cell, radius and LOD detail keys, holding the player while the ground is built; `cell_finished` signal |
| `door_preloader.gd` | `DoorPreloader` | the load doors of the place shown and, near one, the place behind it built in the background (`Preparation`, a typed class) |
| `held_place.gd` | `HeldPlace` | `hold`/`release`: a place built ahead is in the scene, hidden, without physics and off the navigation map |
| `place.gd` | `Place` | the place shown: entering an interior or a worldspace, its sky, light and environment, the weather start, leaving the old one, actors the AI brings in; signals `built`, `left`, `message`, `failed` |
| `place_transition.gd` | `PlaceTransition` | the fade phases through a load door and `travel`, which takes what the preloader built |
| `script_bridge.gd` | `ScriptBridge` | the `SkydotPapyrus` signal handlers (enable, animate, impulse, motion type, open, lock, quests, messages), `activate` (locks, load doors, plain doors, scripts, activate parents), finding a reference's node, the `--activate` run |
| `save_service.gd` | `SaveService` | F5, F9, `--save-to`, `--load`: the VM state plus the place and camera |
| `shot_recorder.gd` | `ShotRecorder` | F12 shots (PNG and JSON with place, camera in engine and game terms, time, weather, options, pack hashes, GPU, console commands and the note the user types), `--screenshot`, `--shot-delay` |
| `debug_overlay.gd` | `DebugOverlay` | a `CanvasLayer`: notes, journal, navmesh overlay, path drawing, position text, actor inspection |
| `benchmark_run.gd` | `BenchmarkRun` | `--benchmark`: the flight and its frame statistics |

Per frame (`_process`): fog sync and image space; during a `--screenshot`
run only the shots; then the player held or tracked and the camera following
it, `WorldStreamer.update` (cells, LOD), `DoorPreloader.step`,
`PlaceTransition.step`, Papyrus (`update_actor` for triggers, `update`), the
clock hand-off and `SkydotAi.update`, the notes' age, the countdown that
quits an `--activate` run, scripted activations, the benchmark, then input.

The parts do not know the root. What a built cell needs next (scripts, navmesh
overlay, load door registration) is the root's `_on_cell_built`, connected to
`WorldStreamer.cell_finished` and `Place.built`; notes and failures are
signals. `_input = false` (tools set it) turns off `PlayerInput` and the
door fades. `WorldStreamer`, `DoorPreloader` and `GameClock` run every frame
and every front end needs them, so they are the parts to move into C++ when
the VR shell starts.

### 8.3 Tools and tests

- `game/tools/*.gd`: headless or windowed measurement drivers against a real
  pack (`preload_check`, `door_check`, `collision_check`, `nav_check`,
  `actor_check`, `ai_run`, ...). They set `viewer._input = false` so the
  user's mouse does not interfere. `scene_dump` is a scene oracle: it
  builds interiors and exterior blocks, pauses the tree and prints a canonical
  text dump of every node, so a refactor of the builders is proved by
  diffing two dumps. `shader_dump` is the shader oracle: it hashes the code of
  every shader the engine can produce, including those no scene reaches (sky,
  LOD, particles, image space).
- `engine/tests/smoke/*.gd`: headless editor runs registered with ctest (see
  section 12).

---

## 9. Walkthroughs

### 9.1 `bethconv convert --data …/Data -o pack`

1. `check_target`: refuse a FUSE or rotational target for loose output.
2. `pack::prepare_inputs`: build the mount plan (Data, or Data + MO2
   profile) and the load order (`LoadOrder::build` from the profile's or the
   game's `plugins.txt`, with CC plugins hoisted).
3. The same call mounts the plan with `install::mount`: archives, then loose
   folders, increasing priority.
4. `pack::convert`:
   - merge pass 1 → `MergedWorld`;
   - pass 2 → `records.fb`;
   - pass 3 (`WorldSink`) → `world.fb`;
   - every winning vpath, sorted: hash, dedupe, convert by kind, store in
     the blob;
   - `finish`: `assets.idx` replaced atomically, `vpath.idx`,
     `manifest.json`, `report.json`.
5. JSON events or terminal text report progress and the result.

### 9.2 Opening the viewer at Riverwood

1. `SkydotPack.open` checks the pack and mounts the store; `open_world()`
   reads and indexes `world.fb`.
2. The viewer creates `SkydotPapyrus`, `SkydotAi` and the player rig;
   `_start_game` starts the start-game quests; `Place.enter_exterior` creates
   `SkydotWeather` (sky, light, fog) and `SkydotLod`.
3. Each frame `WorldStreamer.step` requests the 5×5 cells around the camera (their
   models, textures, land textures and actor clips load on the cache's
   workers), then begins and continues builds within 8 ms per frame. LOD
   fills the rest and masks loaded cells. The player is held until the
   ground under it is built.

### 9.3 Pulling a lever

1. F → `pick_ref` finds the lever's reference from the camera ray.
2. `ScriptBridge.activate` checks the lock, then `SkydotPapyrus.activate(ref,
   activator)`, which sends `OnActivate` to the reference's scripts (and
   its aliases').
3. The script calls `PlayAnimationAndWait` (latent: the thread waits for a
   text key, with a timeout) and `Enable()`/`Disable()` on linked refs.
4. SkydotPapyrus updates its world view and emits `play_animation` and
   `enable_changed`. `ScriptBridge` plays the clip on the lever's
   `SkydotAnimator`; its `text_key` and `finished` signals call
   `SkydotPapyrus.notify_animation_event`, which wakes the waiting script
   thread. `ScriptBridge` also shows or hides the nodes and wakes clutter that
   rested on them.

### 9.4 Going through a load door

1. While the player is near the door, the place behind it has been prepared
   (`DoorPreloader.step`).
2. Activating the door calls `PlaceTransition.go(door)`: fade out
   (`begin_fade`), then `travel`; with no one at the keyboard, `travel` at
   once. `Place.leave` frees the current place, the prepared one
   (`DoorPreloader.take`) is released (or built now), the player is put at
   the arrival transform, the weather and LOD are switched, and actors are
   placed (`SkydotAi.begin_placing`/`settle_actors`, then `continue_build`
   places them).
3. After the build is complete and a few frames have been drawn, fade in.

---

## 10. Threading model

| Where | Threads | Shared state and protection |
| --- | --- | --- |
| Converter | Single-threaded pipeline; `bc_encode` uses worker threads per texture level | none needed |
| `AssetCache` | 1–4 `std::thread` workers (half the cores, max 4) plus the caller | `mutex_` around `ready_`, `in_flight_`, `queue_`; `get` never blocks on a worker, so occasionally an asset is loaded twice and the first result is kept |
| Worker loads | create Godot objects (`GLTFDocument`, `ImageTexture`, `Animation`) off the main thread | Godot's resource-creation thread safety; headless mode loads on the caller instead |
| `WorldData` | any thread: immutable after `open` | none needed |
| `SkydotWorld` | main thread only, except caches the workers' materials touch | `grass_mutex_`, `projected_mutex_`, `std::once_flag` for the add-on index |
| `SkydotMaterials` | main thread; shader variants cached | `projected_mutex_` |
| `SkydotImageSpace` | render thread (compositor) | `mutex_` |
| Papyrus VM | main thread; script "threads" are cooperative coroutines | none |
| Pack tool | GDScript `Thread`s for queries and pipe readers | results handed back with `call_deferred` |

---

## 11. Versioning

Several version numbers move independently:

| What | Where | Value (working tree) | Bumped when |
| --- | --- | --- | --- |
| Pack format | `k_pack_format_version` (`converter/include/bethconv/pack/vpath_index.hpp`), manifest, `vpath.idx` header; engine `SkydotPack::PACK_FORMAT_VERSION` | 6 | the pack layout's meaning changes |
| `records.fb` | `k_snapshot_format_version` (header) | per `snapshot.hpp` | `records.fbs` or the header changes |
| `world.fb` | `k_world_format_version` / engine `WORLD_FORMAT_VERSION` (min 8) | 10 | `world.fbs` meaning changes |
| Script, LOD, animation assets | `format_version` in each root table | 1 / per schema | their schema changes |
| Asset settings fingerprints | `ConvertOptions::*_settings()` (`mesh/19`, `texture/1`, `script/2`, `lod/1`, `animation/2`) | — | a writer's output changes for unchanged input (renames assets, no format change) |
| CLI JSON | `cli::k_json_version` | — | the JSON the pack tool reads changes |
| Shot JSON | `ViewerSettings.SHOT_FORMAT` | 1 | the F12 metadata changes |

Readers refuse unknown versions and say which numbers they read.

---

## 12. Testing

| Suite | Where | What it covers |
| --- | --- | --- |
| Unit (Catch2) | `converter/tests/unit/` | every layer, with synthetic inputs from `tests/support/*_builder.hpp` (ESM, BSA, NIF, DDS, PEX, HKX, strings, LOD); about 400 ctest entries under ASan/UBSan |
| Corpus | `converter/tests/corpus/` | real installs from `$SKYRIM_DATA_{LE,SE,VR}`, recording only counts, versions and hashes (`corpus-expectations.json`); skipped when unset |
| Fuzz | `converter/tests/fuzz/` | libFuzzer targets for ESM, BSA, NIF, DDS, PEX, HKX, strings, snapshot, LOD, assets, forms; also replayed as normal tests with seeds |
| Test pack | `converter/tools/testpack/`, `tests/testpack/` | a complete synthetic pack (meshes, a cube map, cells, doors, a lock, a lever with scripts, a quest); a determinism check builds it twice and compares |
| Engine smoke | `engine/tests/smoke/*.gd` | headless Godot runs against the test pack: extension loads, refusal paths, pack, assets, Papyrus, quests, LOD, animation, actors, AI, physics, navigation, weather, shaders, pack tool; the viewer's typed options (`viewer_settings.gd`, no pack) |
| Viewer runs | `engine/tests/CMakeLists.txt` | the viewer walks through doors, locked doors, a lever, `--from-shot`, save and load |
| Visual | F12 shots and `COMPARISON-SHOTS.md` (outside the repo) | rendering compared with game screenshots by hand |

---

## 13. Where to find things

| I want to… | Look at |
| --- | --- |
| add a field the engine needs from a record | `record/forms_*.hpp` (struct + `parse_*`), `pack/world/<domain>.cpp` (the collector's handler + `write_*`), `formats/schema/world.fbs`, bump `k_world_format_version`, `pack/world.hpp` and `pack/world_file.cpp` (`WorldFile`, if the CLI should show it), engine reader, `formats/pack-format.md` |
| support a new asset kind | `formats/include/skydot_formats/asset_kind.hpp` (`AssetKind`, its word and extension), `pack/convert.cpp` (`kind_of`, the switch, a settings fingerprint), `pack/pack_writer.*`, `formats/pack-format.md`, engine `AssetCache::load` |
| change how a NIF converts | `mesh/nif_reader.cpp`, `nif_controllers.cpp`, `gltf_writer.cpp`; bump `mesh/N` in `ConvertOptions::mesh_settings` |
| change mod or load-order handling | `install/mount_plan.*`, `install/mo2.*`, `record/load_order.*` |
| change how a reference is placed | `CellBuilder::place_ref` (`engine/extension/src/build/cell_builder.cpp`), and `Decorator::decorate` (`build/decoration.cpp`) for what is done to its model |
| change a shader | the file in `engine/game/shaders/` (its header comment names the C++ that assembles it; `game/shaders/README.md`); what the C++ adds per variant is in `render/materials.cpp`, `terrain.cpp`, `water.cpp`, `lod.cpp`, `weather.cpp`, `particles.cpp`, `image_space.cpp` |
| add a Papyrus native | `vm/papyrus.cpp` (`bind` calls), `vm/quests.cpp` for quest ones |
| change streaming or door transitions | `game/viewer/` (`world_streamer.gd`, `door_preloader.gd`, `place_transition.gd`, `place.gd`) |
| change a viewer key | the action in `game/project.godot` (`[input]`), `player_input.gd`, and the dispatcher `_on_command` in `cell_viewer.gd` |
| change what the pack tool shows | `game/packtool/*.gd`, `converter/docs/cli-json.md` |

---

## 14. Glossary

| Term | Meaning |
| --- | --- |
| ESM / ESP / ESL | plugin files (master, plugin, light plugin); ESL-ness is a header flag |
| FormID | 32-bit record id; top byte = index into the plugin's master list, made global by the load order |
| GRUP | a group in the plugin tree; its label is a type, a FormID, a block number or grid coordinates depending on the group type |
| Record / field | a form (header + payload); the payload is a list of typed fields (subrecords) |
| REFR / ACHR | a placed object / a placed actor in a cell |
| Base object | what a reference places: STAT, DOOR, LIGH, ACTI, CONT, FURN, NPC_, ... |
| CELL / WRLD | an interior or one 4,096-unit exterior square / a worldspace (Tamriel) |
| Persistent cell | a worldspace's cell holding references that are always loaded |
| LAND / LTEX / TXST | terrain heights and layers / land texture / texture set |
| XCLL / LGTM | a cell's lighting / a lighting template it can inherit from |
| WTHR / CLMT / REGN / SPGD | weather / climate / region / precipitation (shader particle geometry) |
| IMGS | image space: HDR, cinematic (saturation, brightness, contrast) and tint |
| NAVM | navmesh |
| VMAD | the scripts attached to a record, with property values |
| PEX | compiled Papyrus script |
| QUST / alias / fragment | quest / a named slot a quest fills with a reference / per-stage script function |
| PACK | AI package (schedule, conditions, procedure tree) |
| CTDA | a condition (function, comparison, value) |
| NIF | Gamebryo mesh format; BSLightingShaderProperty / BSEffectShaderProperty are its shader blocks |
| HKX | Havok packfile: skeletons, animations, behaviours |
| BSA / BA2 | Bethesda archives; v104 (LE, zlib) and v105 (SE, LZ4) |
| vpath | a normalized game path (`meshes/clutter/apple01.nif`) |
| Pack | the converter's output directory |
| Settings fingerprint | the string hashed into asset names so changed options rename assets |
| MO2 | Mod Organizer 2: mods live in folders, merged by a virtual filesystem |
