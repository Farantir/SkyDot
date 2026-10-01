#!/usr/bin/env python3
"""Print every texture the GLBs in a tree reference, relative to the tree.

URIs are resolved against their GLB, as a glTF consumer does. Exits 1 if a
reference escapes the tree.
"""
import json
import struct
import sys
import pathlib

root = pathlib.Path(sys.argv[1]).resolve()
found, escaping = set(), []
for glb in sorted(root.rglob('*.glb')):
    b = glb.read_bytes()
    length, kind = struct.unpack_from('<II', b, 12)
    assert kind == 0x4E4F534A, f'{glb}: first chunk is not JSON'
    doc = json.loads(b[20:20 + length])
    for image in doc.get('images', []):
        uri = image.get('uri')
        if uri is None:
            continue
        target = (glb.parent / uri).resolve()
        try:
            found.add(str(target.relative_to(root)))
        except ValueError:
            escaping.append((str(glb), uri))

for path in sorted(found):
    print(path)
for glb, uri in escaping:
    print(f'ESCAPES THE TREE: {glb} -> {uri}', file=sys.stderr)
sys.exit(1 if escaping else 0)
