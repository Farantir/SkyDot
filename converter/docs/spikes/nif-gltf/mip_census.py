#!/usr/bin/env python3
"""Count DDS mip chains under a directory: complete, short or single-level.
"""
import struct
import sys
import pathlib
import collections

DDPF_FOURCC = 0x4

rows = collections.Counter()
short_by = collections.Counter()
for p in pathlib.Path(sys.argv[1]).rglob('*.dds'):
    b = p.read_bytes()
    if b[:4] != b'DDS ' or len(b) < 128:
        continue
    h, w, _pitch, _depth, mips = struct.unpack_from('<IIIII', b, 12)
    pf_flags, = struct.unpack_from('<I', b, 80)
    rgb_bits, = struct.unpack_from('<I', b, 88)
    _caps1, caps2 = struct.unpack_from('<II', b, 108)
    kind = (b[84:88].decode('latin1') if pf_flags & DDPF_FOURCC
            else f'uncompressed{rgb_bits}')
    if caps2 & 0x200:
        kind += '+cubemap'
    need = max(w, h).bit_length()          # floor(log2)+1, exact for powers of two
    state = ('single-level' if mips <= 1
             else 'complete' if mips >= need
             else 'short')
    rows[(kind, state)] += 1
    if state == 'short':
        short_by[need - mips] += 1

total = sum(rows.values())
print(f'{total} dds')
for (kind, state), n in sorted(rows.items(), key=lambda kv: -kv[1]):
    print(f'{n:7}  {kind:18} {state}')
print('levels short:', dict(sorted(short_by.items())))
