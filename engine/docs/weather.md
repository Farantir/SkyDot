# Weather

`SkydotWeather` (`render/weather.*`) runs an exterior's sky: time of day,
weather choice and fades, sky colours, light, fog, clouds, sun, moons, stars,
rain and snow, lightning. The viewer adds one outside; interiors keep their
own lighting.

## Time and choice

- `hour` advances at `time_scale` game seconds per real second (the game's
  default 20; the viewer's `--time-scale`, 0 in screenshots and benchmarks);
  midnight advances `day`.
- The weather comes from the region the camera stands in: the REGN with a
  weather list whose polygon contains it, highest priority first, of this
  worldspace or its parent. Elsewhere the climate's list. A weather is drawn
  by its chance. Gating globals and the override flag are not used.
- With `auto_weather`, a new weather is drawn every 4 to 10 game hours, and
  when the camera walks into a region that does not offer the current one.
  `set_weather` and `next_weather` change it at once.
- A change fades over 10 / transition delta seconds (DATA; about 20 s for
  vanilla weathers; a guess). Colours, fog and cloud layers cross-fade;
  precipitation fades out by the old weather's end point and in from the new
  one's begin point.

## Drawing

- Sky: a gradient shader from the weather's sky upper, horizon and lower
  colours. Ambient light, the directional light (sun by day, moon at night;
  sunlight colour) and depth fog come from the same table.
- Clouds: `meshes/sky/clouds.nif` follows the camera; its 29 shapes are the
  29 layers, each with a shader cross-fading the old and new weather's
  texture, time-of-day colour and alpha, scrolled by the layer's speed
  (0.02 UV per second at full speed: a guess). The dome's vertex colours
  are (1, 0, 0, fade); only the alpha is used.
- Sky objects are drawn at the far plane, so the world hides them: stars
  (the climate's model; the data's star colour times 0.3 and the square of
  the darkness, a guess), the sun and its glare (the weather's sun colour and
  glare), Masser and Secunda at night with their phase every phase length
  days. Draw order: stars, moons, sun, glare, then clouds over them.
- Rain and snow: GPU particles in a box around the camera, from the SPGD:
  speed, count from density and box size, atlas frames, rotation, wind from
  DATA. Sizes are guesses (ten game units per unit for snow, forty for
  rain, whose texture is a one-pixel streak per frame). Rain turns about the
  vertical only.
- Lightning: rainy weathers with a thunder frequency below 255 flash every
  4 to 44 s (a guess from that frequency), brightening the sky, clouds and
  ambient in the lightning colour; signal `lightning`.

## Not done

- No game-frame comparison: star brightness, cloud speed, rain and snow
  size, fade times and lightning rate are guesses.
- Thunder sound, weather sounds, image spaces, volumetric lighting, sky
  statics, the aurora and sun damage are not used.
- Water does not reflect the sky; clouds cast no shadows.
- The sun's path is a fixed arc east to west; the moons follow the night arc.

## Fog (2026-10-03)

Our shaders (lit, effects, terrain, LOD, water) compute the game's fog
themselves and write it to `FOG`, which replaces Godot's depth fog:
`min(max, ((d − near)/(far − near))^power)`, coloured from the near to the
far fog colour by the same ramp (`with_game_fog`, render/materials.cpp). Godot's
own depth fog uses a smoothstep and one colour; with the far colour alone,
nearby fog whitened everything. The values come through the Environment
(depth begin/end/curve/density, the far colour as fog light colour, the near
colour as its `skydot_fog_near_color` meta) via `SkydotMaterials.sync_fog`;
interiors use their XCLL fog the same way. The formula is from memory of the
game's shader, not measured: weathers with a low power (SkyrimCloudySN 0.35)
stay hazy close by. Needs an in-game comparison in the same weather.
