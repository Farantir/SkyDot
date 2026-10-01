# Spike: vcpkg dependency coexistence

**Question:** do `bsa`, `nifly`, `basis_universal` and `fastgltf` build together
under one vcpkg manifest?

**Budget:** 3 days · **Took:** ~1 hour · **Result:** yes, except `basis_universal`

## Criterion

All link; `nifly` builds with the same flags; no ODR or zlib version conflict.

## Findings

vcpkg baseline `127402f1c75bb3d5ff6bce04b285faa4930a5aca` (2026-07-27),
Linux/GCC 16.1.1.

| Package | Port | Version | Result |
| --- | --- | --- | --- |
| `bsa` | `rsm-bsa` | 4.1.0 | ok |
| `fastgltf` | `fastgltf` | 0.9.0 | ok (pulls `simdjson`) |
| `meshoptimizer` | `meshoptimizer` | — | ok |
| `zstd` | `zstd` | 1.5.7 | ok |
| `zlib` | `zlib` | — | ok |
| `blake3` | `blake3` | 1.8.6 | ok |
| `nlohmann-json` | `nlohmann-json` | 3.12.0 | ok |
| `cli11` | `cli11` | 2.7.2 | ok |
| `catch2` | `catch2` | 3.15.3 | ok |
| `basis_universal` | none | — | needs a submodule |

Cold resolve: 1.4 min for the record/archive set, +41 s for the mesh set. No
conflicts, one zlib.

`nifly`:

- Sets C++17 with a directory-scoped `set()`, which does not leak into our
  targets.
- Builds as C++23 in 29 s, so the whole project can use one standard.
- Its tests only build when it is the top-level project, so no Catch2 is pulled
  in.
- Ships its own dependencies in `external/`.

## Consequences

- vcpkg + CMake stays.
- `basis_universal` would have to be a submodule. It was later dropped, since
  desktop textures are passed through (see `headless-bake.md`).
- `zlib` is a core dependency: 105,468 of 1.18M vanilla records are compressed.
