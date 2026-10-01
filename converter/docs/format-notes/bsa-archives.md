# BSA archives — observations

From `archive/ArchiveSet` over `rsm-bsa`, run on the LE, SE and VR installs and
a 71-archive modded load order.

## The version selects the codec

v104 compresses blocks with zlib, v105 with LZ4. `rsm-bsa`'s
`file::decompress()` takes the version as a parameter instead of reading it
from the file, so `SourceInfo::version` must be kept and passed back. Losing it
corrupts data for exactly one of the two games.

LE ships v104 throughout; SE, VR and the modded set ship v105.

## We decompress blocks ourselves

`ArchiveSet::read()` reads the raw block and uses `io::inflate_exact` (v104) or
`io::lz4_decompress_exact` (v105) instead of
`bsa::tes4::file::decompress()`.

`rsm-bsa` 4.1.0's `file::decompress_into_lz4` never returns on a truncated
frame:

```cpp
do {
    inptr += insz;  insz  = in.end()  - inptr;
    outptr += outsz; outsz = a_out.end() - outptr;
    result = LZ4F_decompress(dctx, outptr, &outsz, inptr, &insz, &opts);
} while (result != 0 && !LZ4F_isError(result));
```

With both buffers exhausted, `LZ4F_decompress` returns a nonzero hint without
consuming or producing anything, and the loop repeats with the same arguments.
The fuzzer found it on a 321-byte archive whose entries declare plausible sizes,
so a size cap would not help. An exception can be caught; a hang cannot.

Our loop stops on exhaustion and on lack of progress; either check alone
suffices. It also recovers the data in that reproducer, since only the frame's
end mark was damaged.

Checked byte-for-byte against 33,685 LZ4 entries (SE meshes) and 132,907 zlib
entries (LE), with all pinned corpus hashes unchanged. The LZ4 pass got faster
(8.5 s → 7.0 s) because the file object is no longer copied.

## Vanilla LE ships three corrupt entries

`scan --read-all` over LE reads 132,907 entries and fails on three:

```
meshes/architecture/markarth/exterior/tower/markarthtemp.txt
meshes/architecture/markarth/exterior/castle/markarthtemp.txt
meshes/architecture/markarth/exterior/temple/markarthtemp.txt
```

All in `Skyrim - Meshes.bsa`, all failing with a zlib `data error`. They are
broken as shipped. The corpus harness pins them as expected failures, so a
fourth failure or a sudden success is both caught.

## Format is detected from content

`mount_archive` uses `bsa::guess_file_format`, not the extension; mods ship BA2
data named `.bsa` and vice versa. TES3 archives are rejected with a specific
message.

## Precedence

1. Higher mount `priority` wins.
2. At equal priority, a loose file beats an archived one (as in the game).
3. Otherwise the later mount wins.

Providers are kept sorted best-first per path at insert time, so `resolve()` is
one hash lookup and `conflicts()` is free.

## Path normalization is ASCII-only

Bethesda's hashing is ASCII. A locale-aware lowercase would make lookups depend
on the user's locale (Turkish dotless i).
