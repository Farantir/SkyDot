# Third-party dependencies

Every dependency with its licence and pinned revision.

## Licence floor

GPL-3.0-or-later, like bethconv. AGPL-3 code is excluded: skymp's Papyrus VM
may be read as a `.pex` reference but never depended on.

## Submodules

| Path | Upstream | Revision | Licence | Used for |
| --- | --- | --- | --- | --- |
| `extern/godot-cpp` | https://github.com/godotengine/godot-cpp | `6cceaf6a5f8b0d78ac5d71c139fd7fabba43b918` (master, 2026-09-08) | MIT | GDExtension bindings for API 4.7 |
| `extern/flatbuffers` | https://github.com/google/flatbuffers | `7e163021e59cca4f8e1e35a7c828b5c6b7915953` (v25.12.19) | Apache-2.0 | Reading `world.fb`; `flatc` generates the reader. Same version as bethconv's vcpkg port |
| `extern/catch2` | https://github.com/catchorg/Catch2 | `317ac1ed4c0bb6e6b91eafc817e05c488feffcb3` (v3.16.0) | BSL-1.0 | The unit tests (`tests/unit/`); not part of the extension |

`master`, because the release branches stop at 4.5 and only `master` has the
4.7 API JSON (see `docs/godot-notes.md`). Update the pin together with
`GODOTCPP_API_VERSION` in `CMakeLists.txt` and `compatibility_minimum` in
`game/skydot.gdextension`.

## Not a dependency

Godot itself: the editor binary is whatever `ctest` finds on `PATH`.
