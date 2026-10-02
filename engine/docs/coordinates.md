# Coordinates

## Spaces

| | Up | Units | Handedness |
| --- | --- | --- | --- |
| Skyrim (world.fb, NIF) | +Z | game units | right-handed |
| Godot / glTF | +Y | metres | right-handed |

Conversion, as in bethconv's mesh writer:

- axes: rotate -90° about X, so Skyrim `(x, y, z)` becomes Godot `(x, z, -y)`;
- scale: 0.0142875 m per game unit (64 units per yard).

Converted meshes carry this conversion in their root node. A reference
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

## The model's root node

The game places a model by its reference and replaces whatever transform
the NIF's root node carries; `SkydotWorld` resets that node (the first child
of `bethconv_z_up_to_y_up`) when it places a reference. The converter keeps
the transform, so a mesh viewed on its own looks as in NifSkope. Riverwood
Trader's `CounterCornerIn01` has its root turned 90° about Z: applied, the
counter broke apart and its clutter floated (shot 2026-10-02 16:20).
