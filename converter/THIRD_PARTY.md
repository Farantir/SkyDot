# Third-party dependencies

Every dependency with its licence and pinned revision.

## Licence floor

The project is GPL-3.0-or-later because `nifly` is GPL-3.

AGPL-3 code is excluded. skymp's Papyrus VM is the only embeddable `.pex`
interpreter, and linking it would make the whole project AGPL-3. It may be read
as a reference, not depended on.

## Submodules

| Dependency | Licence | Used for |
| --- | --- | --- |
| [nifly](https://github.com/ousnius/nifly) | GPL-3.0 | NIF parsing (no vcpkg port) |
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) `b943862` | MIT or public domain | BC7 and BC1 encoding of uncompressed textures, BC decoding for tests (no vcpkg port; only `bc7enc`, `rgbcx`, `bc7decomp` are built, not the Apache-2.0 `bc7e.ispc`) |

## vcpkg

Versions come from the `builtin-baseline` in `vcpkg.json`.

| Package | Version | Licence | Used for |
| --- | --- | --- | --- |
| `cli11` | 2.7.2 | BSD-3-Clause | CLI parsing |
| `nlohmann-json` | 3.12.0 | MIT | `report.json`, `manifest.json` |
| `zlib`, `lz4` | — | zlib / BSD-2-Clause | Record and archive decompression |
| `rsm-bsa` | 4.1.0 | MIT | BSA reading |
| `fastgltf` | 0.9.0 | MIT | glTF writing |
| `blake3` | 1.8.6 | CC0 / Apache-2.0 | Content hashing |
| `flatbuffers` | 25.12.19 | Apache-2.0 | `world.fb` and the script, LOD and animation assets |
| `catch2` | 3.15.3 | BSL-1.0 | Unit tests |

`flatc` from the `flatbuffers` port compiles `formats/schema/*.fbs` at build
time. The generated headers are not committed.

## Known defects

Worked around here and worth reporting upstream. All three can be triggered by a
file in a mod folder; the third by a file Bethesda ships. The fuzzers found the
first two; reading the mesh writer's output back with `pack/pack_view` found
the third.

| Dependency | Defect | Workaround |
| --- | --- | --- |
| `rsm-bsa` 4.1.0 | `file::decompress_into_lz4` calls `LZ4F_decompress` in a loop without a progress check. A frame truncated mid-block loops forever. A 321-byte archive reproduces it. | `ArchiveSet::read` uses `io::lz4_decompress_exact`. See `docs/format-notes/bsa-archives.md`. |
| `fastgltf` 0.9.0 | The exporter writes non-finite floats as out-of-range decimals without an error (NaN → `2.696539702293474e+308`, `-inf` → `-1.797693134862316e+308`; vanilla SE ships a `0xFFFFFFFF` NaN in a node transform). The result is not valid JSON. | `gltf_writer.cpp`'s `json_number` writes 0 instead; `nif_reader.cpp` warns and substitutes the identity transform. |
| `fastgltf` 0.9.0 | `URI::decodePercents` reads the two bytes after `%` without a bounds check, so a trailing truncated escape reads past the string. | `gltf_writer.cpp` percent-encodes before building a `fastgltf::URI`, so fastgltf only sees escapes we wrote. |

## Rejected

| Dependency | Licence | Reason |
| --- | --- | --- |
| [skymp Papyrus-VM](https://github.com/skyrim-multiplayer/skymp) | AGPL-3.0 | Would make the project AGPL. Reference only. |
| [Champollion](https://github.com/Orvid/Champollion) | — | A decompiler for humans; not part of conversion. |
| [OpenMW `components/esm4`](https://github.com/OpenMW/openmw/tree/master/components/esm4) | GPL-3.0 | Oblivion-oriented; the record layer was written from scratch instead. |
