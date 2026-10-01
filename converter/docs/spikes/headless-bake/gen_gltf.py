#!/usr/bin/env python3
"""Generate synthetic .glb files sized like Skyrim statics.

Only the importer's load matters, not what the geometry looks like.
"""
import json, struct, math, os, sys, random

def make_grid(n, seed):
    """A subdivided, deformed plane: 2*n*n triangles."""
    rnd = random.Random(seed)
    pos, nrm, uv = [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            u, v = i / n, j / n
            h = math.sin(u * 6.0 + seed) * math.cos(v * 5.0 + seed) * 0.35
            pos += [(u - 0.5) * 100.0, h * 40.0, (v - 0.5) * 100.0]
            l = math.sqrt(h * h + 1.0)
            nrm += [-h / l, 1.0 / l, 0.0]
            uv += [u, v]
    idx = []
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            b = a + 1
            c = a + (n + 1)
            d = c + 1
            idx += [a, c, b, b, c, d]
    return pos, nrm, uv, idx

def build_glb(n, seed, texture_uri):
    pos, nrm, uv, idx = make_grid(n, seed)
    vcount = len(pos) // 3

    pos_b = struct.pack(f"<{len(pos)}f", *pos)
    nrm_b = struct.pack(f"<{len(nrm)}f", *nrm)
    uv_b  = struct.pack(f"<{len(uv)}f", *uv)
    idx_b = struct.pack(f"<{len(idx)}I", *idx)

    def pad4(b): return b + b"\x00" * ((4 - len(b) % 4) % 4)
    blob = b""
    offsets = []
    for chunk in (pos_b, nrm_b, uv_b, idx_b):
        offsets.append(len(blob))
        blob += pad4(chunk)

    minp = [min(pos[i::3]) for i in range(3)]
    maxp = [max(pos[i::3]) for i in range(3)]

    gltf = {
        "asset": {"version": "2.0", "generator": "bethconv-bake-fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": f"static_{seed:05d}"}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3, "material": 0}]}],
        "materials": [{
            "name": f"mat_{seed:05d}",
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.0, "roughnessFactor": 0.8},
        }],
        "textures": [{"source": 0}],
        "images": [{"uri": texture_uri}],
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": offsets[0], "byteLength": len(pos_b), "target": 34962},
            {"buffer": 0, "byteOffset": offsets[1], "byteLength": len(nrm_b), "target": 34962},
            {"buffer": 0, "byteOffset": offsets[2], "byteLength": len(uv_b),  "target": 34962},
            {"buffer": 0, "byteOffset": offsets[3], "byteLength": len(idx_b), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": vcount, "type": "VEC3",
             "min": minp, "max": maxp},
            {"bufferView": 1, "componentType": 5126, "count": vcount, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": vcount, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5125, "count": len(idx), "type": "SCALAR"},
        ],
    }

    js = pad4(json.dumps(gltf, separators=(",", ":")).encode())
    total = 12 + 8 + len(js) + 8 + len(blob)
    out = struct.pack("<4sII", b"glTF", 2, total)
    out += struct.pack("<II", len(js), 0x4E4F534A) + js
    out += struct.pack("<II", len(blob), 0x004E4942) + blob
    return out, len(idx) // 3

if __name__ == "__main__":
    outdir, count, texdir = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    rnd = random.Random(1234)
    tris = 0
    textures = sorted(os.listdir(texdir)) if os.path.isdir(texdir) else []
    for i in range(count):
        # Skyrim statics cluster low with a long tail; 8..40 grid => 128..3200 tris.
        n = rnd.choice([8, 10, 12, 16, 16, 20, 24, 32, 40])
        tex = textures[i % len(textures)] if textures else "missing.ktx2"
        data, t = build_glb(n, i, f"../textures/{tex}")
        tris += t
        with open(os.path.join(outdir, f"static_{i:05d}.glb"), "wb") as f:
            f.write(data)
    print(f"wrote {count} glb, {tris} triangles total, avg {tris//count}")
