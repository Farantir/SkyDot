# Coordinates

## Spaces

| | Up | Units | Handedness |
| --- | --- | --- | --- |
| Skyrim (world.fb, NIF) | +Z | game units | right-handed |
| Godot / glTF | +Y | metres | right-handed |

Conversion, as in bethconv's mesh writer:

- axes: rotate -90° about X, so Skyrim `(x, y, z)` becomes Godot `(x, z, -y)`;
- scale: 0.0142875 m per game unit (64 units per yard).

Baked scenes already contain this conversion in their root node. A reference
is placed with `C · T · C⁻¹`, where `T` is its Skyrim transform and `C` the
axis rotation; `SkydotWorld.skyrim_transform` does this.

## Reference rotation

REFR `DATA` stores three angles in radians. Skyrim's angles are clockwise
(negative in a right-handed frame) and applied Z first, then Y, then X:

    R = Rx(-x) · Ry(-y) · Rz(-z)

The sign of the Z rotation is confirmed visually: Breezehome's wall, floor and
roof pieces join without gaps. The X/Y order is not yet confirmed; few objects
in that cell are tilted. Check it against a cell with tilted clutter (fallen
books, leaning weapons) before relying on it.
