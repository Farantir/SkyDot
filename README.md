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
