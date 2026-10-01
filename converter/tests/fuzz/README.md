# Fuzz targets

One target per reader of untrusted bytes:

| Target | Entry point |
| --- | --- |
| `fuzz_esm` | `record::Plugin::open` and the GRUP/record/field walk |
| `fuzz_forms` | the per-type field definitions, via `FormCensus` |
| `fuzz_bsa` | `archive::ArchiveSet::mount_archive` + `read` of every entry |
| `fuzz_nif` | `mesh::read_nif`, then `mesh::write_glb` on the result |
| `fuzz_dds` | `texture::parse_dds` + `texture::complete_mip_tail` |
| `fuzz_strings` | `record::StringTable::parse`, all three framings |
| `fuzz_snapshot` | `pack::Snapshot` open and queries |
| `fuzz_pex` | `script::parse_pex`, `script::read_pex_script`, and the script asset round trip |
| `fuzz_lod` | `pack::read_lod_source` as `.lod`, `.lst` and `.btt`, and the LOD asset round trip |

## What is checked

Not just "no crash". Each target checks the contract:

- entry points return instead of throwing;
- failure is a `ParseError`; success is self-consistent (a mip chain fits the
  file, a GLB reads back, an index count matches its directory);
- ASan/UBSan catch out-of-bounds reads.

That is why `fuzz_nif` writes the GLB and `fuzz_dds` re-parses its output: most
bugs found so far produced wrong output, not crashes.

Checks use `BETHCONV_FUZZ_CHECK`, not `assert`, because `linux-fuzz` is a
release build where `assert` compiles away.

## Building and running

libFuzzer needs clang:

```sh
cmake --preset linux-fuzz          # clang, -fsanitize=fuzzer,address,undefined
cmake --build --preset linux-fuzz
./build/linux-fuzz/tests/fuzz/bethconv-fuzz-dds -max_total_time=3600 corpus/dds
```

## Without clang

Each target also builds as a replay binary with any compiler; it feeds the given
files or directories to the same entry point:

```sh
./build/linux-debug-asan/tests/fuzz/bethconv-fuzz-dds-replay seeds/dds/*
```

It finds nothing new, but keeps the targets compiling and running in `ctest`.

## Findings

The first ~600,000 inputs found five problems:

| Target | Defect | Fix |
| --- | --- | --- |
| `fuzz_bsa` | rsm-bsa 4.1.0's LZ4 loop never checks progress; a truncated frame hangs. 321-byte reproducer. | `ArchiveSet::read` uses `io::lz4_decompress_exact`. |
| `fuzz_esm` | `parse_plugin_header` read the error of an `expected` that held a value when HEDR was short. | Check each read. |
| `fuzz_nif` | Unbounded recursion in the scene-graph walk; child indices can point back up. 291-byte reproducer. | `k_max_node_depth` plus a cycle check. |
| `fuzz_nif` | fastgltf 0.9.0's `URI::decodePercents` reads past a trailing `%`. | Percent-encode before building the URI. |
| — | Two target assertions were wrong: records that fail to inflate are counted but skipped, and the texture pass drops trailing junk. | Both corrected. |

## Seeds

`bethconv-fuzz-seeds <dir>` writes a starting corpus with the synthetic builders
from `tests/support/`, mixing valid and deliberately malformed files. No game
data, so seeds are generated, not committed. The `fuzz-replay` ctest entry
generates them and replays them through every target.
