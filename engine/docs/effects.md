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

LIGH flicker flags vary brightness by the intensity amplitude over the flicker
period (smooth noise) and move the light within a 64th of the movement
amplitude of its place (about 1 cm for a torch; the game's scale is unknown,
see render/flicker.cpp); pulse
flags use a sine. Periods of 0 become 0.2 s, or 1 s for the slow flags.

## Lights that follow a weather (`SkydotEmittance`)

A light reference with XEMI (7,045 in Skyrim.esm) names a region, and the light
takes the sunlight colour of that region's weather: its own colour times it,
looked up twice a second at the running time of day. The vanilla street fires
and window lights use the `FXWthrInvert*` weathers, whose sunlight is black by
day and sunrise, 70% at sunset and white at night, so they burn from dusk and
are off by day (the Whiterun brazier at 13:12 lit nothing in the game's frame;
the converter dropped XEMI, so SkyDot lit the facade white). The cave
weathers carry coloured sunlight and tint their lights. Without a running
weather (interiors) the light is left as it is. Needs a pack of world.fb
format 12.

## Cost

Riverwood flythrough (5x5 cells, debug build of the extension): median frame
0.9 ms without effects, 1.3 ms with; the difference is almost all clips.
