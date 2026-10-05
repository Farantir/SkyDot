# Shaders

The engine's shaders, as text files. Godot's own shader preprocessor
(`#include`, `#define`, `#ifdef`, `#if`) puts them together; the C++ in
`extension/src/render/` only chooses which variant a material needs
(`shader_source.*`) and keeps the compiled `Shader`s.

The files are Godot resources in `res://shaders/`, so the editor opens them in
its shader editor and Godot loads them like any other shader. There is no
import step, so they work in headless runs, and in an exported game they are
in the `.pck` (see "Exports"). Restart to see an edit.

## Three kinds of shader

- **Whole shaders** (`.gdshader`) with no variants: `shader_source::shader()`
  loads the file, and Godot caches the `Shader` by path. They `#include` what
  they share (`game_fog`, `far_plane`, `lod_mask`, `sprite`).
- **Variants** of one fragment (`.gdshaderinc`): the C++ gives Godot a small
  stub, and the fragment's `#ifdef`s and `#if`s pick the render modes and
  code. `shader_source::variant()` writes it:

  ```
  shader_type spatial;
  #define SKYDOT_DOUBLE_SIDED
  #define SKYDOT_ALPHA_TEST
  #include "res://shaders/lighting.gdshaderinc"
  ```

  A fragment has no `shader_type` of its own (a shader has one, and the stub
  gives it), and the C++ keeps a `Shader` for each distinct stub.
- **Compute shaders** (`.comp`) for the image space: GLSL for
  `RenderingDevice`, which has no Godot preprocessor. `shader_source::load()`
  reads the text; it is not `.glsl`, which the editor would import as an
  `RDShaderFile`.

## Files

| File | What | Made by |
| --- | --- | --- |
| `lighting.gdshaderinc` | BSLightingShaderProperty: `SKYDOT_DOUBLE_SIDED`, one of `SKYDOT_ALPHA_TEST`, `_BLEND`, `_MUL` | `lighting_code()`, `materials.cpp` |
| `effect.gdshaderinc` | BSEffectShaderProperty: `SKYDOT_DOUBLE_SIDED`, `SKYDOT_PARTICLES`, `SKYDOT_LIT`, one of `SKYDOT_ALPHA_TEST`, `_ADD`, `_MUL`, `_BLEND` | `effect_code()`, `materials.cpp` |
| `refraction.gdshaderinc` | refraction (screen-space distortion): `SKYDOT_DOUBLE_SIDED` | `refraction_code()`, `materials.cpp` |
| `terrain.gdshaderinc` | terrain layers: `SKYDOT_LAYERS` 1 to 7 | `TerrainBuilder::shader_code()`, `terrain.cpp` |
| `game_fog.gdshaderinc` | the game's fog and directional ambient, included by all of the above and by water and LOD | the shaders |
| `water.gdshader` | water | `WaterMaterials::shader()`, `water.cpp` |
| `lod_terrain`, `lod_object`, `lod_water`, `lod_tree` `.gdshader` | LOD terrain, objects, water, trees | `SkydotLod`, `lod.cpp` |
| `lod_mask.gdshaderinc` | discard over loaded cells, included by the LOD shaders | the LOD shaders |
| `sky.gdshader`, `clouds.gdshader`, `precipitation.gdshader` | weather | `SkydotWeather`, `weather.cpp` |
| `sprite_add.gdshader`, `sprite_mix.gdshader`, `sprite.gdshaderinc` | stars, sun and moons: two blend modes of one body | `SkydotWeather`, `weather.cpp` |
| `far_plane.gdshaderinc` | sky objects sit just in front of the far plane, included by clouds and sprites | the shaders |
| `particles_process.gdshader` | the process shader of every particle system | `SkydotMaterials::particles_process_shader()` |
| `image_space_reduce.comp`, `image_space_adapt.comp`, `image_space_grade.comp` | the image space passes (GLSL 450 compute) | `compile()`, `image_space.cpp` |

## Conventions

- **Header comment.** Every file starts with a `/* ... */` block that says
  how it is used: which C++ makes it, and the defines a variant takes. Keep it
  true when the C++ changes. Godot's preprocessor drops comments, so it does
  not reach the shader; the loader cuts it from `.comp` files, which Godot
  does not preprocess.
- **Defines** are `SKYDOT_` plus a name: the preprocessor has one namespace
  across every include. The C++ chooses them; the render modes follow from
  them in the fragment (`#ifdef SKYDOT_DOUBLE_SIDED`, then `render_mode
  cull_disabled;`, else `cull_back`).
- **Fog and ambient** are in the shader that wants them, not added by the C++.
  It includes `game_fog.gdshaderinc`, ends `fragment()` with
  `FOG = skydot_game_fog(VERTEX);` (the effect shader writes its own `FOG`)
  and, if it is lit, adds `ambient_light_disabled` to its render modes and
  ends with `EMISSION += ALBEDO * skydot_ambient(...)`, so the game's
  ambient replaces Godot's. The globals these read must exist before the
  shader compiles: `shader_source` calls `SkydotMaterials::ensure_fog_globals()`
  for everything it makes.
- **Includes** are `#include "res://shaders/NAME"`, always the full path.
- **Several `render_mode` lines** are fine, in a shader and in an include,
  under `#ifdef`s too: their modes add up. Godot does not complain when two of
  one kind (say `cull_back` and `cull_disabled`) meet, so a variant's `#ifdef`
  must choose one of them, as the cull mode is chosen in `lighting.gdshaderinc`.
- **No loops or token pasting**, so what C++ used to generate per variant is
  written out under `#if`: `terrain.gdshaderinc` declares the samplers and mixes
  of each of its seven layers in `#if SKYDOT_LAYERS > n` blocks.
- **Tabs**, as the shader editor writes them (`.editorconfig`).

## Changing a shader

Edit the file and start the game. Three checks, each of which finds something
the others cannot:

- `smoke_shaders` (ctest): every shader the engine can produce is listed, each
  file it includes exists, and Godot's preprocessor and shader compiler log no
  `SHADER ERROR` for any of them. It runs headless, where the dummy renderer
  parses and type-checks the shaders but does not compile them for a GPU.
- `godot --path game --script res://tools/shader_compile.gd` (needs a window):
  gives every shader to the real renderer, draws each, and exits with the
  number that logged an error. Run it after changing what a variant selects.
- A re-render. `SkydotMaterials.shader_sources()` returns a stub, or a
  `.gdshader`'s text with its includes unexpanded, so `tools/shader_dump.gd`
  hashes only which variants exist and what they define, not what Godot makes
  of them. Whether a change left the picture as it was, only pictures show:
  render the same `--from-shot` views before and after
  (`godot --path game res://viewer/cell_viewer.tscn -- --from-shot SHOT.json
  --screenshot OUT.png --no-input`, `--no-input` last) and compare the pixels.

## Exports

`.gdshader` and `.gdshaderinc` are resources, so Godot exports them with the
other resources. Nothing in a scene refers to the `.gdshaderinc` files (the
C++ names them in the stubs it builds), so an export preset that exports only
selected scenes and their dependencies must list `shaders/*.gdshaderinc` in
its resources. `.comp` files are not a resource type: an export preset must
list `shaders/*.comp` under "Filters to export non-resource files/folders", or
the image space passes will not compile in the exported game. The repository
has no export preset yet.
