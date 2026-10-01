# skydot

A Godot 4 engine for packs produced by [bethconv](../converter). The converter
turns a Bethesda game install into open formats (glTF meshes, DDS textures, a
memory-mappable record snapshot) on your machine; this engine reads only that.

## What this is not

- **Not a way to play Skyrim**, and not soon. The first goal is walking
  through one city in VR: no NPCs, combat, quests or scripts.
- **No SKSE plugins.** They are native DLLs hooking the original executable.
- **No behavior or animation mods.** Havok behavior graphs are not
  interpreted; Nemesis, Pandora, DAR/OAR and MCO have nothing to run on.
- **No Bethesda assets are redistributed.** You convert your own install.
  There are no prebuilt packs, and this repository contains no converter
  output.

## Status

The engine shows interior cells and streams exterior ones. What exists:

- **The GDExtension** (`extension/`): godot-cpp, CMake presets for Linux and
  Windows (debug and release), hot reload in debug. The library builds into
  `game/bin/`.
- **`SkydotPack`**: mounts a bethconv pack. Reads `manifest.json`, the
  `records.fb` header and `vpath.idx` per
  [`formats/pack-format.md`](../formats/pack-format.md) v4, refuses unknown
  versions with both numbers in the message, resolves virtual paths in any
  spelling to files on disk, and mounts a baked `.pck` over `res://`.
- **`SkydotWorld`**: reads the pack's `world.fb` (cells, references, base
  objects, lights) and builds a cell: baked scenes placed at their references
  (see `docs/coordinates.md`), `OmniLight3D`/`SpotLight3D` for lights, editor
  markers and initially disabled references skipped.
- **Exteriors**: terrain, water, grid streaming, sky and light from the
  climate's weather, and distant LOD beyond the loaded cells (terrain,
  objects, tree billboards; `SkydotLod`, see [`docs/lod.md`](docs/lod.md)).
- **Materials** (`SkydotMaterials`): Skyrim-style lighting, effect and
  refraction shaders; billboards.
- **Effects** (`SkydotAnimator`, `SkydotParticles`, `SkydotFlicker`, see
  [`docs/effects.md`](docs/effects.md)): controllers and sequences (flames,
  glows, UV scrolling, doors, waterwheels), particle systems, flickering
  lights.
- **Activation**: `pick_ref` finds the usable reference along a ray (doors,
  activators, containers, furniture, flora, items, anything scripted);
  `get_ref_info` gives its lock, load door, linked refs, activate parents and
  scripts with their property values; `get_door` gives a load door's
  destination and arrival. The viewer walks through load doors between
  interiors and worldspaces, opens and closes plain doors, and keeps locked
  doors shut.
- **Papyrus** (`SkydotPapyrus`, see [`docs/papyrus.md`](docs/papyrus.md)): a
  clean-room VM running the pack's decoded scripts. All 36 opcodes, states,
  properties, arrays, latent waits and animation waits on a cooperative
  scheduler, update timers, events, trigger volumes, and saves of the whole
  script state. About 45 natives (enable state, doors, locks, activation,
  animations, positions, random numbers, timers, messages). In the viewer,
  scripts attach as cells load, run on activation and triggers, and F5/F9
  save and load.
- **Quests**: start-game-enabled quests start with the viewer; aliases fill
  from forced references, unique actors and other quests' aliases and carry
  their scripts; stages run their fragments; objectives, journal text and
  globals are tracked and saved. Conditions are not evaluated yet.
- **Cell viewer** (`game/viewer/`): flatscreen fly camera, or four screenshots
  and exit.
- **Smoke tests** (headless editor runs): the extension loads; each refusal
  path is triggered with files the test writes; the converter's test pack is
  mounted and queried; a bake of it is mounted and its four scenes load with
  materials; synthetic effect extras animate, emit and flicker; its doors,
  lock and lever are queried and picked; the viewer walks through its door
  pair; its scripts run in the VM, and the viewer's lever toggles the cube;
  its quest starts, fills its alias with the lever and moves on when the lever
  is pulled.

Not yet: NPCs, most Papyrus natives (actors, dialogue),
conditions, the VR shell.

### Viewing a cell

```sh
# in ../converter: which models does the cell use, and bake only those
B=./build/linux-release/tools/bethconv-cli/bethconv
$B cell pack/ --list --filter breezehome
$B cell pack/ WhiterunBreezehome --models > models.txt
./tools/bake/bethconv-bake.sh --pack pack/ --from models.txt --out breezehome.pck \
    --bethconv $B --godot godot4.7

# here
godot4.7 --path game res://viewer/cell_viewer.tscn -- \
    --pack ../converter/pack --pck ../converter/breezehome.pck --cell WhiterunBreezehome
```

An exterior region, baked the same way and streamed around the camera:

```sh
# in ../converter: Riverwood is around cell (4, -12) of Tamriel
$B cell pack/ --worlds
$B cell pack/ --world Tamriel --grid 4,-12 --radius 2 --models > riverwood.txt
$B cell pack/ --world Tamriel --lod >> riverwood.txt     # distant LOD (about 4,100 meshes)
./tools/bake/bethconv-bake.sh --pack pack/ --from riverwood.txt --out riverwood.pck \
    --bethconv $B --godot godot4.7

# here
godot4.7 --path game res://viewer/cell_viewer.tscn -- \
    --pack ../converter/pack --pck ../converter/riverwood.pck --world Tamriel \
    --at 18400,-47900,300 --target 19400,-46500,0
```

Cells build in steps within `--build-budget` microseconds per frame (8000)
and show once complete. `--time 19` and `--weather SkyrimClear` choose the sky; by default it is noon
under the climate's most likely weather. `--benchmark 10` flies east and
prints frame times (pass `--disable-vsync` to Godot when the window is not
visible, or the compositor throttles it).

The mouse looks (Esc releases it, a click captures it again), WASD/Q/E move,
Shift is faster. F activates what the camera looks at (within 2.6 m); Shift+F
or `--pick-locks on` ignores locks. F5 saves the scripts' state and the place,
F9 loads it. J shows the journal; quest stages and objectives show at the top
left. P shows the camera's position and facing in the game's terms, and
`--look Z,X` takes the same angles as `player.getangle z` and `x`.
`--set-stage MQ101:10` sets a quest stage at start, `--quests off` keeps
start-game-enabled quests from starting.
`--activate 0xREF,0xREF` activates references in turn, each in the place the
previous one led to, and exits; it runs headless. `--save-to FILE` saves at
the end of such a run, `--load FILE` loads at the start. Add
`--screenshot out.png` to render four views and exit.

## Build and run

Needs CMake ≥ 3.28, Ninja, a C++20 compiler, Python 3 (for godot-cpp's binding
generator) and a Godot 4.7 editor on `PATH` as `godot4.7`, `godot4` or `godot`
(or `$GODOT`).

```sh
git submodule update --init
cmake --preset linux-debug
cmake --build --preset linux-debug     # -> game/bin/libskydot.linux.template_debug.x86_64.so
ctest --preset linux-debug             # 8 tests, 15 with a baked test pack
../tools/ci/check-no-game-data.sh
```

Presets: `linux-debug`, `linux-release`, `windows-debug`, `windows-release`.
Debug presets build godot-cpp's `template_debug` (what the editor loads);
release presets build `template_release` (only exported games load it), so
`ctest` under a release preset still uses the debug library.

**Hot reload:** `game/skydot.gdextension` sets `reloadable = true` and the
debug presets enable `GODOTCPP_USE_HOT_RELOAD`, so rebuilding with the editor
open on `game/` reloads the library. Only tested headlessly so far.

### Converter output for the tests

Two tests need converter output and show as *Not Run* without it. Build
bethconv's test pack (no Bethesda data) and optionally bake it:

```sh
# in ../converter, after building it
./build/linux-debug-asan/tools/testpack/bethconv-testpack /tmp/tp
./tools/bake/bethconv-bake.sh --pack /tmp/tp/pack --out /tmp/tp/testpack.pck \
    --bethconv ./build/linux-debug-asan/tools/bethconv-cli/bethconv --godot godot4.7

# here
cmake --preset linux-debug -DSKYDOT_TESTPACK=/tmp/tp/pack -DSKYDOT_TESTPCK=/tmp/tp/testpack.pck
ctest --preset linux-debug
```

With both repositories side by side, `SKYDOT_TESTPACK` defaults to the pack
bethconv's `ctest -R testpack` leaves in its build tree.
`SKYDOT_TESTPACK_REQUIRE=1` turns skips into failures.

### From GDScript

```gdscript
var pack := SkydotPack.new()
if pack.open("/path/to/pack") != OK:
    push_error(pack.get_error())      # e.g. "pack format version 2 is not one this engine reads (it reads v1)"
    return
pack.mount_baked("/path/to/bake.pck")

var vpath := SkydotPack.model_vpath("Clutter\\Apple01.nif")   # "meshes/clutter/apple01.nif"
var on_disk := pack.resolve(vpath)                            # ".../assets/3f/3f9c….glb", or ""
var scene := load(SkydotPack.scene_path_for(vpath))           # "res://meshes/clutter/apple01.scn"
```

## Layout

```
extension/src/            the GDExtension, C++20
  register_types.cpp      entry point and class registration
  assets/pack.*           SkydotPack
  world/world.*           SkydotWorld
game/                     the Godot project (.gdextension, project.godot, glue)
  viewer/                 cell viewer
tests/smoke/              headless editor runs, registered with ctest
tools/ci/                 repository checks
docs/godot-notes.md       observations about the editor and bindings
extension/schema/*.fbs   copies of bethconv's schemas (ctests check they match)
extern/godot-cpp          submodule, master, bindings for 4.7
extern/flatbuffers        submodule, v25.12.19 (headers and flatc)
```

## Licence

GPL-3.0-or-later. Dependencies are listed in `THIRD_PARTY.md`.
