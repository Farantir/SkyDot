"""Decode every HKX under a folder and summarize what Skyrim uses.

    python3 census.py EXTRACTED_DIR [--jobs 12]
"""

import collections
import math
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import hkx


def one(path):
    r = {"path": path, "classes": {}, "error": None, "anims": []}
    try:
        h = hkx.load(open(path, "rb").read())
    except Exception as e:  # noqa: BLE001 - a census records every failure
        r["error"] = f"{type(e).__name__}: {e}"
        return r
    r["classes"] = h.classes
    r["ptr"] = h.pf.ptr
    r["skeletons"] = [(s.name, len(s.bones)) for s in h.skeletons]
    for a in h.animations:
        st = a.stats
        bad_q = 0
        for row in a.frames:
            for t, q, s in row:
                if any(math.isnan(x) or abs(x) > 1e5 for x in t + q + s):
                    bad_q += 1
        r["anims"].append({
            "kind": a.kind, "frames": a.num_frames, "tracks": a.tracks,
            "floats": a.float_tracks, "duration": a.duration,
            "rot_q": st.get("rot_q", {}), "slack": st.get("slack", []),
            "blocks": st.get("num_blocks", 0), "max_frames": st.get("max_frames", 0),
            "frame_dur": st.get("frame_dur", 0), "motion": a.extracted_motion,
            "mismatch": st.get("offset_mismatch", 0), "overlap": st.get("float_overlap", 0),
            "fmask": st.get("float_mask", {}),
            "notes": sum(len(n) for _, n in a.annotations), "bad": bad_q,
            "note_texts": [x.text.split(".")[0].split(" ")[0] for _, n in a.annotations for x in n],
        })
    r["bindings"] = [(b.skeleton_name, len(b.track_to_bone), b.blend_hint) for b in h.bindings]
    return r


def main():
    root = sys.argv[1]
    files = sorted(os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs if f.endswith(".hkx"))
    with ProcessPoolExecutor() as ex:
        results = list(ex.map(one, files, chunksize=16))
    classes = collections.Counter()
    errors = collections.Counter()
    rot_q = collections.Counter()
    kinds = collections.Counter()
    slack = collections.Counter()
    blend = collections.Counter()
    fps = collections.Counter()
    motion = collections.Counter()
    notes = collections.Counter()
    max_frames = collections.Counter()
    bad = frames = anims = multi = mismatch = overlap = 0
    fmasks = collections.Counter()
    bad_files, slack_files = [], []
    examples = {}
    for r in results:
        if r["error"]:
            errors[r["error"].split(":")[0] + ": " + r["error"].split(":", 1)[1][:60]] += 1
            examples.setdefault(r["error"][:60], r["path"])
            continue
        classes.update(r["classes"].keys())
        for b in r["bindings"]:
            blend[b[2]] += 1
        for a in r["anims"]:
            anims += 1
            kinds[a["kind"]] += 1
            frames += a["frames"]
            bad += a["bad"]
            rot_q.update(a["rot_q"].keys())
            for s in a["slack"]:
                slack["0-3" if 0 <= s < 4 else "4-15" if 0 <= s < 16 else str(s)] += 1
            if a["blocks"] > 1:
                multi += 1
            max_frames[a["max_frames"]] += 1
            fps[round(1 / a["frame_dur"]) if a["frame_dur"] else 0] += 1
            motion[a["motion"]] += 1
            notes.update(a["note_texts"])
            mismatch += a["mismatch"]; overlap += a["overlap"]
            fmasks.update({k: v for k, v in a["fmask"].items()})
            if a["bad"]:
                bad_files.append((a["bad"], r["path"][len(root):]))
            if any(s >= 16 for s in a["slack"]):
                slack_files.append((a["slack"], r["path"][len(root):]))
    print(f"{len(files)} files, {len(files) - sum(errors.values())} read, {sum(errors.values())} failed")
    for e, n in errors.most_common():
        print(f"  FAIL {n:5} {e}")
    for e, p in list(examples.items())[:10]:
        print(f"    e.g. {p[len(root):]}")
    print(f"files containing each class: {dict(classes.most_common())}")
    print(f"{anims} animations ({dict(kinds)}), {frames} frames, {multi} with several blocks, "
          f"{bad} non-finite or huge samples")
    print(f"rotation quantization (animations using it): {dict(rot_q)}")
    print(f"block slack bytes (decode end to next block): {dict(slack)}")
    print(f"max frames per block: {dict(max_frames.most_common(5))}; frame rate: {dict(fps)}")
    print(f"extracted motion: {dict(motion)}; binding blend hints: {dict(blend)}")
    print(f"track offset mismatches {mismatch}, float data overlapping transforms {overlap}, "
          f"float masks {dict(fmasks)}")
    print(f"files with bad samples: {sorted(bad_files, reverse=True)[:6]}")
    print(f"files with slack >= 16: {len(slack_files)}, e.g. {slack_files[:4]}")
    print(f"annotation kinds (first word, top 25): {dict(notes.most_common(25))}")


if __name__ == "__main__":
    main()
