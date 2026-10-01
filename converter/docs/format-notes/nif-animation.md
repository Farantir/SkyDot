# NIF controllers and particle systems

What the mesh writer puts into GLB extras so an engine can animate a model.
glTF animations are not used: most of what Skyrim animates (UV offsets,
emissive colour, alpha-test thresholds, particle emission) has no glTF target,
and an importer drops materials no mesh uses, which particle systems need.

## Vanilla SE census

22,047 NIFs in `Meshes0/1`:

| Blocks | Files |
| --- | --- |
| `NiFloatInterpolator` / `NiFloatData` | 2,158 / 1,995 |
| `NiTransformInterpolator` / `NiTransformData` | 1,733 / 1,684 |
| `BSEffectShaderPropertyFloatController` | 1,355 |
| `NiControllerManager` (sequences) | 1,274 (4,758 sequences) |
| `NiBoolInterpolator` (visibility) | 1,055 |
| `NiParticleSystem` | 789 (1,704 systems) |
| `BSLightingShaderPropertyFloatController` | 614 |
| `BSEffectShaderPropertyColorController` | 417 |

Rotation keys are XYZ Euler in 11,173 of 16,465 `NiTransformData`; float keys
are mostly Hermite (30,571 quadratic, 3,710 linear, 23 TBC). The commonest
sequence names are `Open`, `Close`, `mIdle`, `mLoop`, `AutoPlay`, `Idle`.

After conversion: 9,692 clips, 104,353 channels, 1,704 particle systems.

## Node extras

`bethconv.id` (the node's index in the converter's model) on every node a
channel or particle system names; engines rename nodes on import, so channels
refer to this id. `bethconv.hidden: true` for NiAVObject flag bit 0: not
drawn until a controller shows it.

## `bethconv.animations`

On the root node (next to `source`), a list of clips:

```json
{"name": "Open", "autoplay": false, "cycle": "clamp", "frequency": 1, "phase": 0,
 "start": 0, "stop": 0.6, "text_keys": [[0.6, "end"]],
 "channels": [{"node": 4, "property": "rotation_z", "interp": "linear",
               "components": 1, "times": [0, 0.6], "values": [0, -0.314]}]}
```

- Controllers outside a `NiControllerManager` run by themselves in the game.
  They become unnamed clips, grouped by clock (cycle, frequency, phase, start,
  stop, active flag). `autoplay` is the controller's active bit.
- Each `NiControllerSequence` becomes a named clip. Transform links are
  resolved by node name; shader and alpha controllers by the property they
  target.
- `interp` is `step`, `linear` or `cubic`. Cubic keys carry `in`/`out`
  tangents in Gamebryo's per-segment units: `v(s) = h00 v0 + h10 out0 + h01 v1
  + h11 in1` for `s` in [0, 1] between two keys. TBC keys are converted to
  such tangents; quaternion keys are always slerped.
- A transform interpolator without keys gives a one-key `step` channel with
  its own pose.

Properties: `translation` (3), `rotation` (quaternion xyzw), `rotation_x/_y/_z`
(radians, applied X, then Y, then Z), `scale`, `visible`, `alpha_test_ref`
(0-255), `effect.<var>` and `lighting.<var>` for the shader controllers
(`emissive_multiple`, `alpha`, `u_offset`, `v_offset`, `u_scale`, `v_scale`,
`falloff_*`, `emissive_color`, `glossiness`, `specular_strength`,
`specular_color`, `environment_map_scale`, `refraction_strength`),
`particles.birth_rate`, `particles.active`, `particles.speed`,
`particles.radius`, `particles.life_span`, `particles.gravity_strength`.

## `bethconv.particles`

On the root node, one entry per `NiParticleSystem`: its `node`, `material`
(the same block as a material's extras), `world_space`, `max_particles`,
`strip`, and:

- `emitters`: `kind` (`box`, `sphere`, `cylinder`, `mesh`), the emitter
  object's `node`, `size` (box width/height/depth, sphere radius, cylinder
  radius/height), for mesh emitters the `meshes` whose vertices emit,
  `mesh_velocity` and `mesh_axis`; speed, declination, planar angle, colour,
  radius and life span, each with its variation; `birth_rate`, the rate the
  system starts with (the standalone emitter controller's first key, else the
  highest any sequence gives it).
- `gravity`: `node`, `axis`, `strength`, `decay`, `spherical`, `turbulence`,
  `turbulence_scale`, `world_aligned`.
- `rotation`, `scales` (BSPSysScaleModifier, 60 values per second of age),
  `grow_time`/`fade_time`, `simple_color` (three colours with stops and fade
  in/out, as fractions of the lifetime), `color_keys`, `subtex` and
  `subtex_offsets` (rectangles as u, width, v, height).
- `drags`: every NiPSysDragModifier as `node`, `axis` (zero for all
  directions), `percentage`, `range`, `range_falloff`. Vanilla systems carry
  one per axis (X, Y, Z). `drag`/`drag_range` repeat the last one.
- `unsupported`: modifier types present but not converted.

## Not converted

Counted as warnings (`<type> xN: not converted`), SE files:
`NiPSysBombModifier` 228, `BSLagBoneController` 108,
`BSPSysInheritVelocityModifier` 103, `NiFloatExtraDataController` 71,
`BSFrustumFOVController` 54, `NiPSysColliderManager` 50,
`BSPSysRecycleBoundModifier` 36, `NiPathInterpolator` 23,
`BSProceduralLightningController` 13, `BSWindModifier` 7,
`NiLookAtInterpolator` 5, and a handful of rarer ones. Strip particle systems
(148 files, trails) are kept as plain particles. `NiPSysSpawnModifier`
(spawn on death) is ignored silently.
