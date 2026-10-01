#!/usr/bin/env python3
"""Source textures for the headless-bake spike.

Sizes follow SE's mix: mostly 1024 and 2048, 512 for clutter, a few 4096.
Content is noise, the worst case for a BC7 encoder.
"""
import os, sys, random
from PIL import Image

SIZES = [(512, 20), (1024, 45), (2048, 30), (4096, 5)]  # (px, weight %)

def pick(rnd):
    r, acc = rnd.randrange(100), 0
    for px, w in SIZES:
        acc += w
        if r < acc:
            return px
    return 1024

if __name__ == "__main__":
    outdir, count = sys.argv[1], int(sys.argv[2])
    os.makedirs(outdir, exist_ok=True)
    rnd = random.Random(99)
    total_px, hist = 0, {}
    for i in range(count):
        px = pick(rnd)
        total_px += px * px
        hist[px] = hist.get(px, 0) + 1
        img = Image.frombytes("RGB", (px, px), os.urandom(px * px * 3))
        img.save(os.path.join(outdir, f"tex_{i:05d}.png"), compress_level=1)
    print(f"wrote {count} png, {total_px/1e6:.1f} Mpx, mix={dict(sorted(hist.items()))}")
