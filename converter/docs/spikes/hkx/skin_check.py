"""Where do a GLB's skinned vertices land under glTF's skinning rule?

    python3 skin_check.py MODEL.glb...

For each skinned mesh node, compares the skinned bind pose (sum of
weight * jointWorld * inverseBindMatrix * v, which is all a glTF consumer
uses) with the node-placed mesh (nodeWorld * v, how a static mesh would sit).
Both are in the model's own units, below the converter's z-up root. They
should agree: a skinned mesh's bind pose is where the artist placed it.
"""

import struct
import sys

import numpy as np

import glb

TYPES = {"SCALAR": 1, "VEC3": 3, "VEC4": 4, "MAT4": 16}
COMPS = {5126: "f", 5123: "H", 5121: "B"}


def check(path):
    doc, b = glb.read(path)

    def acc(i):
        a = doc["accessors"][i]
        v = doc["bufferViews"][a["bufferView"]]
        n, fmt = TYPES[a["type"]], COMPS[a["componentType"]]
        stride = v.get("byteStride", n * struct.calcsize(fmt))
        o = v.get("byteOffset", 0) + a.get("byteOffset", 0)
        return np.array([struct.unpack_from(f"<{n}{fmt}", b, o + i * stride)
                         for i in range(a["count"])])

    def local(n):
        t = np.array(n.get("translation", [0, 0, 0]))
        x, y, z, w = n.get("rotation", [0, 0, 0, 1])
        s = n.get("scale", [1, 1, 1])
        r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                      [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                      [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        m = np.eye(4)
        m[:3, :3] = r * np.array(s)
        m[:3, 3] = t
        return m

    parent = {c: i for i, n in enumerate(doc["nodes"]) for c in n.get("children", [])}

    def world(i):  # up to, not including, the converter's z-up root
        m = local(doc["nodes"][i])
        while i in parent and not doc["nodes"][parent[i]].get("name", "").startswith("bethconv_"):
            i = parent[i]
            m = local(doc["nodes"][i]) @ m
        return m

    worst = 0.0
    for n_index, n in enumerate(doc["nodes"]):
        if "skin" not in n:
            continue
        skin = doc["skins"][n["skin"]]
        ibm = acc(skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
        joint_world = [world(j) for j in skin["joints"]]
        placed = world(n_index)
        for prim in doc["meshes"][n["mesh"]]["primitives"]:
            p = acc(prim["attributes"]["POSITION"])
            j = acc(prim["attributes"]["JOINTS_0"]).astype(int)
            w = acc(prim["attributes"]["WEIGHTS_0"])
            d = 0.0
            for vi in range(len(p)):
                v = np.append(p[vi], 1)
                s = sum(w[vi][k] * (joint_world[j[vi][k]] @ ibm[j[vi][k]] @ v) for k in range(4))
                d = max(d, float(np.linalg.norm(s[:3] - (placed @ v)[:3])))
            worst = max(worst, d)
            print(f"  {n.get('name', ''):24} {len(p):5} vertices, skinned vs placed: max {d:8.3f} units")
    return worst


if __name__ == "__main__":
    bad = 0
    for path in sys.argv[1:]:
        print(path.rsplit("/", 1)[-1])
        bad += check(path) > 0.01
    sys.exit(1 if bad else 0)
