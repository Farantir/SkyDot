# Shaders

The engine's shaders, as text files. The C++ in `extension/src/render/` reads
them (`shader_source.*`), adds what differs per variant (defines, render
modes, the game's fog and ambient, per-layer lines) and gives the result to
Godot. Before, they were raw strings in that C++.

The files are read as plain text from `res://shaders/` through `FileAccess`,
once per process. There is no import step, so they work in headless runs, and
in an exported game they are in the `.pck` (see "Exports"). Restart to see an
edit.

## Files

| File | What | Assembled by |
| --- | --- | --- |
| `lighting.gdshaderinc` | BSLightingShaderProperty | `lighting_code()`, `materials.cpp` |
| `effect.gdshaderinc` | BSEffectShaderProperty | `effect_code()`, `materials.cpp` |
| `refraction.gdshaderinc` | refraction (screen-space distortion) | `refraction_code()`, `materials.cpp` |
| `game_fog.gdshaderinc` | the game's fog and directional ambient | `with_game_fog()`, `materials.cpp` |
| `terrain.gdshader` | terrain layers, one variant per layer count | `TerrainBuilder::shader_code()`, `terrain.cpp` |
| `water.gdshader` | water | `WaterMaterials::shader_code()`, `water.cpp` |
| `lod_mask.gdshaderinc` | discard over loaded cells, shared by the LOD shaders | `lod_code()`, `lod.cpp` |
| `lod_terrain`, `lod_object`, `lod_water`, `lod_tree` `.gdshaderinc` | LOD terrain, objects, water, trees | `lod_code()`, `lod.cpp` |
| `sky.gdshader`, `clouds.gdshader`, `sprite.gdshader`, `precipitation.gdshader`, `far_plane.gdshaderinc` | weather | `make_shader()`, `weather.cpp` |
| `particles_process.gdshader` | the process shader of every particle system | `SkydotParticles::process_shader_code()`, `particles.cpp` |
| `image_space_reduce.comp`, `image_space_adapt.comp`, `image_space_grade.comp` | the image space passes (GLSL 450 compute) | `compile()`, `image_space.cpp` |

## Conventions

- **Header comment.** A file may start with a `/* ... */` block that says
  which C++ assembles it and with what (what comes before it, what is
  inserted into it). The loader cuts that block and the newline after it, so
  the header is not part of the shader and the code string is the same as
  without it. A file without a header is taken whole. Keep the header true
  when the C++ changes.
- **Extensions.** `.gdshader` starts with `shader_type`: a whole shader that
  the C++ uses as it is or fills in. `.gdshaderinc` is a fragment the C++
  puts after other text (`shader_type`, `render_mode`, `#define` lines). Both
  are Godot resources the editor opens with its shader editor, which flags
  what the C++ completes. `.comp` is GLSL for `RenderingDevice`; it is not
  `.glsl`, which the editor would import as an `RDShaderFile`.
- **Markers.** A line `%NAME%` is replaced by the C++: `%LAYER_SAMPLERS%` and
  `%LAYER_MIXES%` in `terrain.gdshader`, `%FAR%` in `clouds.gdshader` and
  `sprite.gdshader`. `%BLEND%` in `sprite.gdshader` is replaced inside its
  `render_mode` line.
- **Lines the C++ inserts.** `with_game_fog()` adds `game_fog.gdshaderinc`
  after the `render_mode` line and, before the closing brace of `fragment()`,
  `FOG = skydot_game_fog(VERTEX);`. `with_game_ambient()` adds
  `ambient_light_disabled` to the render modes and, in the same place, an
  `EMISSION +=` line. Those two lines are in the C++, next to the search that
  finds where they go: the first `void fragment()` and its matching brace, so
  keep that spelling.
- **Tabs**, as the shader editor writes them (`.editorconfig`).

## Changing a shader

Edit the file and start the game. If the change is meant to leave the
shaders as they are (moving, renaming, splitting a file), prove it:
`godot --headless --path game --script res://tools/shader_dump.gd -- --out
before.txt` before and after, then diff the two. It lists a hash for every
shader the engine can produce, which `tools/scene_dump.gd` cannot reach (sky,
clouds, LOD, particles, image space). The smoke test `smoke_shaders` fails if
a file is missing (the loader reports it with `push_error` and the shader is
empty), a marker is left, or a file lost its header.

## Exports

`.gdshader` and `.gdshaderinc` are resources, so Godot exports them. `.comp`
files are not a resource type: an export preset must list `shaders/*.comp`
under "Filters to export non-resource files/folders", or the image space
passes will not compile in the exported game. The repository has no export
preset yet.

## Next step: Godot's own includes

The assembly above is string concatenation that a variant of
`#include "res://shaders/game_fog.gdshaderinc"` plus `#define`s (and
`#ifdef`s for the render modes) could do in Godot's shader preprocessor, as
the code review proposes. That changes the code strings (the preprocessor
splices the text itself), so `shader_dump` cannot vouch for it: only
rendering can. It would be verified by re-rendering `--from-shot` shots
before and after, with pixel differences of zero.
