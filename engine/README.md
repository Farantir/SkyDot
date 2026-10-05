# skydot

A Godot 4 engine for packs produced by [bethconv](../converter). The converter
turns a Bethesda game install into open formats (glTF meshes, DDS textures, a
memory-mappable record snapshot) on your machine; this engine reads only that.

## What this is not

- **Not a way to play Skyrim**, and not soon. The first goal is walking
  through one city in VR, and the VR shell does not exist yet: the viewer is a
  flatscreen window. Actors go about their AI packages and quest scripts run,
  but there is no combat, dialogue or inventory, most actor natives are
  missing, and quests do not evaluate conditions.
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
  `records.fb` header, `vpath.idx` and the asset store (one memory-mapped
  blob, or loose files) per
  [`formats/pack-format.md`](../formats/pack-format.md) v6, and refuses
  unknown versions with both numbers in the message.
- **Assets load straight from the pack**, no import or bake: meshes through
  Godot's runtime glTF loader, DDS textures as they are (block-compressed,
  cube maps too), on worker threads, cached by virtual path
  (`assets/asset_cache.hpp`).
- **`SkydotWorld`**: reads the pack's `world.fb` (cells, references, base
  objects, lights) and builds a cell: models placed at their references
  (see `docs/coordinates.md`), `OmniLight3D`/`SpotLight3D` for lights, editor
  markers and initially disabled references skipped.
- **Exteriors**: terrain, water, grid streaming, weather (`SkydotWeather`,
  see [`docs/weather.md`](docs/weather.md): time of day, region and climate
  weathers fading into each other, clouds, sun, moons, stars, rain, snow,
  lightning), and distant LOD beyond the loaded cells (terrain,
  objects, tree billboards; `SkydotLod`, see [`docs/lod.md`](docs/lod.md)).
- **Materials** (`SkydotMaterials`): Skyrim-style lighting, effect and
  refraction shaders (all shaders are text files in `game/shaders/`, see its
  README); billboards.
- **Effects** (`SkydotAnimator`, `SkydotParticles`, `SkydotFlicker`, see
  [`docs/effects.md`](docs/effects.md)): controllers and sequences (flames,
  glows, UV scrolling, doors, waterwheels), particle systems, flickering
  lights.
- **Navigation** (see [`docs/navigation.md`](docs/navigation.md)): every
  cell's navmeshes become navigation regions, joined across cell borders, so
  Godot's navigation map paths over Skyrim's own navmeshes.
- **Activation**: `pick_ref` finds the usable reference along a ray (doors,
  activators, containers, furniture, flora, items, anything scripted);
  `get_ref_info` gives its lock, load door, linked refs, activate parents and
  scripts with their property values; `get_door` gives a load door's
  destination and arrival. The viewer walks through load doors between
  interiors and worldspaces, opens and closes plain doors, and keeps locked
  doors shut. Near a load door it builds the place behind it ahead
  (`begin_cell`/`begin_exterior` with `continue_build_static`, resources by
  `request_cell`/`request_exterior`), so going through takes a fraction of
  the time; actors are added on arrival.
- **Actors** (see [`docs/actors.md`](docs/actors.md)): placed NPCs and
  creatures stand dressed in their outfits (skeleton, body parts, FaceGen
  head, skin tint) and walk the navmesh at the speed of their walk and run
  clips, opening plain doors on the way. Clips are chosen by name; Havok
  behaviour graphs are not interpreted.
- **AI packages** (`SkydotAi`, see [`docs/ai.md`](docs/ai.md)): an actor's
  schedule and conditions pick its package, and its procedures run against a
  game clock: actors walk to work, home, the inn and to bed, through load
  doors, sandbox, patrol and travel. They only walk up to a bed or chair, and
  following, escorting and fighting are not done: the actor stands.
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
  globals are tracked and saved. Quest conditions (alias fills, log entries)
  are not evaluated yet; AI packages answer about two dozen condition
  functions.
- **Cell viewer** (`game/viewer/`): flatscreen fly camera, or four screenshots
  and exit.
- **Unit tests** (`tests/unit/`, Catch2): the Papyrus VM and its saves, AI
  packages, actor plans and virtual paths, native and in milliseconds. Their
  inputs (scripts, `world.fb`) are built in memory.
- **Smoke tests** (headless editor runs): the extension loads; each refusal
  path is triggered with files the test writes; the converter's test pack is
  mounted and queried; its four meshes and its textures (a cube map among
  them) load from the blob, and its cells build with Skyrim materials; synthetic effect extras animate, emit and flicker; its doors,
  lock and lever are queried and picked; the viewer walks through its door
  pair; its scripts run in the VM, and the viewer's lever toggles the cube;
  its quest starts, fills its alias with the lever and moves on when the lever
  is pulled.

Not yet: combat, dialogue and scenes, inventories, weapons, furniture use
(actors stand next to beds and chairs), most Papyrus natives (actors, form
lists, inventories), quest conditions, behaviour graphs, the VR shell.

### The pack tool

The project's main scene converts installs and opens packs, for anyone
without a terminal:

```sh
godot4.7 --path game          # or run an exported build
```

- **Convert:** picks a detected install (Steam libraries, the game's
  `plugins.txt`), optionally a Mod Organizer 2 instance and profile, and an
  output folder that `bethconv target` checks while it is typed (slow or
  FUSE disks are flagged before anything starts). Progress, the log and the
  result come from `bethconv convert --json`.
- **Packs:** every pack the tool wrote or opened and those in the packs
  folder, with size, stale blob bytes and failures; update one with its
  recorded inputs, delete it, or open it in the viewer at Riverwood, in any
  worldspace or in an interior.

It runs the converter as a separate process (`game/packtool/bethconv.gd`):
next to the executable, else a build in `../converter/build/`, or the binary
chosen under Settings. The JSON it reads is described in
`../converter/docs/cli-json.md`.

### Viewing a cell

Convert once (see `../converter/README.md`), onto a local SSD; then any cell
or worldspace opens directly:

```sh
# in ../converter
B=./build/linux-release/tools/bethconv-cli/bethconv
$B convert --data "<Skyrim Special Edition>/Data" -o ~/packs/se   # about a minute
$B cell ~/packs/se --list --filter breezehome

# here
godot4.7 --path game res://viewer/cell_viewer.tscn -- \
    --pack ~/packs/se --cell WhiterunBreezehome
```

An exterior, streamed around the camera with distant LOD beyond it:

```sh
godot4.7 --path game res://viewer/cell_viewer.tscn -- \
    --pack ~/packs/se --world Tamriel \
    --at 18400,-47900,300 --target 19400,-46500,0
```

Cells build in steps within `--build-budget` microseconds per frame (8000)
and show once complete. Within `--preload-distance` metres (15) of a load
door, the place behind it (an interior, or the cells and LOD around the
arrival outside) is built ahead while nothing else streams: in the scene but
hidden, without physics and off the navigation map, and switched on when the
player arrives (outside, cell by cell as they finish). It costs the memory
of a second place; `--preload-doors off` or O turns it off (`--screenshot`
and `--benchmark` do not preload). `game/tools/preload_check.gd` times going
through the nearest door. Going through one fades to black, travels once the
screen is black, and fades back when the new place is built (outside: the
cells in range and the LOD), a few frames later so the first slow frame of
a new place is not seen; the player is held meanwhile. Time starts at `--time` (default 12) and runs at
`--time-scale` (default 20); `--weather SkyrimClear` keeps one weather,
otherwise the region's or climate's weathers take turns. T and Shift+T move
the time by an hour, K changes the weather. `--benchmark 10` flies east and
prints frame times (pass `--disable-vsync` to Godot when the window is not
visible, or the compositor throttles it).

The mouse looks (Esc releases it, a click captures it again). The keys are
InputMap actions (`game/project.godot`, read by `viewer/player_input.gd`), so a
gamepad or VR shell can map its own events to them. You walk with
collision: WASD moves, Shift sprints, Ctrl walks, Space jumps (and swims up in
water); V toggles flying through everything (Q/E down/up, Shift faster).
`--walk off` starts flying, `--collision off` builds no physics bodies;
`--benchmark` and `--screenshot` always fly. Without `--at` an interior is
entered at its door from outside. See `docs/physics.md`. F activates what the camera looks at (within 2.6 m); Shift+F
or `--pick-locks on` ignores locks. F5 saves the scripts' state and the place,
F9 loads it. J shows the journal; quest stages and objectives show at the top
left. N shows the navmeshes, G draws a path from the feet to where the
camera looks (`--navigation off` builds none; see `docs/navigation.md`).
P shows the camera's position and facing in the game's terms, and
`--look Z,X` takes the same angles as `player.getangle z` and `x`.
`--set-stage MQ101:10` sets a quest stage at start, `--quests off` keeps
start-game-enabled quests from starting. Placed NPCs and creatures stand
dressed in their outfits and follow their AI packages, walking the navmesh
(`--ai off` keeps them where they were placed, wandering about it; `--wander
off` keeps them in their idle, `--actors off` builds none; see
`docs/actors.md` and `docs/ai.md`). `--shot-delay
SECONDS` lets the world run before a `--screenshot`.
F12 saves a screenshot with a JSON file beside it (Shift+F12 without the text
overlay; `--shot-dir`, default `user://screenshots`): place, camera in engine
and game terms, time, weather, viewer options, pack hashes, GPU, and the game
console commands for the same spot. The viewer then pauses and asks what is
wrong; the answer is stored as the JSON's `note` (Escape skips it,
`--shot-notes off` never asks). `--from-shot shot.json` starts there with
time stopped; with `--screenshot out.png` it renders that view again and
exits (a re-render at Riverwood differed in 0.11% of pixels, from moving
foliage), which is how a shot becomes a bug report or a regression check.
`--activate 0xREF,0xREF` activates references in turn, each in the place the
previous one led to, and exits; it runs headless. `--save-to FILE` saves at
the end of such a run, `--load FILE` loads at the start. Add
`--screenshot out.png` to render four views and exit.

## Build and run

Needs CMake ≥ 3.28, Ninja, a C++20 compiler, Python 3 (for godot-cpp's binding
generator) and a Godot 4.7 editor on `PATH` as `godot4.7`, `godot4` or `godot`
(or `$GODOT`). The unit tests need no editor.

```sh
git submodule update --init
cmake --preset linux-debug
cmake --build --preset linux-debug     # -> game/bin/libskydot.linux.template_debug.x86_64.so
ctest --preset linux-debug             # the unit tests and 15 smoke tests (22 with the converter's test pack)
ctest --preset linux-debug -L unit     # only the unit tests: no editor, no test pack
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

Most tests need converter output: without it the pack tests are skipped and
the seven viewer tests are not registered. Build the converter's test pack (no
Bethesda data):

```sh
# in ../converter, after building it
./build/linux-debug-asan/tools/testpack/bethconv-testpack /tmp/tp

# here
cmake --preset linux-debug -DSKYDOT_TESTPACK=/tmp/tp/pack
ctest --preset linux-debug
```

`SKYDOT_TESTPACK` defaults to the pack the converter's `ctest -R testpack`
leaves in its build tree. `SKYDOT_TESTPACK_REQUIRE=1` turns skips into
failures.

Headless runs load assets on the calling thread: Godot's headless renderer
creates resources without locking, so the worker threads are exercised only
with a window (the viewer, `--screenshot`, `--benchmark`).

### From GDScript

```gdscript
var pack := SkydotPack.new()
if pack.open("/path/to/pack") != OK:
    push_error(pack.get_error())      # e.g. "pack format version 5 is not one this engine reads (it reads v6): <path>"
    return
var vpath := SkydotPack.model_vpath("Clutter\\Apple01.nif")   # "meshes/clutter/apple01.nif"
var model := pack.load_scene(vpath)                           # SkydotModel, or null
add_child(model.instantiate())
var texture := pack.load_texture("textures/clutter/apple01.dds")
var bytes := pack.get_bytes(vpath)                            # the GLB as stored
```

## Layout

```
extension/src/            the GDExtension, C++20
  register_types.cpp      entry point and class registration
  assets/pack.*           SkydotPack
  assets/pack_store.*     vpath.idx and asset bytes (blob or loose)
  assets/asset_cache.*    models and textures built from those bytes
  data/                   world.fb as read (WorldData), FlatBuffers helpers, coordinates, ActorPlan
  nav/                    navigation regions from navmeshes
  render/                 materials, terrain, water, grass, LOD, weather, effects
  physics/                collision bodies, the player
  actors/                 SkydotActor, animation, locomotion, where actors are
  build/                  CellBuilder: cells built from data, and the decoration of models
  world/                  SkydotWorld (a facade over data/ and build/) and its queries
  ai/                     AI packages, SkydotAi
  vm/                     the Papyrus VM and SkydotPapyrus
game/                     the Godot project (.gdextension, project.godot, glue)
  packtool/               the pack tool (main scene): convert, list, view
  viewer/                 cell viewer: cell_viewer.gd puts it together, its parts sit beside it (ARCHITECTURE.md 8.2)
  tools/                  headless checks against a real pack
tests/unit/               Catch2 tests of skydot_core, the Godot-free sources
tests/smoke/              headless editor runs, registered with ctest
tools/ci/                 repository checks
docs/godot-notes.md       observations about the editor and bindings
extern/godot-cpp          submodule, master, bindings for 4.7
extern/flatbuffers        submodule, v25.12.19 (headers and flatc)
extern/catch2             submodule, v3.16.0 (the unit tests)
```

## Licence

GPL-3.0-or-later. Dependencies are listed in `THIRD_PARTY.md`.
