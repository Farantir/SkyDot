# SkyDot

A Godot 4 engine for Skyrim's data, and the converter that prepares it.

You convert your own game install on your machine; the engine reads only the
converter's output. No Bethesda data is in this repository or distributed with
it.

| Directory | What |
| --- | --- |
| [`converter/`](converter) | `bethconv`: game install -> pack (glTF meshes, DDS textures, decoded scripts, the world's cells and objects) |
| [`engine/`](engine) | `skydot`: a GDExtension and viewer that load packs |
| [`formats/`](formats) | the pack format both sides agree on, and its FlatBuffers schemas |
| [`tools/ci/`](tools/ci) | repository checks, also run as a pre-commit hook |

Each half builds on its own (see its README). Run `tools/ci/install-hooks.sh`
once after cloning.

## How to play

SkyDot is early: 0.1.0 lets you walk around Skyrim, not play it yet. You
need your own copy of Skyrim Special Edition (Legendary Edition and VR data
convert too).

1. Download the archive for your system from the
   [releases](https://github.com/Farantir/SkyDot/releases) and unpack it
   anywhere. Linux (also the Steam Deck in desktop mode): run
   `skydot.x86_64`. Windows: run `skydot.exe`.
2. **Convert** tab: pick your Skyrim install (found automatically when Steam
   installed it), optionally a Mod Organizer 2 profile, and a folder for the
   pack. Converting the whole game takes a while and needs about 35 GB.
3. **Packs** tab: pick the pack, choose where to start (a worldspace or an
   interior) and press *Open in the viewer*.

In the viewer: the mouse looks (Esc releases it, a click captures it again),
WASD moves, Shift sprints, Ctrl walks, Space jumps, F opens doors and
activates things, V toggles flying (Q/E down/up). F5 saves, F9 loads. All keys
are listed at the top of `engine/game/viewer/cell_viewer.gd`.

What works: the world streams in with its LOD, weather and time of day; doors
and load doors; NPCs follow their daily schedules; quests and their scripts
run; saving and loading. Not yet: combat, dialogue, menus, the inventory and
the HUD.

From the command line the viewer starts directly:
`./skydot.x86_64 -- --pack DIR --world Tamriel --at X,Y,Z` or
`-- --pack DIR --cell EDITOR_ID`.

## Thanks

SkyDot stands on years of work by the Skyrim modding and open source
communities. Without their reverse engineering, documentation and tools this
project would not be possible. Thank you to everyone who built and maintains
them.

### What we learned Skyrim from

- **[UESP](https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format)**: the plugin,
  BSA, string table and compiled Papyrus (`.pex`) formats, record by record.
  Cited throughout the converter.
- **[xEdit](https://github.com/TES5Edit/TES5Edit)**: its record definitions
  (`wbDefinitionsTES5.pas`) settled field layouts, condition functions and
  enums that no other source covers.
- **[Community Shaders](https://github.com/doodlum/skyrim-community-shaders)**:
  its reconstruction of the game's HLSL (`Lighting.hlsl`, `Effect.hlsl`,
  `ISHDR.hlsl`) is how the engine's lighting, effect and image-space shaders
  match the original.
- **[NifTools](https://www.niftools.org/)**: `nif.xml` for NIF blocks,
  controllers and collision layers, and
  [NifSkope](https://github.com/niftools/nifskope) for checking meshes.
- **[Creation Kit wiki](https://ck.uesp.net/)**: Papyrus functions and their
  behaviour.
- **[OpenMW](https://openmw.org/)**: its `esm4` reader was the starting point
  for measuring how much of Skyrim's record set needed covering.

### What we build on

- [Godot](https://godotengine.org/) and
  [godot-cpp](https://github.com/godotengine/godot-cpp)
- [nifly](https://github.com/ousnius/nifly) (NIF parsing)
- [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) (BC texture encoding)
- [rsm-bsa](https://github.com/Ryan-rsm-McKenzie/bsa) (BSA archives)
- [fastgltf](https://github.com/spnda/fastgltf),
  [FlatBuffers](https://github.com/google/flatbuffers),
  [BLAKE3](https://github.com/BLAKE3-team/BLAKE3),
  [zlib](https://zlib.net/), [LZ4](https://github.com/lz4/lz4),
  [CLI11](https://github.com/CLIUtils/CLI11),
  [nlohmann/json](https://github.com/nlohmann/json) and
  [Catch2](https://github.com/catchorg/Catch2)

Licences and pinned versions are in `converter/THIRD_PARTY.md` and
`engine/THIRD_PARTY.md`.

## Licence

GPL-3.0-or-later (see `LICENSE`). The converter links nifly, which is GPL-3.
AGPL code is never linked.

The engine carries an additional permission for script mods
(`LICENSE-EXCEPTION.md`): mods made only of scripts and data that use only the
mod API may use any licence. Native mods stay under the GPL.
