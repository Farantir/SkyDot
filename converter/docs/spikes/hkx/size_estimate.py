"""What would every clip cost as glTF channels? Compared with the HKX bytes.

    python3 size_estimate.py EXTRACTED_DIR

Options measured per clip (float32 everywhere):
  dense   - translation + rotation per track per frame (what make_glb writes)
  varying - only channels that change over the clip; constant ones as one key
  spline  - the HKX's own spline-compressed data block (kept as is)
"""

import os
import sys
from concurrent.futures import ProcessPoolExecutor

import hkx


def one(path):
    try:
        h = hkx.load(open(path, "rb").read())
    except Exception:  # noqa: BLE001
        return None
    out = [0, 0, 0, 0, os.path.getsize(path)]
    for a in h.animations:
        n = a.num_frames
        out[0] += n * a.tracks * 7 * 4 + n * 4
        varying = 0
        for t in range(a.tracks):
            for k, width in ((0, 3), (1, 4), (2, 3)):
                first = a.frames[0][t][k]
                moves = any(max(abs(x - y) for x, y in zip(a.frames[f][t][k], first)) > 1e-4
                            for f in range(n))
                varying += (n if moves else 1) * width * 4
        out[1] += varying + n * 4
        out[2] += a.stats.get("data", 0)
        out[3] += 1
    return out


def main():
    root = sys.argv[1]
    files = [os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs if f.endswith(".hkx")]
    tot = [0] * 5
    with ProcessPoolExecutor() as ex:
        for r in ex.map(one, files, chunksize=16):
            if r:
                tot = [a + b for a, b in zip(tot, r)]
    mib = 1 << 20
    print(f"{tot[3]} clips: dense {tot[0] / mib:.0f} MiB, varying channels {tot[1] / mib:.0f} MiB, "
          f"spline data {tot[2] / mib:.1f} MiB (all .hkx files {tot[4] / mib:.0f} MiB)")


if __name__ == "__main__":
    main()
