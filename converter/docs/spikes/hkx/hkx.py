"""Read Skyrim's Havok packfiles (hk_2010.2.0-r1): skeletons and animations.

Spike code (see ../hkx.md). LE files use 4-byte pointers, SE files 8-byte;
everything else is the same. Only the classes Skyrim's skeletons and
animations use are read; their member offsets are fixed per pointer size
because the files carry no type information (__types__ is empty).

    python3 hkx.py FILE.hkx            # summary
    python3 hkx.py FILE.hkx --frames   # also print decoded track 0
"""

from __future__ import annotations

import math
import struct
import sys
from dataclasses import dataclass, field

MAGIC = (0x57E0E057, 0x10C0C010)


class HkxError(Exception):
    pass


@dataclass
class Section:
    name: str
    start: int
    local: int
    global_: int
    virtual: int
    exports: int
    imports: int
    end: int


class Packfile:
    """Packfile bytes with pointer fixups applied as lookup tables."""

    def __init__(self, data: bytes):
        self.d = data
        if len(data) < 0x40:
            raise HkxError("too short")
        m0, m1, _tag, version = struct.unpack_from("<IIii", data, 0)
        if (m0, m1) != MAGIC:
            raise HkxError("not a packfile")
        self.ptr, self.little, self.reuse_padding, self.empty_base = data[16:20]
        if self.ptr not in (4, 8) or self.little != 1:
            raise HkxError(f"unsupported layout {data[16:20].hex()}")
        nsec, self.contents_sec, self.contents_off = struct.unpack_from("<iii", data, 20)
        self.version_string = data[40:56].split(b"\0")[0].decode("ascii", "replace")
        if version != 8:
            raise HkxError(f"packfile version {version}")
        self.sections = []
        off = 0x40
        for _ in range(nsec):
            name = data[off:off + 19].split(b"\0")[0].decode()
            v = struct.unpack_from("<7i", data, off + 20)
            s = Section(name, v[0], *[v[0] + x for x in v[1:]])
            self.sections.append(s)
            off += 48
        # Pointer tables: absolute file offset of a pointer -> absolute target.
        self.ptrs: dict[int, int] = {}
        self.cls: dict[int, str] = {}  # object absolute offset -> class name
        for s in self.sections:
            for p in range(s.local, s.global_ - 7, 8):
                src, dst = struct.unpack_from("<ii", data, p)
                if src != -1:
                    self.ptrs[s.start + src] = s.start + dst
            for p in range(s.global_, s.virtual - 11, 12):
                src, sec, dst = struct.unpack_from("<iii", data, p)
                if src != -1:
                    self.ptrs[s.start + src] = self.sections[sec].start + dst
            for p in range(s.virtual, s.exports - 11, 12):
                src, sec, name_off = struct.unpack_from("<iii", data, p)
                if src != -1:
                    a = self.sections[sec].start + name_off
                    self.cls[s.start + src] = data[a:data.index(b"\0", a)].decode()

    # --- primitive readers at absolute offsets ---
    def u8(self, a): return self.d[a]
    def i16(self, a): return struct.unpack_from("<h", self.d, a)[0]
    def u16(self, a): return struct.unpack_from("<H", self.d, a)[0]
    def i32(self, a): return struct.unpack_from("<i", self.d, a)[0]
    def f32(self, a): return struct.unpack_from("<f", self.d, a)[0]
    def floats(self, a, n): return list(struct.unpack_from(f"<{n}f", self.d, a))

    def ptr_at(self, a) -> int | None:
        return self.ptrs.get(a)

    def cstr(self, a) -> str | None:
        t = self.ptr_at(a)
        if t is None:
            return None
        return self.d[t:self.d.index(b"\0", t)].decode("utf-8", "replace")

    def array(self, a) -> tuple[int | None, int]:
        """hkArray: (data address, size)."""
        size = self.i32(a + self.ptr)
        return self.ptr_at(a), size

    @property
    def arr(self) -> int:  # sizeof(hkArray)
        return self.ptr + 8

    @property
    def ref(self) -> int:  # sizeof(hkReferencedObject)
        return self.ptr + 4 + (4 if self.ptr == 8 else 0)

    def root(self) -> int:
        return self.sections[self.contents_sec].start + self.contents_off

    def objects_of(self, name: str) -> list[int]:
        return sorted(a for a, c in self.cls.items() if c == name)


# --------------------------------------------------------------------------
# Classes

@dataclass
class Skeleton:
    name: str
    parents: list[int]
    bones: list[str]
    # Reference pose per bone: (translation xyz, rotation xyzw, scale xyz)
    pose: list[tuple[list[float], list[float], list[float]]]
    float_slots: list[str] = field(default_factory=list)


@dataclass
class Annotation:
    time: float
    text: str


@dataclass
class Animation:
    kind: str
    duration: float
    tracks: int
    float_tracks: int
    annotations: list[tuple[str, list[Annotation]]]
    num_frames: int = 0
    # frames[f][track] = (t, q, s)
    frames: list[list[tuple[list[float], list[float], list[float]]]] = field(default_factory=list)
    float_frames: list[list[float]] = field(default_factory=list)
    stats: dict = field(default_factory=dict)
    extracted_motion: str | None = None


@dataclass
class Binding:
    skeleton_name: str
    animation: int | None
    track_to_bone: list[int]
    float_to_slot: list[int]
    blend_hint: int


def qs_transform(pf: Packfile, a: int):
    t = pf.floats(a, 3)
    q = pf.floats(a + 16, 4)
    s = pf.floats(a + 32, 3)
    return t, q, s


def read_skeleton(pf: Packfile, a: int) -> Skeleton:
    r, A = pf.ref, pf.arr
    name = pf.cstr(a + r) or ""
    o = a + r + pf.ptr
    pa, pn = pf.array(o)
    parents = [pf.i16(pa + 2 * i) for i in range(pn)] if pn else []
    ba, bn = pf.array(o + A)
    bone_size = pf.ptr * 2  # hkStringPtr + hkBool, padded
    bones = [pf.cstr(ba + i * bone_size) or "" for i in range(bn)] if bn else []
    ra, rn = pf.array(o + 2 * A)
    pose = [qs_transform(pf, ra + 48 * i) for i in range(rn)] if rn else []
    fa, fn = pf.array(o + 4 * A)  # floatSlots after referenceFloats
    slots = [pf.cstr(fa + i * pf.ptr) or "" for i in range(fn)] if fn else []
    if not (len(parents) == len(bones) == len(pose)):
        raise HkxError(f"skeleton {name}: {len(parents)} parents, {len(bones)} bones, "
                       f"{len(pose)} poses")
    return Skeleton(name, parents, bones, pose, slots)


def read_binding(pf: Packfile, a: int) -> Binding:
    r, A = pf.ref, pf.arr
    name = pf.cstr(a + r) or ""
    anim = pf.ptr_at(a + r + pf.ptr)
    o = a + r + 2 * pf.ptr
    ta, tn = pf.array(o)
    t2b = [pf.i16(ta + 2 * i) for i in range(tn)] if tn else []
    fa, fn = pf.array(o + A)
    f2s = [pf.i16(fa + 2 * i) for i in range(fn)] if fn else []
    return Binding(name, anim, t2b, f2s, pf.u8(o + 2 * A))


def read_annotations(pf: Packfile, a: int, n: int):
    out = []
    track_size = pf.ptr + pf.arr
    for i in range(n):
        t = a + i * track_size
        name = pf.cstr(t) or ""
        aa, an = pf.array(t + pf.ptr)
        notes = []
        for j in range(an):
            e = aa + j * (2 * pf.ptr)  # float time, padded; hkStringPtr text
            notes.append(Annotation(pf.f32(e), pf.cstr(e + pf.ptr) or ""))
        out.append((name, notes))
    return out


ANIM_TYPES = {0: "unknown", 1: "interleaved", 2: "delta", 3: "wavelet",
              4: "mirrored", 5: "spline", 6: "quantized", 7: "predictive", 8: "reference"}


def read_animation(pf: Packfile, a: int, decode=True) -> Animation:
    r, A, P = pf.ref, pf.arr, pf.ptr
    kind = pf.i32(a + r)
    duration = pf.f32(a + r + 4)
    ntracks = pf.i32(a + r + 8)
    nfloats = pf.i32(a + r + 12)
    motion = pf.ptr_at(a + r + 16)
    ann_a, ann_n = pf.array(a + r + 16 + P)
    anim = Animation(ANIM_TYPES.get(kind, str(kind)), duration, ntracks, nfloats,
                     read_annotations(pf, ann_a, ann_n) if ann_n else [],
                     extracted_motion=pf.cls.get(motion) if motion is not None else None)
    base = a + r + 16 + P + A  # end of hkaAnimation
    cls = pf.cls.get(a)
    if cls == "hkaSplineCompressedAnimation":
        if decode:
            decode_spline(pf, base, anim)
    elif cls == "hkaInterleavedUncompressedAnimation":
        if decode:
            decode_interleaved(pf, base, anim)
    else:
        raise HkxError(f"animation class {cls}")
    return anim


def decode_interleaved(pf: Packfile, base: int, anim: Animation):
    ta, tn = pf.array(base)
    fa, fn = pf.array(base + pf.arr)
    n = anim.tracks
    nf = tn // n if n else 0
    anim.num_frames = nf
    anim.frames = [[qs_transform(pf, ta + 48 * (f * n + k)) for k in range(n)] for f in range(nf)]
    if anim.float_tracks:
        m = anim.float_tracks
        anim.float_frames = [pf.floats(fa + 4 * f * m, m) for f in range(fn // m)]


# --- spline-compressed ---------------------------------------------------

# Rotation quantizations: (name, bytes, alignment)
ROT_Q = {0: ("POLAR32", 4, 4), 1: ("THREECOMP40", 5, 1), 2: ("THREECOMP48", 6, 2),
         3: ("THREECOMP24", 3, 1), 4: ("STRAIGHT16", 2, 2), 5: ("UNCOMPRESSED", 16, 4)}


def align(o, n):
    return (o + n - 1) // n * n


def quat_40(b: bytes):
    c = int.from_bytes(b[:5], "little")
    mask = (1 << 12) - 1
    half = mask >> 1
    frac = 1.0 / (half * math.sqrt(2.0))
    v = [((c >> (12 * i)) & mask) - half for i in range(3)]
    shift = (c >> 36) & 3
    neg = (c >> 38) & 1
    t = [x * frac for x in v]
    return insert_w(t, shift, neg)


def quat_48(b: bytes):
    x, y, z = struct.unpack_from("<HHH", b, 0)
    shift = ((y >> 14) & 2) | ((x >> 15) & 1)
    neg = (z >> 15) & 1
    mask = (1 << 15) - 1
    half = mask >> 1
    frac = 1.0 / (half * math.sqrt(2.0))
    t = [((v & mask) - half) * frac for v in (x, y, z)]
    return insert_w(t, shift, neg)


def insert_w(t, shift, neg):
    w2 = 1.0 - sum(x * x for x in t)
    w = math.sqrt(max(0.0, w2))
    if neg:
        w = -w
    out = list(t)
    out.insert(shift, w)
    return out


def quat_uncompressed(b: bytes):
    return list(struct.unpack_from("<4f", b, 0))


QUAT = {1: quat_40, 2: quat_48, 5: quat_uncompressed}


class SplineReader:
    def __init__(self, pf: Packfile, start: int, stats: dict):
        self.pf, self.o, self.stats = pf, start, stats

    def align(self, n):
        self.o = align(self.o, n)

    def u8(self):
        v = self.pf.d[self.o]; self.o += 1; return v

    def u16(self):
        v = self.pf.u16(self.o); self.o += 2; return v

    def f32(self):
        v = self.pf.f32(self.o); self.o += 4; return v

    def knots(self):
        n = self.u16()
        p = self.u8()
        k = [self.u8() for _ in range(n + p + 2)]
        return n, p, k

    def vector(self, flags, q8: bool, default):
        """Position or scale: a spline, static values, or the default."""
        spline_axes = [(flags >> (4 + i)) & 1 for i in range(3)]
        static_axes = [(flags >> i) & 1 for i in range(3)]
        if any(spline_axes):
            n, p, knots = self.knots()
            self.align(4)
            lo, hi, const = [0.0] * 3, [0.0] * 3, list(default)
            for i in range(3):
                if spline_axes[i]:
                    lo[i] = self.f32(); hi[i] = self.f32()
                elif static_axes[i]:
                    const[i] = self.f32()
            cps = []
            for _ in range(n + 1):
                v = list(const)
                for i in range(3):
                    if spline_axes[i]:
                        if q8:
                            q = self.u8() / 255.0
                        else:
                            q = self.u16() / 65535.0
                        v[i] = lo[i] + (hi[i] - lo[i]) * q
                cps.append(v)
            self.align(4)
            return ("spline", n, p, knots, cps)
        v = list(default)
        for i in range(3):
            if static_axes[i]:
                v[i] = self.f32()
        return ("static", v)

    def rotation(self, flags, qtype):
        if qtype not in QUAT:
            raise HkxError(f"rotation quantization {ROT_Q.get(qtype, (qtype,))[0]}")
        name, size, al = ROT_Q[qtype]
        fn = QUAT[qtype]
        self.stats.setdefault("rot_q", {}).setdefault(name, 0)
        self.stats["rot_q"][name] += 1
        if flags & 0xF0:
            n, p, knots = self.knots()
            self.align(al)
            cps = []
            for _ in range(n + 1):
                cps.append(fn(self.pf.d[self.o:self.o + size])); self.o += size
            self.align(4)
            return ("spline", n, p, knots, cps)
        if flags & 0x0F:
            self.align(al)
            q = fn(self.pf.d[self.o:self.o + size]); self.o += size
            self.align(4)
            return ("static", q)
        return ("static", [0.0, 0.0, 0.0, 1.0])

    def floats(self, flag):
        """A float track: flag bit 0xF0 spline, 0x0F static.

        Vanilla only has masks 0x03 (static) and 0x12 (spline); every spline
        float track's control points are 16-bit. Which bit selects the
        quantization is not known (no 8-bit float track exists to tell)."""
        if flag & 0xF0:
            n, p, knots = self.knots()
            self.align(4)
            lo, hi = self.f32(), self.f32()
            cps = [[lo + (hi - lo) * (self.u16() / 65535.0)] for _ in range(n + 1)]
            self.align(4)
            return ("spline", n, p, knots, cps)
        if flag & 0x0F:
            return ("static", [self.f32()])
        return ("static", [0.0])


def find_span(n, p, u, knots):
    if u >= knots[n + 1]:
        return n
    lo, hi = p, n + 1
    mid = (lo + hi) // 2
    while u < knots[mid] or u >= knots[mid + 1]:
        if u < knots[mid]:
            hi = mid
        else:
            lo = mid
        mid = (lo + hi) // 2
    return mid


def de_boor(span, p, u, knots, cps):
    d = [list(cps[j + span - p]) for j in range(p + 1)]
    for r in range(1, p + 1):
        for j in range(p, r - 1, -1):
            i = j + span - p
            den = knots[i + p - r + 1] - knots[i]
            alpha = (u - knots[i]) / den if den else 0.0
            d[j] = [(1 - alpha) * a + alpha * b for a, b in zip(d[j - 1], d[j])]
    return d[p]


def evaluate(track, u):
    if track[0] == "static":
        return list(track[1])
    _, n, p, knots, cps = track
    return de_boor(find_span(n, p, u, knots), p, u, knots, cps)


def normalize(q):
    m = math.sqrt(sum(x * x for x in q)) or 1.0
    return [x / m for x in q]


def decode_spline(pf: Packfile, base: int, anim: Animation):
    i32, f32, A = pf.i32, pf.f32, pf.arr
    num_frames = i32(base)
    num_blocks = i32(base + 4)
    max_frames = i32(base + 8)
    mask_q_size = i32(base + 12)
    block_dur = f32(base + 16)
    block_inv = f32(base + 20)
    frame_dur = f32(base + 24)
    arrays = align(base + 28, pf.ptr)

    def ints(k):
        a, n = pf.array(arrays + k * A)
        return [i32(a + 4 * i) for i in range(n)] if n else []

    block_offsets, float_block_offsets, transform_offsets, float_offsets = (ints(k) for k in range(4))
    data, data_n = pf.array(arrays + 4 * A)
    stats = anim.stats
    stats.update(num_blocks=num_blocks, max_frames=max_frames, frame_dur=frame_dur,
                 block_dur=block_dur, data=data_n, mask_q_size=mask_q_size)
    anim.num_frames = num_frames
    if len(block_offsets) != num_blocks:
        raise HkxError(f"{len(block_offsets)} block offsets for {num_blocks} blocks")
    nt, nfl = anim.tracks, anim.float_tracks
    blocks = []
    for b in range(num_blocks):
        start = data + block_offsets[b]
        masks = [pf.d[start + 4 * t:start + 4 * t + 4] for t in range(nt)]
        fmask = [pf.d[start + 4 * nt + i] for i in range(nfl)]
        rd = SplineReader(pf, start + align(mask_q_size, 4), stats)
        tracks = []
        for t in range(nt):
            # transformOffsets, when present, gives every track's start: a
            # free check that each track's decode used the right length.
            if transform_offsets:
                expect = start + transform_offsets[b * nt + t]
                if rd.o != expect:
                    stats["offset_mismatch"] = stats.get("offset_mismatch", 0) + 1
                    rd.o = expect
            qt, pt, rt, st = masks[t]
            pos = rd.vector(pt, (qt & 3) == 0, [0.0, 0.0, 0.0])
            rot = rd.rotation(rt, (qt >> 2) & 0xF)
            scl = rd.vector(st, ((qt >> 6) & 3) == 0, [1.0, 1.0, 1.0])
            tracks.append((pos, rot, scl))
        transform_end = rd.o - start
        fl = []
        if nfl:
            rd.o = start + float_block_offsets[b]
            if rd.o - start < transform_end:
                stats["float_overlap"] = stats.get("float_overlap", 0) + 1
            fl = [rd.floats(fmask[i]) for i in range(nfl)]
            for i in range(nfl):
                stats.setdefault("float_mask", {}).setdefault(fmask[i], 0)
                stats["float_mask"][fmask[i]] += 1
        blocks.append((tracks, fl, rd.o - start))
    stats["block_bytes"] = [x[2] for x in blocks]
    # How well the decode fits the data: bytes between where a block's
    # decode ended and where the next block (or the data) begins.
    ends = block_offsets[1:] + [data_n]
    stats["slack"] = [e - (o + x[2]) for e, o, x in zip(ends, block_offsets, blocks)]
    # Sample every frame. Consecutive blocks share their boundary frame.
    per_block = max_frames - 1
    for f in range(num_frames):
        b = min(f // per_block, num_blocks - 1) if per_block > 0 else 0
        u = float(f - b * per_block)
        tracks, fl, _ = blocks[b]
        row = []
        for pos, rot, scl in tracks:
            row.append((evaluate(pos, u), normalize(evaluate(rot, u)), evaluate(scl, u)))
        anim.frames.append(row)
        if nfl:
            anim.float_frames.append([evaluate(x, u)[0] for x in fl])


# --------------------------------------------------------------------------

@dataclass
class HkxFile:
    pf: Packfile
    skeletons: list[Skeleton]
    animations: list[Animation]
    bindings: list[Binding]
    classes: dict[str, int]


def load(data: bytes, decode=True) -> HkxFile:
    pf = Packfile(data)
    classes: dict[str, int] = {}
    for c in pf.cls.values():
        classes[c] = classes.get(c, 0) + 1
    skeletons = [read_skeleton(pf, a) for a in pf.objects_of("hkaSkeleton")]
    anim_addrs = (pf.objects_of("hkaSplineCompressedAnimation")
                  + pf.objects_of("hkaInterleavedUncompressedAnimation"))
    animations = [read_animation(pf, a, decode) for a in sorted(anim_addrs)]
    bindings = [read_binding(pf, a) for a in pf.objects_of("hkaAnimationBinding")]
    return HkxFile(pf, skeletons, animations, bindings, classes)


def main(argv):
    path = argv[1]
    h = load(open(path, "rb").read())
    pf = h.pf
    print(f"{path}: {pf.version_string}, {pf.ptr}-byte pointers, classes {h.classes}")
    for s in h.skeletons:
        print(f"skeleton '{s.name}': {len(s.bones)} bones, float slots {s.float_slots}")
        for i, (b, p, pose) in enumerate(zip(s.bones, s.parents, s.pose)):
            if i < 6 or "--bones" in argv:
                t, q, sc = pose
                print(f"  {i:3} {b:32} parent {p:3} t={[round(x, 3) for x in t]} "
                      f"q={[round(x, 4) for x in q]} s={[round(x, 3) for x in sc]}")
    for a in h.animations:
        print(f"animation {a.kind}: {a.duration:.3f} s, {a.num_frames} frames, "
              f"{a.tracks} tracks, {a.float_tracks} float tracks, motion {a.extracted_motion}")
        print(f"  stats {a.stats}")
        for name, notes in a.annotations:
            if notes:
                print(f"  annotations '{name}': {[(round(n.time, 3), n.text) for n in notes]}")
        if "--frames" in argv:
            for f in range(0, a.num_frames, max(1, a.num_frames // 10)):
                t, q, s = a.frames[f][0]
                print(f"  frame {f:3} track0 t={[round(x, 3) for x in t]} q={[round(x, 4) for x in q]}")
    for b in h.bindings:
        print(f"binding to '{b.skeleton_name}': {len(b.track_to_bone)} tracks mapped, "
              f"blend {b.blend_hint}")


if __name__ == "__main__":
    main(sys.argv)
