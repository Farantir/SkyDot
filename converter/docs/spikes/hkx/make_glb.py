"""Skeleton GLB + skinned body parts + HKX clips -> one animated GLB.

    python3 make_glb.py OUT.glb SKELETON.glb SKELETON.hkx \
        --part body.glb --part hands.glb ... --clip walk.hkx --clip run.hkx

The skeleton GLB is bethconv's conversion of skeleton.nif; body parts are
bethconv GLBs whose skins name skeleton bones. Each clip's tracks drive the
skeleton nodes of the same name (case-insensitive), in game units: the
converter's z-up/unit node sits above the skeleton, so local transforms
need no conversion. Materials are dropped (grey), which shows motion best.
"""

import argparse
import os
import struct

import glb
import hkx

FLOAT, USHORT = 5126, 5123


def merge_part(doc, bin_, part_path, name_to_node, attach_to):
    pdoc, pbin = glb.read(part_path)
    view_base = len(doc.get("bufferViews", []))
    acc_base = len(doc.get("accessors", []))
    while len(bin_) % 16:
        bin_ += b"\0"
    off = len(bin_)
    bin_ += pbin
    for v in pdoc.get("bufferViews", []):
        v = dict(v)
        v["byteOffset"] = v.get("byteOffset", 0) + off
        doc.setdefault("bufferViews", []).append(v)
    for a in pdoc.get("accessors", []):
        a = dict(a)
        if "bufferView" in a:
            a["bufferView"] += view_base
        doc.setdefault("accessors", []).append(a)
    pnodes = pdoc["nodes"]
    for ni, n in enumerate(pnodes):
        if "mesh" not in n:
            continue
        mesh = pdoc["meshes"][n["mesh"]]
        prims = []
        for p in mesh["primitives"]:
            p = {"attributes": {k: v + acc_base for k, v in p["attributes"].items()},
                 "indices": p["indices"] + acc_base, "mode": p.get("mode", 4)}
            prims.append(p)
        doc.setdefault("meshes", []).append({"name": mesh.get("name", ""), "primitives": prims})
        node = {"name": n.get("name", "part"), "mesh": len(doc["meshes"]) - 1}
        if "skin" in n:
            skin = pdoc["skins"][n["skin"]]
            joints = []
            for j in skin["joints"]:
                jn = pnodes[j].get("name", "").lower()
                if jn not in name_to_node:
                    raise SystemExit(f"{part_path}: joint {jn!r} not in the skeleton")
                joints.append(name_to_node[jn])
            doc.setdefault("skins", []).append({
                "joints": joints,
                "inverseBindMatrices": skin["inverseBindMatrices"] + acc_base})
            node["skin"] = len(doc["skins"]) - 1
        doc["nodes"].append(node)
        doc["nodes"][attach_to].setdefault("children", []).append(len(doc["nodes"]) - 1)


def add_clip(doc, bin_, path, skeleton, name_to_node, stats):
    h = hkx.load(open(path, "rb").read())
    a = h.animations[0]
    t2b = h.bindings[0].track_to_bone if h.bindings and h.bindings[0].track_to_bone else list(range(a.tracks))
    fd = a.stats.get("frame_dur") or (a.duration / max(1, a.num_frames - 1))
    times = [f * fd for f in range(a.num_frames)]
    t_acc = glb.accessor(doc, bin_, struct.pack(f"<{len(times)}f", *times), FLOAT,
                         len(times), "SCALAR", ([times[0]], [times[-1]]))
    samplers, channels = [], []
    skipped = []
    for track in range(a.tracks):
        bone = skeleton.bones[t2b[track]]
        node = name_to_node.get(bone.lower())
        if node is None:
            skipped.append(bone)
            continue
        ts = [a.frames[f][track][0] for f in range(a.num_frames)]
        qs = [a.frames[f][track][1] for f in range(a.num_frames)]
        # Hide parked prop bones (see census: ~2e7 units away) by not
        # animating them; they keep their rest pose.
        if any(abs(x) > 1e5 for t in ts for x in t):
            skipped.append(bone + " (parked)")
            continue
        # Keep quaternions in one hemisphere so linear interpolation is short.
        for i in range(1, len(qs)):
            if sum(x * y for x, y in zip(qs[i - 1], qs[i])) < 0:
                qs[i] = [-x for x in qs[i]]
        for prop, vals, typ in (("translation", ts, "VEC3"), ("rotation", qs, "VEC4")):
            flat = [x for v in vals for x in v]
            acc = glb.accessor(doc, bin_, struct.pack(f"<{len(flat)}f", *flat), FLOAT,
                               len(vals), typ)
            samplers.append({"input": t_acc, "output": acc, "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": prop}})
    name = os.path.splitext(os.path.basename(path))[0]
    doc.setdefault("animations", []).append({"name": name, "samplers": samplers, "channels": channels})
    stats.append((name, a.num_frames, a.duration, len(channels) // 2, skipped))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("skeleton_glb")
    ap.add_argument("skeleton_hkx")
    ap.add_argument("--part", action="append", default=[])
    ap.add_argument("--clip", action="append", default=[])
    args = ap.parse_args()

    doc, bin_ = glb.read(args.skeleton_glb)
    doc.pop("animations", None)
    root = doc["scenes"][0]["nodes"][0]
    doc["nodes"][root].pop("extras", None)  # bethconv's visibility clip
    name_to_node = {n.get("name", "").lower(): i for i, n in enumerate(doc["nodes"])}
    skeleton = hkx.load(open(args.skeleton_hkx, "rb").read()).skeletons[0]
    for p in args.part:
        merge_part(doc, bin_, p, name_to_node, root)
    stats = []
    for c in args.clip:
        add_clip(doc, bin_, c, skeleton, name_to_node, stats)
    doc.pop("materials", None)
    glb.write(args.out, doc, bin_)
    for name, frames, dur, bones, skipped in stats:
        print(f"{name}: {frames} frames, {dur:.2f} s, {bones} bones animated, skipped {skipped}")


if __name__ == "__main__":
    main()
