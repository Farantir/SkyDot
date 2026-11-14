# Sound

What the game data says about sound, what world.fb carries (format 42) and how
the game layer (`game/game/sounds.gd`) plays it. Decoding of the files: ADR 0005.
Census numbers are Skyrim.esm (SE).

## Records

### SNDR, sound descriptor (2453)

| Field | Meaning | world.fb |
|---|---|---|
| CNAM | Descriptor type. 0x1EEF540A (standard) on all 2453; no compound descriptor exists in the vanilla data | `descriptor_type` |
| GNAM | The SNCT category | `category` |
| SNAM | "Alternate Sound For": this descriptor stands in for that one when its conditions pass (202 of them: footsteps on water, items, impacts on metal skin) | `alternate_for` |
| ANAM | Files, relative to `Data\Sound` (with or without the prefix); one is picked at random | `files` |
| ONAM | The SOPM output model (all 2453) | `output_model` |
| CTDA | Conditions, run on the source reference (363 CTDAs: GetCurrentTime 308, IsInInterior 28, GetIsRace 16, ...). The tavern loop `AMBTavernInteriorA2DLPSD` plays only at hour <= 2 or >= 20 | `conditions` |
| LNAM | 4 bytes: byte 0 undocumented (0, 1, 2, 4), byte 1 looping (0 none, 0x08 loop, 0x10 envelope fast, 0x20 envelope slow), byte 2 undocumented, byte 3 rumble send value | `lnam_unknown0`, `looping`, `lnam_unknown2`, `rumble` |
| FNAM | 4 bytes on 63 descriptors, undocumented | `fnam` |
| BNAM | Frequency shift and variance (percent, signed), priority (64, 128, 192, 255 ...), dB variance, static attenuation (hundredths of a dB) | `frequency_shift`, `frequency_variance`, `priority`, `db_variance`, `static_attenuation` |

### SOPM, output model (69)

| Field | Meaning | world.fb |
|---|---|---|
| NAM1 | Byte 0 flags (bit 0 attenuates with distance, bit 1 allows rumble: the player's first/third person models), byte 3 reverb send percent (0x14..0x64) | `flags`, `reverb_send` |
| MNAM | 0 uses HRTF: a mono source placed in 3D; 1 defined speaker output: the channels go to the speakers by ONAM, the distance only scales the volume (the `SOMStereo*` models, the dialogue 2D models, UI) | `type` |
| ONAM | 24 bytes: 3 input rows (mono, stereo left, stereo right) of 8 speaker percentages (FL, FR, C, LFE, RL, RR, SL, SR). `SOMStereoRad*`: mono to FL/FR 100 and the four rear/side speakers 50, left to FL/RL/SL, right to FR/RR/SR | `speaker_output` |
| ANAM | 20 bytes: 4 undocumented, min and max distance (float, game units), five curve bytes, 3 undocumented | `min_distance`, `max_distance`, `curve`, `anam_unknown` |
| FNAM, CNAM, SNAM | Only on the 4 records of form version 30 (the `*Linear` models and `SOMStereoRad09000DragonCrashLand`), which have no NAM1: FNAM is taken as the flags (1 on all four), SNAM as 2 speaker rows, CNAM (2) is not read | `flags`, `speaker_output` |

The curve is five volume percentages at evenly spaced distances from min to
max. Most models use 100, 50, 20, 5, 0; dialogue 100, 75, 50, 25, 0; some
distant sounds start at 0 (`SOMMono10000_dryDistant` 0, 50, 100, 50, 0: silent
near, loudest at half range).

### SNCT, sound category (18)

| Field | Meaning | world.fb |
|---|---|---|
| FNAM | Bit 0 mute when submerged, bit 1 should appear on menu (a user slider) | `flags` |
| PNAM | Parent category | `parent` |
| VNAM | Static volume multiplier, u16 / 65535 | `static_volume` |
| UNAM | The slider's default, u16 / 65535 (only on the 4 menu categories) | `default_menu_volume` |

A sound's category volume is the product of the static multipliers up the
chain times every menu slider on it. The tree (static volume, menu default):

```
_AudioCategoryMaster 0.9
  AudioCategoryPausedDuringMenuLoad 1
    AudioCategorySFX 0.9 [menu, default 1]
      AudioCategoryUI 1
      AudioCategorySFXFadeDuringDialogue 1
        AudioCategoryPausedDuringMenuFade 1
          AudioCategoryAMB 1, AudioCategoryAMBr 1, AudioCategoryMuteSubmerged 1,
          AudioCategoryMAG 1 (AMB, AMBr, MuteSubmerged: mute when submerged)
        AudioCategoryFST 1 [menu, default 0.8]
        AudioCategoryFSTnpc 0.7
        AudioCategoryNPCKillMove 0.9
  AudioCategoryPausedDuringMenu 1
    AudioCategoryVOCGeneral 0.9 [menu, default 0.8]
      AudioCategoryVOC 1, AudioCategoryNonVOC 1
  AudioCategoryMUS 0.7 [menu, default 0.5]
```

So an ambient loop plays at 0.9 x 0.9 = 0.81 (-1.8 dB) of its file level after
its static attenuation, an NPC footstep at 0.57 (-4.9 dB), dialogue at 0.65.

The menu sliders are in SkyrimPrefs.ini `[AudioMenu]` (`uIDn` the category,
`fValn` the value; the defaults equal UNAM).

### SOUN, sound marker (997)

SDSC is the descriptor. The old inline sound (FNAM file name, SNDD 36 bytes,
on 659 markers) is not read by the game's Skyrim path and not by us. Placed
markers play their descriptor at their position.

### ASPC, acoustic space (45)

| Field | Meaning | world.fb |
|---|---|---|
| SNAM | Ambient loop (a SNDR) | `ambient` |
| RDAT | A region whose sounds play inside (sound-only regions such as `AudioIntDungeonCave01`) | `region` |
| BNAM | The REVB reverb | `reverb` |
| OBND | Editor bounds, not used | - |

An interior's acoustic space is CELL XCAS (542 interiors, 114 exterior cells,
none of them in Tamriel's towns). Placed ASPC references (125, 21 in
worldspaces) carry an XPRM box (half extents, as triggers): inside it, that
space replaces the cell's.

### REVB, reverb (10)

DATA 14 bytes: decay time (ms, u16), HF reference (Hz, u16), room filter, room
HF filter, reflections, reverb amplitude (i8 each), decay HF ratio (x 100),
reflect delay (scaled), reverb delay (ms), diffusion %, density %, unknown.
world.fb `reverbs` by name.

### REGN sounds (60 regions with RDAT type 7)

RDSA entries of 12 bytes: SNDR, flags (bit 0 pleasant, 1 cloudy, 2 rainy, 3
snowy: the weather classes it plays in), chance. Loops have chance 1; one-shots
(birds, gusts) 0.01..0.1. RDAT's priority and override flag pick between
overlapping regions. world.fb `Region.sounds`, `sound_priority`,
`sound_override`. Riverwood is in `WeatherPineForest` (priority 50, 60
sounds: birds by time of day through their own conditions, the wind bed
`AMBrWindBedForestPineLPSD`, crickets at night).

### WTHR sounds (30 weathers)

SNAM 8 bytes each: SNDR and type (0 default, 1 precipitation, 2 wind, 3
thunder). world.fb `Weather.sounds`. The clear weathers have none.

### FSTS, FSTP, footsteps

FSTP (116): DATA an IPDS, ANAM the animation tag (FootLeft, FootRight,
FootSprintLeft, FootJumpLand ...). FSTS (35): XCNT the five counts, DATA the
FSTPs of walk, run, sprint, sneak and swim one after another. ARMA SNDD names
the set; the sound is the IPDS entry for the material under the foot (IPCT
sound). world.fb `footsteps`, `footstep_sets`, `ArmorAddon.footstep_set`.

## Queries

`SkydotWorld`: `get_sound_descriptor`, `get_sound_output_model`,
`get_sound_category`, `get_acoustic_space`, `get_reverb`,
`get_sound_alternates`, `get_region_sounds(world, x, y)`,
`get_region_sound_data(region)`, `get_weather_sounds`, `get_footstep_set`,
`get_footstep`; `get_cell` has `acoustic_space`. `SkydotPapyrus.
sound_conditions_met(sound, ref)` runs a descriptor's conditions.

## Playing (game/game/sounds.gd, game/game/ambience.gd)

- **Choosing the descriptor.** A descriptor asked for gives way to the first
  of its alternates (SNAM points at it) whose conditions pass, recursively;
  one whose own conditions fail does not play. A loop whose conditions fail
  starts silent and fades in when they pass; every looping sound's conditions
  are asked again every 2 s and it fades over 2 s (both estimated). Ambient
  regions' birds and insects are gated so (GetCurrentTime, and Dawnguard's
  GetIsCurrentWeather on the eclipse weather).
- **Level.** File level - static attenuation - dB variance + the category
  (static volumes up the SNCT chain, times the menu sliders:
  `Sounds.category_volumes`, defaults UNAM) + the output model:
  - HRTF (MNAM 0), attenuating, with a source: an AudioStreamPlayer3D on the
    source, Godot's distance attenuation and air-absorption filter off, the
    SOPM curve on top, and `Sounds.HRTF_DB` (-10 dB). Measured with the
    skycompare sound series (2026-11-14): the game plays these sounds about
    12 dB below SkyDot's level before (the waterwheel loop at its min distance
    -43.7 dB RMS against -30.9, the Bannered Mare door -38.5 against -27.3);
    the category is 1.8 dB of that.
  - Defined speaker output (MNAM 1): an AudioStreamPlayer at the front pair's
    level of its speaker rows; attenuating, with a source, the curve still
    scales it by distance (the "2D" water, wind and dungeon loops). The
    Bannered Mare interior loop (SOMStereo_verb) plays within 3 dB of its data
    in the game.
  - Godot's air-absorption filter took up to 10 dB more off far sounds than
    the curve; the game follows the curve (the waterwheel series: -1, -4, -9 dB
    at 240, 440, 640 units against the curve's -1, -4, -7).
- **Looping.** 0x08 loops between the file's sampler (smpl) loop points (the
  whole file in all vanilla loops); the envelopes 0x10/0x20 ("LPM" files:
  `mag_flames_fire_lpm.wav` loops frames 12862..72839 of 135251) play their
  start, loop, and when stopped finish the pass and play the tail (XAudio2's
  ExitLoop). Fast and slow are not told apart.
- **Markers.** A cell's SOUN references start when the cell is built; loops
  are held out of range and stop with the cell. A one-shot marker plays once.
- **Ambience** (game/game/ambience.gd), checked every second:
  - the acoustic space: the innermost placed ASPC box holding the listener,
    else the cell's XCAS (an exterior cell's where the listener stands). Its
    ambient loop plays non-positionally, its region's sounds while inside;
  - exteriors: the regions with sounds whose areas hold the listener, by
    priority, down to the first with the override flag; an RDSA entry plays
    when its weather flags hold the current weather's class (an interior's
    region always). Loops play while that holds, one-shots are rolled each
    second with their chance and play at a random point around the listener,
    between the min distance and halfway to the max (estimated);
  - the weather's SNAM loops (types 0-2); thunder (3) is not played.
  - Everything that starts or stops fades over 2 s (estimated), so walking
    between spaces and regions crossfades.
- **Range setting.** `--sound-range FACTOR` (0.25..8, default 1 = the game's;
  `ViewerSettings.sound_range` -> `GameConfig.sound_range` ->
  `Sounds.range_scale`) multiplies every output model's min and max distance,
  region one-shot distances too.
- **Not done:** reverb (REVB, SOPM reverb send), priority and voice limits,
  loudness culling, rumble, footsteps (data in world.fb 42: FSTS/FSTP/ARMA
  SNDD; needs the material under the foot), thunder, music (W7), dialogue
  voices' categories and output models (speech.gd keeps Godot's inverse
  distance).
