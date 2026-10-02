# Physics

Godot's Jolt (`game/project.godot`), fed with the Havok collision the
converter keeps in mesh extras (`../formats/pack-format.md`, "Collision in
meshes"). `SkydotWorld` attaches bodies when it places a model;
`collision = false` (viewer: `--collision off`) builds none.

## Bodies (`world/collision.*`)

- Parsed once per model on the loader's thread (`ModelCollision::take_from`),
  which also drops the collision arrays from the template's extras. Godot
  shapes are made on first use on the main thread and shared by every copy.
- Havok units times 69.99124 are game units, the unit of the node the body
  hangs off; the body inherits the model's and the reference's scale (uniform,
  which Jolt takes).
- Kinds: box (convex radius added to the half extents), sphere, capsule,
  cylinder, convex hull, mesh (`ConcavePolygonShape3D`, both sides solid:
  compressed meshes and `bhkNiTriStripsShape`). The one vanilla
  `bhkPlaneShape` arrives as a flat hull. Hulls thinner than 2 units (load
  door planes, rugs: Havok thickens them by their convex radius) become
  slabs, since Jolt cannot build a hull without volume. Every vanilla SE
  mesh builds its bodies without an error: 13,326 bodies in 11,111 models,
  69 of them cylinders (`game/tools/collision_check.gd`).
- By layer: static, animated static, transparent, trees, terrain, trap,
  ground, invisible walls and stair helpers are solid; clutter, weapons,
  props and debris may move; the rest (biped, triggers, water,
  non-collidable, picking volumes) is ignored.
- By `quality_type` (`hkpCollidableQualityType`: 0 fixed, 1 keyframed, 2 to 7
  moving): fixed is a `StaticBody3D`; keyframed, or layer animated static, an
  `AnimatableBody3D` under its node, so animated doors and gates carry their
  collision; movable is a `SkydotDynamicBody` if it is the model's only body
  and has no mesh shape, else static.
- `SkydotDynamicBody` simulates in world space without scale (the shapes are
  scaled instead) and moves the model with it, from `_integrate_forces`,
  which only runs while it is awake. It starts frozen as placed: Jolt
  activates bodies when they enter the space, so sleep alone does not hold.
  `wake()` unfreezes it; the player wakes what it walks into, and woken
  clutter wakes frozen clutter it hits. `SkydotWorld.wake_clutter` wakes
  what lies near a point: the viewer calls it around a reference a script
  disables or animates, so what rested on it falls. Papyrus
  `ApplyHavokImpulse` wakes and pushes clutter; `SetMotionType` releases it
  (the moving types) or holds it (keyframed, fixed).
- Terrain: one mesh shape per cell with the triangles the quadrants draw, on
  its own layer.

Layers (bits; `SkydotPlayer.LAYER_*`): 1 world, 2 clutter, 4 actor (the
player), 8 terrain, 16 NPC (placed actors, `SkydotActor`; see actors.md).
The player collides with NPCs; NPCs with the world, clutter, terrain, the
player and each other.

Riverwood's nine cells: 743 bodies (693 static, 45 clutter, 14 animated). A
streaming benchmark there is the same with and without collision.

## Player (`SkydotPlayer`)

A `CharacterBody3D` with its origin at the feet: a cylinder 1.83 m tall,
0.3 m in radius, eyes at 1.7 m. Flat-bottomed on purpose: a capsule's round
bottom meets a step's edge at a slant and slides off.

- Walk 1.6, run 5, sprint 7.5 m/s (roughly the game's); 1.1 m jumps; 50°
  slopes.
- Steps up to 0.45 m: when blocked, it tries up, forward and down, going far
  enough onto the step (8 cm past the edge) that the flat bottom stands on it
  rather than on Jolt's rounded edge. It never steps onto loose clutter but
  pushes it.
- Swims when the chest (1.25 m) is under `water_height`, floating just under
  the surface; leaves the water 0.25 m above it.
- `fly` ignores collision; `hold` freezes it (the viewer holds it until the
  cell under it is built).
- `get_eye_position` interpolates between physics ticks, for the camera.

## Picking

`SkydotWorld.pick_ref` casts the view ray against the world, clutter and
terrain layers first. The first body hit decides: its reference if that is
usable, otherwise nothing behind it can be picked. Model bounds still find
usable references without collision of their own (most triggers' activators,
some flora), as long as they are in front of that first body.

## Viewer

Walking is the default. The camera follows the player's eyes; P, saves and
the Papyrus player position use the player's feet. An interior entered
without `--at` puts the player where the game does: the arrival spot of the
door outside that leads in. A spot given in game units is lifted onto the
terrain if it is under it. Falling 200 m below the last ground switches to
flying.

## Open

- Not seen on screen yet: feel of the speeds, the step-up, swimming.
- Explosions do not wake clutter, and `SetMotionType` cannot make a static
  model move (only models built as clutter can).
- No constraints: chains, hanging lanterns and multi-body ragdolls stay
  static.
- Per-triangle Havok materials (sound, footsteps) are not kept.
