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
- Swims when the chest (1.25 m) is under `water_height`; leaves the water
  0.25 m above it. Swimming follows the look (yaw and pitch): forward looking
  down dives. Without move input the chest floats back to 0.1 m under the
  surface (at most 1 m/s); moving level or upwards stops there instead of
  leaving the water. The 0.1 m, the 1 m/s and the water's acceleration are
  assumed, as is diving at any downward pitch (no dead zone is known).
- The water's current (`water_current`, m/s) is added to the swim velocity,
  so without input the swimmer drifts at its speed (assumed: how the game's
  Havok water applies NAM0/XWCU to a character is not known). Not while
  wading.
- Breath: `is_underwater` (the eye under the surface) runs `get_breath` (1 to
  0) down over `breath_time`, 20 s, and refills it at once above water
  (assumed). 20 s is what players measure for vanilla Skyrim;
  fActorSwimBreathBase defaults to 10 s (CK Settings page, not in the esm), and
  how fActorSwimBreathMult (0.2) adds the rest is unknown, so the 20 s is
  fixed (assumed). Out of breath, GameSession takes fActorSwimBreathDamage
  (0.08) of the Health maximum per second (the CK's "percent of health lost
  per second"), unless the player's WaterBreathing actor value is above 0.
  The LeaveWaterBreath gasp (a voice topic) is not played.
- `fly` ignores collision; `hold` freezes it (the viewer holds it until the
  cell under it is built).
- `get_eye_position` interpolates between physics ticks, for the camera.

## Water

Skyrim makes water two ways (Skyrim.esm and Update.esm, read 2026-11-27):

- Cell water: each exterior cell's XCLW height and XCWT type (else the
  worldspace's), drawn as the cell's "Water" plane. Rivers above sea level are
  mostly this, stepping from cell to cell: the White River at Riverwood is
  cell water at -250 in most cells of (2..7, -13..-9). Skyrim.esm gives them
  RiverWaterFlowNE; Update.esm sets 613 cells to the SE flow-map type
  RiverWaterFlow, whose NAM0 current is 0. SE adds 641 per-cell flow maps,
  textures/water/skyrim.esm/flow.X.Y.dds (64x64 BGRA, north row first). The
  water shader reads RG as its rotation; as a direction (2c - 1, x east, y
  north) it points downstream in every river texel around Riverwood (the
  White River flows north-east: (0.88, 0.46) as RiverWaterFlowNE's NAM0) and
  is a unit vector. A is the pattern's scroll speed (0 to 1, mostly 1 in
  rivers), B its tiling. RG (120, 0) fills the rest: 99.6% of it is on dry
  land.
- Placed water: references of an ACTI with WNAM (a WATR): 62 such
  activators, 691 references (207 in Tamriel, 406 in interiors), models
  meshes/water/water1024.nif and similar (and some architecture and
  troughs). The model's shape uses BSWaterShaderProperty, drawn with the
  activator's water type (`Base.water`), else the cell's. Its water volume
  is a bhkSimpleShapePhantom box on layer 11 (water) whose top is the
  surface: water1024.nif's plane is at local z 0, the box from -1024 to 0
  over 1024 x 1024 units. The model's root carries it in the
  `skydot_phantoms` meta (physics/collision.hpp); the reference's transform
  places it. None is near Riverwood; the nearest White River one is
  0x3AC84 in (7, -6).

Currents: a WATR's NAM0 and NAM1 are the linear and angular velocity of its
current (xEdit), world axes: RiverWaterFlowNE (2.54, 1.35, 0); 17 of 34
Skyrim.esm types have a linear one, none an angular one. 128 placed
references carry XWCN 3 and XWCU (3 x X, Y, Z, W); the first entry is the
type's NAM0 as it was when placed (100 of 128 equal it, the rest older
values), the others and every W are 0. Rotating a reference does not turn
it.

The swimmer's water (`SkydotStreamer.hold_player`): the cell's water plane,
or a placed water box around the feet whose top is higher (the streamer keeps
the boxes of every reference it registers, the persistent cell's too, and of
interiors). Its current, all assumed, in Havok units (1 m) per second, game
axes turned into Godot's:

- placed water: the reference's XWCU first entry, else its type's NAM0;
- cell water: its type's NAM0; if that is 0 and the plane draws a flowmap,
  the texel under the feet: its direction times A times 2.88 (RiverWaterFlowNE's
  NAM0 speed); the fill texel pushes nothing. The game may not push by
  flowmaps at all (they may only be looks). Update.esm's 613 RiverWaterFlow
  cells had various Skyrim.esm types before: 247 none, 154 RiverWaterFlowNE
  (2.88), the rest 0.16 to 2.9; world.fb keeps only the winner, so one speed
  stands in for all.

`SkydotWorld`: `get_ref_water_type`, `get_ref_water_currents` (Vector4s, game
axes), `get_cell_water_type`, `get_water_velocity` (linear, angular).
`tests/smoke/water_sse.gd`.

## Picking

`SkydotWorld.pick_ref` casts the view ray against the world, clutter and
terrain layers first. The first body hit decides: its reference if that is
usable, otherwise nothing behind it can be picked. Model bounds still find
usable references without collision of their own (most triggers' activators,
some flora), as long as they are in front of that first body.

## Grab

Holding Activate (`ObjectGrab`, game/game/object_grab.gd) on a loose clutter body
for 0.35 s carries it: Havok's mouse spring at the body's centre pulls it to the
point ahead of the pointer at the distance it was grabbed from (the camera, or
the VR controller's aim). Letting go drops it, the attack button throws it, a
short press activates on release (picks up) as before; elsewhere Activate acts
on the press. Settings: fZKeyMaxForce 100 and fZKeyMaxForceWeightHigh 150 from
Skyrim.esm; spring elasticity 0.2, damping 0.5, object damping 0.75 and
fZKeyHeavyWeight 100 (heavier: not grabbed) are assumed exe values, as are the
hold time, the 1 m drop distance and the throw (8 m/s, at most 40 kg m/s).
Not grabbed: ragdolls (corpses, prop ragdolls) and static models.
`tests/smoke/grab_sse.gd`.

## Viewer

Walking is the default. The camera follows the player's eyes; P, saves and
the Papyrus player position use the player's feet. An interior entered
without `--at` puts the player where the game does: the arrival spot of the
door outside that leads in. A spot given in game units is lifted onto the
terrain if it is under it. Falling 200 m below the last ground switches to
flying.

## Open

- Not seen on screen yet: feel of the speeds, the step-up, swimming, diving.
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
