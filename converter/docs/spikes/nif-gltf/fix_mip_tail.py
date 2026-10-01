#!/usr/bin/env python3
"""Complete a DDS mip chain down to 1x1, in place.

Godot rejects mipmapped DDS files whose chain stops before 1x1; almost all
vanilla SE textures stop at 2x2. Missing levels are filled by repeating the last
stored level.

Prototype only: it corrupts cubemaps. Use `bethconv texture` instead.
"""
import struct
import sys
import pathlib

BLOCK_BPB = {b'DXT1': 8, b'DXT2': 16, b'DXT3': 16, b'DXT4': 16, b'DXT5': 16,
             b'ATI1': 8, b'BC4U': 8, b'BC4S': 8, b'ATI2': 16, b'BC5U': 16, b'BC5S': 16}

DDPF_FOURCC = 0x4
DDPF_RGB = 0x40
DDPF_LUMINANCE = 0x20000
DDPF_ALPHA = 0x2


def level_size(w, h, block, unit):
    if block:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * unit
    return w * h * unit


def fix(path):
    b = bytearray(path.read_bytes())
    if bytes(b[:4]) != b'DDS ' or len(b) < 128:
        return 'not-dds'
    h, w, _pitch, _depth, mips = struct.unpack_from('<IIIII', b, 12)
    pf_flags, = struct.unpack_from('<I', b, 80)
    fourcc = bytes(b[84:88])
    rgb_bits, = struct.unpack_from('<I', b, 88)

    if pf_flags & DDPF_FOURCC:
        if fourcc == b'DX10':
            return 'dx10-unhandled'
        unit = BLOCK_BPB.get(fourcc)
        if unit is None:
            return 'unknown-fourcc'
        block = True
    elif pf_flags & (DDPF_RGB | DDPF_LUMINANCE | DDPF_ALPHA):
        if rgb_bits % 8:
            return 'sub-byte-pixel'
        unit = rgb_bits // 8
        block = False
    else:
        return 'unknown-pixelformat'

    if mips <= 1:
        return 'single-level'          # no chain, so nothing to complete
    need = max(w, h).bit_length()      # floor(log2)+1, exact for powers of two
    if mips >= need:
        return 'already-complete'

    off, ww, hh = 128, w, h
    for _ in range(mips - 1):
        off += level_size(ww, hh, block, unit)
        ww, hh = max(1, ww // 2), max(1, hh // 2)
    last = bytes(b[off:off + level_size(ww, hh, block, unit)])
    if not last:
        return 'truncated'
    seed = last if block else last[:unit]

    tail = b''
    for _ in range(mips, need):
        ww, hh = max(1, ww // 2), max(1, hh // 2)
        n = level_size(ww, hh, block, unit)
        tail += (seed * (n // len(seed) + 1))[:n]
    struct.pack_into('<I', b, 28, need)          # dwMipMapCount
    path.write_bytes(bytes(b) + tail)
    return 'fixed'


if __name__ == '__main__':
    counts = {}
    for p in sorted(pathlib.Path(sys.argv[1]).rglob('*.dds')):
        r = fix(p)
        counts[r] = counts.get(r, 0) + 1
    for k, v in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f'{v:6} {k}')
