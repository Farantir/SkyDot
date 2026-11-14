# Physics

Godot's Jolt (`game/project.godot`), fed with the Havok collision the
converter keeps in mesh extras (`../formats/pack-format.md`, "Collision in
meshes"). `SkydotWorld` attaches bodies when it places a model;
`collision = false` (viewer: `--collision off`) builds none.

## Bodies (`physics/collision.*`)

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
  collision (not synced to physics: synced, it follows only its own local
  transform, and the clips move its parent, so opened doors stayed shut;
  `game/tools/door_check.gd` opens the Sleeping Giant's doors and walks
  through); movable is a `SkydotDynamicBody` if it is the model's only body
  and has no mesh shape, else static.
- `SkydotDynamicBody` simulates in world space without scale (the shapes are
  scaled instead) and moves the model with it, from `_integrate_forces`,
  which only runs while it is awake. It starts frozen as placed: Jolt
  activates bodies when they enter the space, so sleep alone does not hold.
  `wake()` unfreezes it; the player wakes what it walks into, woken
  clutter wakes frozen clutter it hits, and a ragdoll wakes what its bodies
  touch or reach within the frame (`Ragdoll.wake_clutter`), as a sleeping
  Havok body is dynamic the moment it is hit. Clutter placed in the air
  stays there until touched, as in the game, except a reference with
  `RefFlags.dont_havok_settle` (REFR record flag 0x20000000, "Don't Havok
  Settle"): the editor did not settle it, and the game lets it fall or
  settle once the cell loads (measured 2026-11-13 with whole-cell dumps
  after coc: the Bleak Falls Barrow shaft bones and web sacs, flagged, lie
  on the floor 1 s after the load with the player 11,000 units away, while
  Dragonsreach and the Sleeping Giant, nothing flagged, keep their wall
  antlers and table food where placed; simulating all clutter from load
  dropped 99 Dragonsreach refs the game keeps). The cell builder marks such
  a reference (`k_settle_meta`) and its body wakes in `_ready`.
  `SkydotWorld.wake_clutter` wakes
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
- Constraints in actors' ragdolls (`game/game/ragdoll.gd`: the race
  skeleton's bodies and constraints as Jolt 6DOF joints; limits, the
  self-collision table and what Jolt cannot match are in its header and
  `godot-notes.md`) and in placed models that are ragdolls of their own:
  movable bodies on skinned bones joined by the NIF's constraints
  (`ModelCollision::ragdoll`, e.g. the web-wrapped "Dessicated Corpse",
  effects/fxspiderwebkitdinnerhumangnd.nif: three capsules on Bone01-03,
  clutter layer, two ragdoll constraints). A reference with
  `dont_havok_settle` gets no fixed bodies for them but the meta
  `skydot_ragdoll`, and `PropRagdolls` (game/game/prop_ragdolls.gd) builds
  the ragdoll with the actors' builder on the clutter layer, simulated from
  the load (the Bleak Falls Barrow sacs 0x486FA/0x486FC fall about 370 units
  to the floor, as in the game). Open: such a model the editor settled keeps
  its bodies fixed and is not woken by touch; constrained models whose
  bodies are not on skin bones (book covers, amulet chains, signs, hanging
  lanterns) stay static; the state dump reports such a reference at its
  placement, the body the game's ref position follows is not measured.
- Contact materials do not follow the game. Godot 4.7's Jolt module combines
  friction as min(a, b) and restitution as clamp(a + b, 0, 1)
  (`modules/jolt_physics/spaces/jolt_space_3d.cpp`, `SetCombineFriction` /
  `SetCombineRestitution`; no per-space override from GDExtension); Havok's
  rule (said to be sqrt(a b) for both) has no source here to cite. Only
  clutter gets its NIF friction and restitution; statics, animated bodies and
  terrain keep Godot's default (friction 1, bounce 0), which under min/sum
  leaves a ragdoll its own values (0.3 / 0.8 for the human, wolf and draugr).
  The terrain's material in the game is unknown (LTEX HNAM holds 2/0 for 66 of
  68 vanilla textures). Measured (sse_ragdoll_races RAGDOLL_PUSH=15 at the
  game's place on the Whiterun plain, 2026-11-09): travel human 17.2 m (game
  13.1), wolf 18.8 (18), draugr 21.4 (14.5); a ragdoll friction of 0.6 would
  bring the human to 13.5 m, restitution barely matters (0 to 0.8: 18.2 to
  17.2 m). Same friction data, the wolf matches and the bipeds slide too far:
  the gap is not one combine factor. Needs the game's terrain and static
  materials and its combine rule measured (SkyrimRemote) before a fix.
- Per-triangle Havok materials (sound, footsteps) are not kept.
