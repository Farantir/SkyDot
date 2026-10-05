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

## Licence

GPL-3.0-or-later (see `LICENSE`). The converter links nifly, which is GPL-3.
AGPL code is never linked.
