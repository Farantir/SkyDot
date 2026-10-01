#!/usr/bin/env python3
"""Rewrap a block-compressed DDS as KTX2 without transcoding.

The BCn payload is copied as is; only the container changes. Used to test
whether Godot keeps the block format of such a KTX2.

Spec: https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html
"""
import struct, sys, os

# DDS FourCC / DXGI -> (vkFormat, KDF colorModel, block bytes)
VK_BC1_SRGB, VK_BC1_UNORM = 132, 131
VK_BC2_UNORM, VK_BC3_UNORM = 135, 137
VK_BC4_UNORM, VK_BC5_UNORM = 139, 141
VK_BC7_UNORM, VK_BC7_SRGB = 145, 146
KDF = {VK_BC1_UNORM:128, VK_BC1_SRGB:128, VK_BC2_UNORM:129, VK_BC3_UNORM:130,
       VK_BC4_UNORM:131, VK_BC5_UNORM:132, VK_BC7_UNORM:134, VK_BC7_SRGB:134}
BLOCK_BYTES = {VK_BC1_UNORM:8, VK_BC1_SRGB:8, VK_BC2_UNORM:16, VK_BC3_UNORM:16,
               VK_BC4_UNORM:8, VK_BC5_UNORM:16, VK_BC7_UNORM:16, VK_BC7_SRGB:16}

def parse_dds(data):
    if data[:4] != b'DDS ': raise ValueError('not DDS')
    height, width = struct.unpack_from('<II', data, 12)
    mipcount = struct.unpack_from('<I', data, 28)[0] or 1
    pf_flags = struct.unpack_from('<I', data, 80)[0]
    fourcc = data[84:88]
    off = 128
    if pf_flags & 0x4 and fourcc == b'DX10':
        dxgi = struct.unpack_from('<I', data, 128)[0]
        off = 148
        vk = {71:VK_BC1_UNORM,72:VK_BC1_SRGB,74:VK_BC2_UNORM,77:VK_BC3_UNORM,
              80:VK_BC4_UNORM,83:VK_BC5_UNORM,98:VK_BC7_UNORM,99:VK_BC7_SRGB}.get(dxgi)
        if vk is None: raise ValueError(f'unsupported DXGI {dxgi}')
    else:
        vk = {b'DXT1':VK_BC1_UNORM, b'DXT3':VK_BC2_UNORM, b'DXT5':VK_BC3_UNORM,
              b'BC4U':VK_BC4_UNORM, b'ATI1':VK_BC4_UNORM,
              b'BC5U':VK_BC5_UNORM, b'ATI2':VK_BC5_UNORM}.get(fourcc)
        if vk is None: raise ValueError(f'unsupported FourCC {fourcc!r}')
    return width, height, mipcount, vk, data[off:]

def basic_dfd(vk):
    """Minimal KHR Data Format Descriptor for a BCn block format."""
    bb = BLOCK_BYTES[vk]
    samples = 1
    block_size = 24 + 16 * samples          # basic block: 24B header + samples
    dfd = struct.pack('<I', 4 + block_size) # dfdTotalSize
    dfd += struct.pack('<II', 0, (2 << 16) | block_size)  # vendor/type, ver/size
    srgb = 2 if vk in (VK_BC1_SRGB, VK_BC7_SRGB) else 1   # transferFunction
    dfd += bytes([KDF[vk], 1, srgb, 0])                   # model, primaries, xfer, flags
    dfd += bytes([3, 3, 0, 0])                            # texelBlockDimension (4x4)
    dfd += bytes([bb, 0, 0, 0, 0, 0, 0, 0])               # bytesPlane0..7
    # One 16-byte sample: bitOffset u16, bitLength-1 u8, channelType u8,
    # samplePosition[4] u8, sampleLower u32, sampleUpper u32.
    dfd += struct.pack('<HBB', 0, (bb * 8 - 1) & 0xFF, 0)
    dfd += bytes([0, 0, 0, 0])
    dfd += struct.pack('<II', 0, 0xFFFFFFFF)
    return dfd

def convert(src, dst):
    w, h, mips, vk, payload = parse_dds(open(src, 'rb').read())
    bb = BLOCK_BYTES[vk]
    levels, off = [], 0
    for i in range(mips):
        lw, lh = max(1, w >> i), max(1, h >> i)
        size = max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * bb
        if off + size > len(payload):
            mips = i
            break
        levels.append((off, size)); off += size
    if not levels: raise ValueError('no level data')

    dfd = basic_dfd(vk)
    header = struct.pack('<12s', bytes([0xAB,0x4B,0x54,0x58,0x20,0x32,0x30,0xBB,0x0D,0x0A,0x1A,0x0A]))
    header += struct.pack('<9I', vk, 1, w, h, 0, 0, 1, len(levels), 0)
    index_size = 4*4 + 8*2
    level_index_size = len(levels) * 24
    dfd_off = 12 + 36 + index_size + level_index_size
    kvd_off = dfd_off + len(dfd)
    # KTX2 stores level data smallest-first, and each level must begin at a
    # file offset that is a multiple of lcm(texel block size, 4).
    align = bb if bb % 4 == 0 else 4
    data_start = kvd_off
    if data_start % align:
        data_start += align - (data_start % align)
    out_levels, blob = [], b''
    for i in reversed(range(len(levels))):
        o, s = levels[i]
        pad = (-(data_start + len(blob))) % align
        blob += b'\x00' * pad
        out_levels.append((data_start + len(blob), s))
        blob += payload[o:o+s]
    out_levels.reverse()

    header += struct.pack('<IIII', dfd_off, len(dfd), 0, 0)
    header += struct.pack('<QQ', 0, 0)
    li = b''
    for (o, s) in out_levels:
        li += struct.pack('<QQQ', o, s, s)
    pre = header + li + dfd
    pad = data_start - len(pre)
    with open(dst, 'wb') as f:
        f.write(pre + b'\x00' * pad + blob)
    return w, h, len(levels), vk

if __name__ == '__main__':
    w,h,m,vk = convert(sys.argv[1], sys.argv[2])
    print(f"{os.path.basename(sys.argv[1])} -> {w}x{h} mips={m} vkFormat={vk}")
