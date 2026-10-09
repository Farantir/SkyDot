# Effects

How the engine plays what the converter writes into a model's extras (see
`../converter/docs/format-notes/nif-animation.md`). `SkydotWorld` attaches all
of it when it places a model; `effects = false` (viewer: `--effects off`)
leaves models and lights still.

## Clips (`SkydotAnimator`)

- Unnamed clips follow the application clock, so every copy of a model moves
  in step, as in the game. Their shader variables go to the material all
  copies share, written once per frame.
- Named clips play on request (`play("Open")`); the first of `AutoPlay`,
  `AutoLoop`, `Idle`, `mIdle`, `SpecialIdle`, `mLoop` starts on load. A model
  that a named clip recolours gets its own material copy.
- Signals: `text_key(clip, key)` as a sequence passes a text key,
  `finished(clip)` when a clamped one ends.
- Models more than 250 m from the camera hold still.
- A skinned model's animated nodes are bones once imported (the cairn
  banners); their ids come from the bone meta and clips pose the bones.
- Euler keys compose X, then Y, then Z; checked against rest poses (door,
  chest, fire, waterwheel), which equal their first or last keys.

## Particles (`SkydotParticles`)

One `GPUParticles3D` per emitter under the system's node, with one process
shader for all (`particles_process.gdshader`) and the effect
shader's particle variant for drawing (camera-facing quads, spin, sub-texture
rectangles). Built on entering the tree, since emitter, gravity and particle
space depend on global transforms. Amount is the peak birth rate times the
longest life, capped by the NIF's maximum; the emission channel scales
`amount_ratio`.

Guesses, not checked against the game: speed, radius and life vary by
±variation/2, angles by ±variation; spherical gravity pushes away from its
object; drag removes `drag` of the velocity per second; turbulence is random
acceleration re-rolled eight times a second.

## Effect shader

- Effect_Lighting (shader flags 2, bit 30) draws the effect lit by the
  scene's lights, wrapped so the far side is dim rather than black; mountain
  clouds and blowing snow take the weather's light this way.
- Additive effects write their own fog (`SkydotMaterials.sync_fog`, the
  `skydot_fog` global): Godot's fog would turn their black, transparent parts
  fog-coloured and add them, so every particle was a square far away.
- Multiplying shapes (NiAlphaProperty source ZERO and destination
  SRC_COLOR, or DEST_COLOR and ZERO) darken what is behind by their colour:
  the contact shadows under clutter, doors and Nordic halls, and gems. Fog
  fades them towards white. Drawn with the mix blend they were white rings.
- Falloff angle controllers key degrees; the property holds cosines.
- Particles sampling one atlas cell stop at the mip where the cell is 16
  texels wide, below which the game's mips blur the cells together.

## Lights (`SkydotFlicker`)

LIGH flicker flags dim the light below its fade by up to the intensity
amplitude (in fade units, so relative depth = amplitude / fade) over the
flicker period (smooth noise), and move the light within a 64th of the
movement amplitude of its place (about 1 cm for a torch; the game's scale is
unknown, see render/flicker.cpp); pulse flags use a sine. LIGH stores 1/period:
the Whiterun street fires' 0.05 is a 20 s cycle with amplitude 1 below fade 3,
so they drift between 67% and 100% (the game's wall next to one varied by
about 5% over 2 s). A stored 0 becomes 0.2 s, or 1 s for the slow flags. The
flicker runs on engine time, so a `--fixed-fps` frame series shows it at its
real speed.

## Lights that follow a weather (`SkydotEmittance`)

A light reference with XEMI (7,045 in Skyrim.esm) names a region, and the light
takes the Effect Lighting colour (NAM0 index 9) of that region's weather: its
own colour times it, looked up twice a second at the running time of day. The
vanilla street fires and window lights use the `FXWthrInvert*` weathers. Their
sunlight is black by day and white at night, but that is not what the game
uses: the Whiterun brazier's light, disabled and enabled in the game
(`game-refs-2026-10-11`, `brazier_toggle.txt`), lights the facade by day as well
as at night, dimmer and more orange than its own colour. FXWthrInvertLightsWhiterun's
Effect Lighting (83/60/34 by day, 238/184/70 at night) reproduces what it adds
by day within a few levels; at night SkyDot's is about 1.3x the game's. The
light's own colour alone (no XEMI) was 3x too bright by day. Without a running
weather (interiors) the light is left as it is. Needs a pack of world.fb
format 12.

## Cost

Riverwood flythrough (5x5 cells, debug build of the extension): median frame
0.9 ms without effects, 1.3 ms with; the difference is almost all clips.
