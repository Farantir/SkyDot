"""Minimal GLB read/write for the spike (JSON + one BIN chunk)."""

import json
import struct


def read(path):
    d = open(path, "rb").read()
    magic, _ver, _len = struct.unpack_from("<III", d, 0)
    assert magic == 0x46546C67
    jl, _ = struct.unpack_from("<II", d, 12)
    doc = json.loads(d[20:20 + jl])
    o = 20 + jl
    bin_ = b""
    if o < len(d):
        bl, _ = struct.unpack_from("<II", d, o)
        bin_ = d[o + 8:o + 8 + bl]
    return doc, bytearray(bin_)


def write(path, doc, bin_):
    while len(bin_) % 4:
        bin_ += b"\0"
    doc.setdefault("buffers", [{}])[0]["byteLength"] = len(bin_)
    j = json.dumps(doc, separators=(",", ":")).encode()
    j += b" " * (-len(j) % 4)
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(j) + 8 + len(bin_))
    out += struct.pack("<II", len(j), 0x4E4F534A) + j
    out += struct.pack("<II", len(bin_), 0x004E4942) + bytes(bin_)
    open(path, "wb").write(out)


def accessor(doc, bin_, data: bytes, comp, count, type_, minmax=None):
    """Append raw data as a new buffer view + accessor; return its index."""
    while len(bin_) % 4:
        bin_ += b"\0"
    doc.setdefault("bufferViews", []).append(
        {"buffer": 0, "byteOffset": len(bin_), "byteLength": len(data)})
    bin_ += data
    acc = {"bufferView": len(doc["bufferViews"]) - 1, "componentType": comp,
           "count": count, "type": type_}
    if minmax:
        acc["min"], acc["max"] = minmax
    doc.setdefault("accessors", []).append(acc)
    return len(doc["accessors"]) - 1
