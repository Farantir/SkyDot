# Pack format v7

The interface between converter and engine. The converter writes packs; the
engine reads only packs. Everything below is stable in v7. The last section
lists what is not promised.

Later v7 packs (sound fingerprint `sound/2`) also hold voices and music as `sound` assets, as Ogg Vorbis (see "Sound assets"); engines that read sound assets must sniff the first bytes (`RIFF` WAV, `OggS`, `SDVC`), not trust the extension. v7 adds sound effects: every `.wav` (16-bit PCM, passed through unchanged) is an asset
of a new kind, `sound`, with extension `.wav`. v6 adds animations: every Havok file (`.hkx`) becomes an `.animfb` asset of
a new kind, `animation`, holding its skeletons and clips; later v6 packs add
behaviour graphs' clip generators and the animationdata text files (clip
ids, root motion) to the same kind, which older v6 packs lack. v5 stores assets in one blob with an index (or, on request, as loose files)
and drops glTF images from meshes: the engine loads packs directly, without a
Godot import or bake. v4 adds LOD: terrain and object LOD (`.btr`, `.bto`) become meshes, and
LOD settings and tree LOD (`.lod`, `.lst`, `.btt`) become `.lodfb` assets of
a new kind, `lod`. v3 decodes scripts: a script asset is a `.pexfb` FlatBuffer instead of the
original `.pex`. v2 added `world.fb` (cells, references and base objects,
decoded and with global FormIDs) and the manifest's `world` key.

v6 and v7 packs written by earlier converters may also contain `records.fb` and a
manifest `records` key; readers ignore both, whether or not the file is there.

## Overview

A pack is a directory. Assets are named by a hash of their source bytes;
`vpath.idx` maps game paths to those hashes. `world.fb` holds the load order
already merged into what the engine needs to build cells, so the engine never
sees plugins, mod indices or overrides. No file contains a timestamp, and the
same input gives a byte-identical pack.

## Versioning

`k_pack_format_version` (`converter/include/bethconv/pack/vpath_index.hpp`) is 7. It
appears in `manifest.json`, `report.json` and the `vpath.idx` header, and
`world.fb` carries a `format_version` of its own. Each is checked separately so
a reader can say which file it cannot read.

Readers refuse unknown versions; they never read what they recognize and skip
the rest. `WorldFile::open` returns `ErrorKind::unsupported` with both numbers.

Writers bump the version whenever the meaning of anything here changes, even if
parsers would not notice. Settings fingerprints (below) are separate: they
rename assets without changing the format.

## Layout

```
pack/
  manifest.json                      what the pack is and what built it
  world.fb                           cells, references, base objects
  assets.idx                         content hash -> offset, size, kind
  assets-0001.blob                   every asset (.glb, .dds, .pexfb, .lodfb, .animfb, .wav bytes)
  vpath.idx                          virtual path -> content hash
  report.json                        every skipped, failed and warned input
```

With `--store loose`, `assets.idx` and the blob are replaced by
`assets/<bb>/<hash><ext>`, one file per asset in a two-level fanout. The
manifest's `store` key says which; readers support both. An asset-less pack
has an index with no entries and an empty blob (or an empty `assets/`).

`world.fb` may be absent (asset-only pack). The `world` key in `manifest.json`
decides: if present, the file must exist and match its `hash`.

There is no `.ktx2`: the headless-bake spike showed DDS passthrough works (see
`converter/docs/spikes/headless-bake.md`).

## `manifest.json`

Keys in this order (`nlohmann::ordered_json`, two-space indent, trailing
newline):

| Key | Content |
| --- | --- |
| `pack_format_version` | `6` |
| `converter` | writer name and version |
| `language` | language used to resolve strings |
| `textures` | how textures were converted: `max_size` (largest side in pixels, 0 for full size; larger textures lost their top mip levels), `complete_mip_chains`, and `uncompressed` (`keep`, `bc7` or `compact`: what happened to textures the game stores uncompressed; BC7 files carry a DX10 header, format 98); absent when textures were not converted |
| `input` | optional: what the pack was converted from, so it can be converted again: `kind` (`data` or `mo2`), `edition`, `data` (the Data folder), `plugin_list`, and for `mo2` also `mo2_instance`, `mo2_profile`, `mods`. Local paths; nothing reads it but front ends |
| `load_order` | plugin filenames in order |
| `source_hashes` | per mounted archive or directory: `name`, `kind`, `bytes`, optional `hash` |
| `world` | `file`, `cells`, `refs`, `bases`, `bytes`, `hash`; absent for an asset-only pack |
| `assets` | `distinct`, `index_entries`, `meshes`, `textures`, `scripts`, `lod`, `animations`, `sounds`, `bytes` (written by this run), `dedupe_saved_bytes` |
| `store` | `layout` (`blob` or `loose`); for a blob also `index` and `blob` (file names); `bytes` (all stored assets) |
| `deferred` | extension → count of inputs not converted by this version |
| `report` | `file`, `failed`, `warnings` |

`load_order` is provenance only; the engine must not depend on it.

## `world.fb`

A plain FlatBuffer (identifier `BWD1`), schema `formats/schema/world.fbs`, with its
own `format_version` (33; 3 added scripts, locks, linked refs, activate parents,
primitives and base flags, 4 quests, globals, placed actors and plugins, 5
navmeshes, 6 cloud layers, weather data, precipitation and weather
regions, 7 base record flags, 8 what actors are built from, 9 AI packages,
NPC factions and placed actors' linked references, 10 image spaces (IMGS)
and weathers' and cells' links to them, interior directional ambient with
lighting templates (LGTM) resolved, directional materials (MATO, STAT DNAM),
grass (GRAS, LTEX GNAM), addon nodes (ADDN) and placed lights' XRDS and
XLIG, 11 large references (WRLD RNAM), per worldspace, 12 placed lights' XEMI,
the region whose weather's sunlight colour tints the light, 13 volumetric
lighting (VOLI) and weathers' links to it (WTHR HNAM), 14 water sun sparkle, fog amount and depth controls (WATR DNAM), 15 grass bound radius (GRAS OBND), 16 base records' alternate textures (MODS), 17 interiors' sky region (XCCM), 18 land texture snow flag, 19 AMMO and PROJ, 20 PROJ muzzle flash, 21 sound descriptors (SNDR), sound markers (SOUN) and output models (SOPM), 22 keywords (KYWD, KWDA on bases), form lists (FLST) and messages (MESG), 23 explosions (EXPL) and base destruction (DEST), 24 actor base values (ACBS, DNAM, AIDT, RACE DATA) and base names, 25 cell and worldspace names, 26 dialogue and conditions, 27 scenes, 28 item data, container contents and leveled list chance globals, 29 recipes, magic effects, enchantments, item effects, harvest data and bench types, 30 game settings, weapon combat data, race attacks, first-person models and impacts, 31 combat styles, the NPC combat style and crime faction, faction relations, 32 the aggro radius of NPC_, 33 magic: spells, effect conditions, magic effect data, art objects, projectile, enchantment and NPC spell data). Format 9 only adds:
an engine reading 9 reads 8, whose actors then have no packages. Written during a merge pass, so every FormID in it is
global: resolved through the winning plugin's master list.

A vector said to be sorted by `id` or `ref` has that field marked `(key)` in
`world.fbs`: unique, increasing, and found with flatc's `LookupByKey`. A
cell's linked references, activate parents and primitives are sorted by `ref`
too, but a reference can have several, so they have no key and a reader looks
for the run of one reference.

Flag words are `bit_flags` enums in the schema, and the fields have the enum as
their type, the integer type they always had, so the bytes do not change.
`world.fbs` has `RefFlags` (references and placed actors), `CellFlags`,
`LightFlags`, `NavTriangleFlags`, `ParentFlags`, `WeatherClass`, `QuestFlags`,
`StageFlags`, `LogEntryFlags`, `AliasFlags`, `NpcFlags`, `NpcTemplateFlags`,
`LeveledListFlags`, `PackageFlags`, `BranchFlags`, `GrassFlags`, `RecordFlags`
and `ScriptStatus`, and `ConditionFlags` for the low bits of a condition's type
byte (the field stays a plain byte, since the comparison is in the top three);
`script.fbs` has `FunctionFlags` and `PropertyFlags`. The values in an enum
are bit numbers. The words are the plugin's own, as stored: an enum names the
bits one of the two halves acts on, and the others can be set. Both halves
test bits by these names, through `has_flag` in
`formats/include/skydot_formats/flags.hpp`.

- `cells`, sorted by id: editor id, worldspace (0 for interiors), DATA flags
  (`CellFlags`), grid, water height, decoded XCLL lighting (the 92-byte form
  and the 72 and 64-byte forms that Skyrim.esm also has; what a shorter one
  lacks reads as neutral: fog far colour = fog near colour, fog max 1, no
  light fade distances, no inherit flags), lighting template, the cell's
  references (sorted by id) and load doors, its terrain, and whether it is its
  worldspace's persistent cell.
- A reference: base, position and rotation in Skyrim space (Z-up, game units,
  radians), scale, flags (`RefFlags`: initially disabled, persistent,
  enable-opposite, activated only by its activate parents), enable parent. Deleted references
  are omitted. Placed actors (ACHR) are listed separately.
- Per cell, sorted by id: its navmeshes (NAVM's NVNM): vertices in Skyrim
  space, triangles with their neighbours per edge and flags
  (`NavTriangleFlags`: edge is a link, preferred, water, in front of a door),
  edge links into other navmeshes (portals across cell borders,
  ledges), and the triangles in front of doors. The search grid and the
  cover triangle list are left out.
- Per cell, sorted by reference: scripts (VMAD), locks (XLOC), linked
  references (XLKR; placed actors' too, from format 9), activate parents
  (XAPR) and primitive volumes (XPRM).
- Scripts, on references and bases: name, status, and properties by name with
  their type, status and values. Object properties are global FormIDs, with an
  alias index when they name a quest alias. Status as stored, a
  `ScriptStatus` (UESP: 0 local, 1 inherited, 2 removed, 3 inherited and
  removed).
- `bases`, sorted by id: every record with a model (`MODL`; for ARMO the world
  model `MOD2`, else `MOD4`, since its `MODL` is an armature FormID) as a
  normalized virtual path (`meshes/...`) or with scripts, and every LIGH with
  its DATA/FNAM values. DOOR bases carry their flags (automatic, hidden,
  minimal use, sliding). INFO, PACK, SCEN and PERK scripts are not included:
  they need fragment data and the systems that run it.
- `quests`, sorted by id: QUST's flags, priority, type, story manager event,
  scripts, the fragment script with its stage fragments (sorted by stage and
  log entry), stages (sorted) with their journal entries, objectives with the
  aliases they target, and aliases in fill order: name, flags, how they are
  filled (forced reference or location, unique actor, another quest's alias,
  created object; else a condition count) and their scripts. Conditions are
  counted, not carried.
- `globals`, sorted by id: GLOB's kind and value.
- `actors`, sorted by reference: ACHR's NPC_, cell, position, rotation and
  flags (as a reference's).
- What actors are built from, each sorted by id (format 8):
  `npcs` (NPC_: sex and other ACBS flags, level, race, template and template
  flags, skin, default and sleeping outfit, height, weight, head parts,
  items, and the path of the FaceGen head the game precomputes for it,
  `meshes/actors/character/facegendata/facegeom/<plugin>/<id>.nif`, named by
  the plugin owning the form, whether or not the pack has it); `races`
  (skeleton NIF and behaviour graph per sex, skin, heights and weights per
  sex, DATA flags, body parts, head parts, armor race); `armors` (ARMO's
  slots, race and addons); `armor_addons` (ARMA's slots, race and further
  races, the model per sex, priorities and weight sliders); `outfits` (OTFT's
  items); `leveled_lists` (LVLI and LVLN with flags, chance none and
  entries). How the engine combines them: `engine/docs/actors.md`.
- AI packages (format 9): `npcs` also carry their packages (PKID, in
  priority order), their default package list (DPLT's FLST, expanded) and
  factions with rank (SNAM). `packages`, sorted by id: every PACK, a package
  (PKDT type 18) or a template (19), with its general and interrupt flags,
  preferred speed, schedule (PSDT: month, day of week, date, hour, minute,
  duration in minutes; -1 any), conditions, template, data inputs, procedure
  tree (templates only), idles, owner quest, combat style and its OnBegin,
  OnEnd and OnChange idles. A data input has its key (UNAM), type (ANAM), the
  template's name for it, a number (Bool, Int, Float), and a location (PLDT:
  type, value, radius) or target (PTDA: type, value, count). The tree is in
  pre-order: a branch has its type, conditions, child count, flags, its
  procedure for a leaf, the input keys the procedure reads and PFO2's flag
  overrides. Conditions (CTDA, with CIS1/CIS2) have their type byte, function,
  comparison value or GLOB, parameters, run-on, reference and third
  parameter; FormID parameters are global, chosen by the function's
  parameter types (`bethconv/record/conditions.cpp`, from xEdit). How the
  engine runs them: `engine/docs/ai.md`.
- Sound (format 21), each sorted by id: `sound_descriptors` (SNDR: the
  file names as normalized virtual paths of sound assets, the output model,
  category, descriptor type CNAM as stored (0x1EEF540A standard), looping
  type, frequency shift and variance in percent, priority, dB variance and
  static attenuation in hundredths of a dB); `sound_markers` (SOUN: the
  descriptor, SDSC); `sound_output_models` (SOPM: flags, whose bit 0 is
  attenuation with distance, type, min and max distance in game units and
  the five attenuation curve bytes). A SOUN is also a `bases` entry without a
  model, so its placed references resolve.
- Keywords, form lists and messages (format 22), each sorted by id:
  `keywords` (KYWD: the editor id, which `Keyword.GetKeyword` looks up; a
  base's `keywords` are its KWDA, only for bases that are in `bases` anyway:
  those with a model or scripts); `form_lists` (FLST: LNAM as global ids, in
  order); `messages` (MESG: `title` FULL, `text` DESC, `flags` DNAM whose bit
  0 is message box, the ITXT of each button).
- Explosions and destruction (format 23): `explosions` (EXPL, sorted by id:
  model, light, the two sounds, impact data set, placed object, projectile,
  force, damage, radius and flags; also a `bases` entry). A base's
  `destruction` (DEST) has its health, flags and stages (DSTD: health percent,
  index, model-damage stage, flags, self-damage per second, explosion, debris
  and count, and DMDL as the stage's model). A stage opens when health falls to
  its percent; the script-side stage number counts the stages opened.
- Actor values and names (format 24): an `npcs` entry adds the ACBS offsets
  (signed), calc levels and speed multiplier, DNAM health, magicka, stamina and
  the 18 skill values and offsets, and AIDT aggression..assistance; `flags`
  carries the whole ACBS word. A `races` entry adds RACE DATA's skill boosts,
  starting health/magicka/stamina, carry weight and the three regeneration
  rates. A `bases` entry adds `name` (FULL) and `activate_text` (RNAM of ACTI
  and FLOR). Format 25 adds `name` (FULL) to `cells` and `worlds`, for the name
  of a load door.
- Dialogue and conditions (format 26): `dialogue_branches` (DLBR),
  `dialogue_topics` (DIAL, each with its `infos` in the topic-children group
  order: a later plugin's override keeps the base plugin's place and new INFOs
  follow in the order the merge first saw them), `voice_types` (VTYP),
  `locations` (LCTN), `relationships` (RELA) and `factions` (FACT: flags and
  vendor hours, radius and chest), all sorted by id except the INFOs. An INFO
  carries its conditions (the PACK condition table, FormIDs global), its
  responses (TRDT with the ILSTRINGS text and the notes), links (TCLT), the
  script fragment names of its VMAD and the usual `scripts`. A `quests` entry
  adds `dialogue_conditions` (CTDAs before NEXT; they gate all of the quest's
  dialogue), `event_conditions` (after NEXT), and `condition_list` on log
  entries and aliases (their `conditions` count stays). `npcs` add `voice_type`
  and `keywords`, `races` `voice_types` (male, female) and `cells` `location`
  (XLCN). Unlike a package's conditions, an empty `string1`/`string2` of a
  dialogue condition is an empty string, not absent.
- Items (format 28): `bases` gain `item` (`ItemData`) for every carriable
  base, with or without a model: ALCH, AMMO, ARMO, BOOK, INGR, KEYM, LIGH
  with the carry flag, MISC, SLGM, SCRL and WEAP. `category` is one of misc,
  weapon, armor, clothing, jewelry, potion, food, ingredient, book, key,
  soul_gem, scroll, ammo, light; `weight`, `value`, `playable` (false for the
  record's Non-Playable flag, or AMMO's), `equip_slot` (ETYP of ARMO, WEAP and
  SCRL), `weapon_type` (WEAP DNAM byte 0, 0..9), `damage`, `armor_rating`
  (DNAM / 100), `armor_type` (BOD2: 0 light, 1 heavy, 2 clothing),
  `biped_slots` (bit n is slot 30 + n) and `food` (ALCH food flag). Armor with
  armor type clothing is jewelry when it covers only amulet, ring or circlet
  slots, else clothing. A CONT base carries `items` (CNTO; a form can be a
  leveled list) and its DATA flags in `flags` (bit 1 respawns). `leveled_lists`
  also holds LVSP, and `chance_global` (LVLG, 0 if none) replaces
  `chance_none` when set.
- Crafting and harvesting (format 29): `recipes` (COBJ: `product` CNAM,
  `product_count` NAM1 default 1, `bench_keyword` BNAM, `components` CNTO,
  `conditions` CTDA), `magic_effects` (MGEF: `name`, `description` DNAM with
  `<mag>` and `<dur>`, `flags`, `base_cost`, `magic_skill`, `actor_value`,
  `archetype`, `casting_type`, `delivery`, `keywords`) and `enchantments` (ENCH:
  `cost`, `charge`, `effects`), each sorted by id. `EffectItem` is
  `{effect, magnitude, area, duration}`; `ItemData.effects` holds it for INGR,
  ALCH and SCRL, `ItemData.enchantment` the EITM of WEAP and ARMO. `bases` of
  FLOR and TREE with PFIG gain `harvest` (`ingredient`, `sound` SNAM,
  `seasonal_yield` PFPC with one byte per season, spring first), FURN bases
  `bench_type` and `bench_skill` (WBDT).
- Combat data (format 30): `gmsts` (every GMST: `editor_id`, `kind`
  floating, integer, boolean or text, and the value; sorted by the ASCII
  lower-case editor id for binary search; unsigned settings are integers bit
  for bit), `impact_data_sets` (IPDS: `entries` of material and impact) and
  `impacts` (IPCT: `model`, `duration`, `orientation`, `angle_threshold`,
  `placement_radius`, `flags`, `sound1`, `sound2`, `hazard`), the last two
  sorted by id. `ItemData` of a WEAP gains `speed`, `reach` (DNAM bytes 4 and
  8), `weapon_skill` (DNAM byte 76, an actor value index), `stagger` (DNAM
  byte 96), `crit_damage` and `crit_mult` (CRDT), `impact_data_set` (INAM),
  `first_person_model` (the model of the WNAM STAT), `equip_sound` and
  `unequip_sound` (NAM9, NAM8). `ArmorAddon` gains `male_model_1st` and
  `female_model_1st` (MOD4, MOD5). `Race` gains `unarmed_damage` and
  `unarmed_reach` (DATA bytes 96 and 100), `impact_material` (NAM4),
  `impact_data_set` (NAM5) and `attacks` (`RaceAttack`: ATKE `event` with the
  ATKD fields; `flags` stay raw, 0x4 is the power attack bit).
- `RefFlags.starts_dead` (bit 5, set on a placed actor whose ACHR record flag 0x200 "Starts Dead"
  is set; added to format 31 without a bump: an older pack has the bit clear and its actors live).
- Magic (format 33; sources UESP and xEdit): `spells` (SPEL, sorted by id: SPIT `cost`,
  `flags` raw, `type`, `charge_time`, `casting_type`, `delivery`, `cast_duration`, `range`,
  `half_cost_perk`; ETYP `equip_slot`; `effects`). `EffectItem` is a table (it was a struct) and
  gains `conditions` (the CTDAs after each EFIT). `MagicEffect` gains the DATA fields beyond
  `delivery` (associated item, resist value, second actor value and weight, skill level, area,
  casting time, taper, projectile, explosion, light, shaders, art objects, impact data, skill usage,
  dual cast, equip ability, image space modifier, perk, AI score), `counter_effects` (ESCE),
  `sounds` (SNDD `kind` + `sound`), `conditions` (CTDA) and `scripts` (VMAD); its `flags` comment
  was corrected (0x4 is detrimental, 0x2 recover). `art_objects` (ARTO: `model`, `art_type`).
  `Projectile` gains `model`, `light`, `muzzle_flash_light`, tracer chance, explosion proximity and
  timer, `explosion`, the three sounds, `sound_level`, `fade_duration`, `impact_force`,
  `default_weapon_source`, `cone_spread` and `relaunch_interval`. `Enchantment` gains the ENIT
  `flags`, `casting_type`, `delivery`, `enchantment_type` (6 enchantment, 12 staff), `charge_time`,
  `base_enchantment` and `worn_restrictions`. `ItemData.enchantment_amount` (WEAP/ARMO EAMT).
  `Npc.spells` and `Race.spells` (SPLO as they are: spells, LVSPs, shouts).
- `Npc` gains `aggro_radius_behaviour`, `aggro_warn`, `aggro_warn_attack` and `aggro_attack`
  (format 32, AIDT).
- Combat styles (format 31): `combat_styles` (CSTY, sorted by id: the CSGD
  multipliers, CSME melee multipliers, CSCR circle/fallback/flank/stalk, CSLR
  strafe, CSFL flight values and the DATA `flags`; a shorter CSGD in an older
  record leaves the rest 0). `Npc` gains `combat_style` (ZNAM) and
  `crime_faction` (CRIF). `Faction` gains `relations` (XNAM: `faction`,
  `modifier`, `reaction` 0 neutral, 1 enemy, 2 ally, 3 friend); `flags` is the
  whole DATA word.
- Scenes (format 27): `scenes` (SCEN), sorted by id, FormIDs global: `quest`
  (the PNAM after the last action), `flags` (FNAM: 0x1 begin on quest start,
  0x2 stop on quest end, 0x4 show all text, 0x8 repeat while true, 0x10
  interruptible), `conditions`, `phases` in order (a phase's index is its
  position; `name`, `start_conditions` before its first NEXT and
  `completion_conditions` after it), `actors` (`alias`, LNAM `flags`, DNAM
  `behaviour_flags`) and `actions`: `type` (0 dialogue, 1 package, 2 timer),
  `name`, `alias` (-1 for timers), `index` (INAM), `flags` (FNAM, dialogue
  only), `start_phase`, `end_phase`, `topic` (dialogue), `packages` (package),
  `timer_seconds` (the second SNAM of a timer), `head_track_alias` (-1
  none), `delay_max`, `delay_min`, `emotion` and `emotion_value`. Actions
  have no conditions in Skyrim Special Edition. The VMAD gives `scripts`,
  `fragment_script`, `begin_fragment`, `end_fragment` and `phase_fragments`
  (`phase`, `on_end`, `fragment`). Old-style result scripts (QNAM, SCHR),
  the editor's phase width (WNAM) and VNAM are not kept. Every PACK is in the
  package table, whether an NPC uses it or not.
- `plugins`, in load order: each active plugin's name and FormID prefix
  (`0xII000000`, or `0xFEIII000` for a light plugin), so scripts can name a
  form by plugin and object id.
- `worlds`, sorted by id: WRLD with parent and parent flags (`ParentFlags`:
  `land_data`, the land comes from the parent), DATA flags, default land and water height, water
  type and bounds, and its large references (format 11). In the game, cells
  out to `uLargeRefLODGridSize` beyond the loaded ones draw their large
  references (rocks, cliffs, large buildings: the Creation Kit lists the
  references whose bounds exceed `fLargeRefMinSize` in the WRLD's RNAM fields,
  one per cell) as full models, and object LOD takes over past them.
  `large_refs` (sorted by id) holds each such reference once with its
  placement as a cell's `Ref` has it, and the cell it stands in; `large_cells`
  (sorted by cell_y, cell_x) says, per cell of the grid, which of them to draw
  there: `count` indices into `large_refs` from `large_cell_refs[first]`. A
  cell's list holds the references that stand in it and those of neighbouring
  cells whose bounds reach into it. RNAM layout (checked against REFR positions
  on all 160,247 entries of Skyrim.esm's Tamriel): per cell, int16 Y, int16 X,
  uint32 n, then n of FormID, int16 Y, int16 X (the reference's own cell).
  A plugin's WRLD override lists only the cells it changes (Update.esm's
  Tamriel 191 of 8,455), so the converter takes each cell's list from the last
  plugin that lists it (an empty list empties the cell); the rest of the WRLD
  is the winner's as usual. A listed reference is left out if it is deleted,
  initially disabled, in another worldspace or not in the load order; its
  placement is its winning REFR's. A reference with an enable parent is kept,
  with `enable_parent` and the flags, for the engine to evaluate. `flags` has
`visible_when_distant`, the REFR's record flag 0x8000, and a base's
`record_flags` has it too: in Skyrim.esm's object LOD (`.bto`) those large
references are already drawn, as the shapes named `...-LargeRef`, and the others
are not (measured on SE, level 4).
- `land_textures`, sorted by id: LTEX with its TXST's diffuse and normal map
  as virtual paths, and its specular value.
- `waters`, sorted by id: WATR's visual values (opacity, shallow, deep and
  reflection colours, fresnel, reflectivity, fog distance, three noise layers
  with wind direction, speed, scale and amplitude) and its noise textures.
  Cells name theirs (XCWT); 0 means the worldspace's.
- `climates` and `weathers`, sorted by id: CLMT's weather chances, sun
  times (hours), sun and glare textures, night sky model, volatility and
  moons with their phase length; WTHR's colour table (17 colours by sunrise,
  day, sunset, night), fog distances, directional ambient, its 29 cloud
  layers (texture, speed, colour and alpha by time of day, enabled; layer i
  is drawn on the i-th shape of `meshes/sky/clouds.nif`), DATA (wind,
  transition, sun glare and damage, precipitation and thunder timing,
  classification, lightning colour), its precipitation and aurora model.
  Worldspaces name their climate.
- `precipitations`, sorted by id: SPGD, rain or snow particles (speeds,
  sizes, atlas, box size, density; units mostly undocumented).
- `regions`, sorted by id, only those with a weather list (REGN RDAT type 3):
  worldspace, polygons in game units, weathers with chance and gating
  global, priority and override flag.
- A cell's terrain (LAND): VHGT's height offset and 33 x 33 deltas as stored
  (heights are 8 units per step, summed down column 0 and then along each
  row), VCLR vertex colours, and per quadrant a base layer and additional
  layers with their VTXT opacity (vertex index in the quadrant's 17 x 17 grid,
  opacity 0..255). Normals are not stored: computed from the heights they
  match VNML to about a degree.

A worldspace's persistent cell has grid (0, 0) like the real cell there, but
holds the persistent references of the whole worldspace; place those by
position.

A reference whose base is not in `bases` places something without a model
(markers without meshes, sounds, etc.). The engine converts coordinates: the
same Z-up → Y-up rotation and 0.0142875 m per unit the mesh writer applies.

## Asset store

Two layouts hold the same assets under the same names.

**Blob** (the default). `assets.idx`, little-endian:

| Field | Type |
| --- | --- |
| magic | `BCAI` |
| version | u32, 1 |
| generation | u32: the blob is `assets-<generation, 4 digits>.blob` |
| count | u32 |
| blob_bytes | u64: bytes of the blob the index covers |
| entries | count × { hash: 32 bytes, offset: u64, size: u64, kind: u8, 7 zero bytes } |

Entries are sorted by hash bytes; kind is 0 mesh, 1 texture, 2 script, 3 lod, 4 animation, 5 sound.
Every entry lies inside `blob_bytes`, and nothing follows the last entry; a
reader refuses an index that breaks either. Entries start on 16-byte
boundaries, in conversion order (sorted virtual paths), so the blob is
byte-identical for the same input.

A run appends to the blob and replaces `assets.idx` at the end (write, then
rename). Bytes past `blob_bytes` are left by an interrupted run; the next run
truncates them. `--prune` copies the referenced entries into the next
generation's blob, switches the index to it, then deletes the old blob.

The blob exists because packs are written to whatever disk has room. About
70,000 loose files on an SMR disk behind ntfs-3g once hung the whole mount
until a hard reset; one large file is written sequentially, and engines map it.

**Loose** (`--store loose`): `assets/<first two hex>/<hex><ext>`.

### The content hash

Name: `BLAKE3-256(source bytes ‖ converter version ‖ settings)` as 64 lowercase
hex digits.

Each component is prefixed with its 64-bit length; otherwise bytes `ab` +
settings `c` would hash like `a` + `bc`.

Hashing the input means dedupe happens before conversion (a duplicate mesh is
never parsed), and names only change when the settings string says the output
changed.

### Settings fingerprints

Per kind, mentioning only settings that affect that kind:

| Kind | Extension | Fingerprint | Current |
| --- | --- | --- | --- |
| mesh | `.glb` | `mesh/<n>;` flags, unit scale as `%.9g` | `mesh/24` |
| texture | `.dds` | `texture/<n>;` flags, `;max=<px>` when limited, `;encode=<mode>` unless `keep`; for a texture some material alpha-tests, `;cov/1;t=<threshold>`, plus `;floor` in the default mode (alpha coverage mips) | `texture/1` |
| script | `.pexfb` | `script/<n>;decoded` | `script/2` |
| lod | `.lodfb` | `lod/<n>;decoded` | `lod/1` |
| animation | `.animfb` | `animation/<n>;decoded` | `animation/1` |
| sound | `.wav` (historical: the bytes may be WAV, Ogg or a voice container) | `sound/<n>;pcm16;vorbis-q<q>` | `sound/2;pcm16;vorbis-q0.4` |

`%.9g` keeps floats identical across machines. The leading number is bumped
whenever a writer's output changes (a changed option already changes its
fingerprint, e.g. `refs=` when pack meshes lost their images): `mesh/2` percent-encoded control bytes in
texture paths, `mesh/3` kept non-finite floats out of JSON, `mesh/8` added
controllers, particle systems and hidden nodes to the extras
([`format-notes/nif-animation.md`](format-notes/nif-animation.md)), `mesh/9`
every drag modifier with its axis, `mesh/10` rigid body fields in the
collision extras (below), `mesh/11` compressed-mesh triangles that follow a
chunk's strips (they were dropped), `mesh/12` cylinder, strips and plane
collision shapes, `mesh/13` the texture of sky shaders (stars), `mesh/14` inverse bind
matrices that include a skinned shape's own placement (bodies had been 120
units low), `mesh/15` quadratic keys' tangents the right way round (steady
motion had eased in and out at every key), `mesh/16` `hair_tint_color` and
`skin_tint_color` in the material extras (FaceGen hair was grey), `mesh/17`
`draw_order` on the children of a BSOrderedNode (node extras), `mesh/18`
water shaders named `BSWaterShaderProperty` rather than `other`, `mesh/19`
`addon` (the BSValueNode's value) on nodes named `AddOnNode...` (node extras;
candle flames hang there), `mesh/22` `extras.bethconv.phantoms` (trigger volumes, below), `mesh/23`
`extras.bethconv.behavior_graph` (the `BSBehaviorGraphExtraData` file path,
as stored, on the root node next to the provenance), `mesh/24` `switch` (the
active child index) on NiSwitchNodes other than LOD nodes (node extras; flora
show child 1 once harvested); the list in
`ConvertOptions::mesh_settings` has the rest. Without a bump, dedupe would keep
reusing stale assets.

Fingerprint bumps rename assets; format bumps make readers refuse the pack.

## Script assets

A `.pexfb` is one compiled Papyrus script, decoded: a FlatBuffer (identifier
`BPX1`, schema `formats/schema/script.fbs`, its own `format_version`, 1) holding the
PEX's string table, user flags and objects with their variables, properties,
states and functions. Names stay indices into the string table; docstrings are
dropped. Each function keeps its instructions as opcode bytes and a flat
argument list (a call's count is implied by the next instruction's offset) and
its source line numbers when the PEX had debug info.

The converter decodes every `.pex` completely and refuses one that is wrong
anywhere: a string index outside the table, an unknown opcode or value type, a
jump outside its function, an object size that disagrees with its content, or
bytes left over. The FlatBuffer verifier only checks structure, so a reader
must still bounds-check indices; a pack written by this converter never
violates them. Scripts keep their virtual paths (`scripts/defaultlever.pex`),
so class `DefaultLever` is found at `scripts/defaultlever.pex`.

All 10,015 LE, 14,302 SE and 13,507 VR scripts and the FUS list's 5,352
decode; the corpus harness pins the counts and a hash over the assets.

## Sound assets

A sound asset keeps its game path, normalized, and is one of three things; a
reader tells them apart by the first four bytes.

- **Sound effect** (`.wav` in the Sound archive, `RIFF`): stored as it came,
  RIFF/WAVE, format tag 1 (PCM), 16 bits, any channel count or rate. Anything
  else (ADPCM, 8-bit, not RIFF) is a failure, counted in `failed` and listed in
  `report.json`. Vanilla SE: all 5,929 `.wav` in `Skyrim - Sounds.bsa` qualify.
  Godot reads the bytes with `AudioStreamWAV.load_from_buffer`.
- **Music** (`.xwm`, WMA2 in an XWMA RIFF): decoded and re-encoded as a plain
  Ogg Vorbis stream (`OggS`), same channels and rate, libvorbis VBR quality 0.4,
  fixed stream serial number so the bytes are deterministic. Godot reads it with
  `AudioStreamOggVorbis.load_from_buffer`.
- **Voice** (`.fuz`: `FUZE`, u32 version 1, u32 lip size, the `.lip` bytes, then an
  XWMA RIFF): a SkyDot voice container, all integers little endian, offsets from
  the start of the container.

  | Offset | Field |
  | --- | --- |
  | 0 | `SDVC` |
  | 4 | u32 version, 1 |
  | 8 | u32 Ogg offset (24) |
  | 12 | u32 Ogg size |
  | 16 | u32 lip offset |
  | 20 | u32 lip size |
  | 24 | the Ogg Vorbis stream (as for music), then the `.lip` bytes untouched |

  The vpath keeps its `.fuz`. A reader slices the Ogg part out and loads it as
  music. The lip bytes are for facial animation later.

A stream that does not decode (not XWMA, truncated, unknown codec) is a failure
like any other. `--no-sounds` skips all three.

## LOD

A worldspace's distant LOD, as the game ships it (for Tamriel):

- `lodsettings/tamriel.lod` → a LOD asset with `settings`: the south-west
  cell of the LOD grid, how many cells it spans and the finest and coarsest
  quad size (4 and 32 cells in vanilla).
- `meshes/terrain/tamriel/tamriel.L.X.Y.btr` → a mesh: the terrain of the
  quad of level L whose south-west cell is (X, Y). Its vertices are relative
  to that corner (the NIF has no translation for it) and it is textured by
  `textures/terrain/tamriel/tamriel.L.X.Y.dds` with a model-space normal map
  (`…_n.dds`). A second shape at a fixed height is the LOD water.
- `meshes/terrain/tamriel/objects/tamriel.L.X.Y.bto` → a mesh: the objects of
  that quad, placed in world space by its own node transform.
- `meshes/terrain/tamriel/trees/tamriel.lst` → a LOD asset with `tree_types`:
  billboard sizes and their rectangles in
  `textures/terrain/tamriel/trees/tamrieltreelod.dds`.
- `meshes/terrain/tamriel/trees/tamriel.4.X.Y.btt` → a LOD asset with
  `trees`: the tree instances of a level-4 quad, in world space.

A `.lodfb` is a FlatBuffer (identifier `BLD1`, schema `formats/schema/lod.fbs`, its
own `format_version`, 1). The source layouts are in
`include/bethconv/pack/lod_asset.hpp`; the converter refuses a file with
counts larger than itself, levels that are not powers of two, or bytes left
over, except after a `.btt`'s declared blocks (six Solstheim files in SE and
VR; skipped with a warning). Every LOD file of the three vanilla installs
converts.

## Animation assets

Every `.hkx` becomes an `.animfb`: a FlatBuffer (identifier `BAN1`, schema
`formats/schema/animation.fbs`, its own `format_version`, 2) with whatever
skeletons (`hkaSkeleton`) and clips (`hkaSplineCompressedAnimation` or
`hkaInterleavedUncompressedAnimation`, with their binding and annotations)
the file holds, a behaviour character's names (`hkbCharacterStringData`)
and a behaviour graph's clip generators (`hkbClipGenerator`: clip name, the
animation file it plays, play mode, speed, start time, user-controlled
fraction and event triggers), its event and variable names and its state
machines (`hkbStateMachine`: states with their generator, enter and exit
events and transitions; SE files only, the 4-byte layout is not measured;
format 2). Other generators, blends, modifiers and physics are not
decoded; such files still convert, so every path resolves.

Clips stay B-splines, as Havok stores them: sampled per frame, vanilla SE's
clips would take 1 GiB, the splines about 130 MiB. The schema's header says
how to sample. Values are game units, Z-up, each bone relative to its parent.
The format and its measurements: `converter/docs/spikes/hkx.md`. Every `.hkx`
of the three vanilla installs converts (SE 7,699 files, 6,126 clips).

Root motion is not in the HKX. Skyrim keeps it, with each clip's id and
playback speed, in text files, which also become `.animfb` assets of kind
`animation`:

| Source | Fields filled |
| --- | --- |
| `meshes/animationdata/<project>.txt` | `project_files`, `project_clips` |
| `meshes/animationdata/boundanims/anims_<project>.txt` | `motions` |
| `meshes/animationdatasinglefile.txt` | `projects`, each with all three |

`dirlist.txt` is not converted. A clip's id is not an index into anything:
the file it plays is the clip generator of the same name in one of the
project's behaviour graphs (`project_files` starting `Behaviors\`), and its
motion is the `motions` entry with the same id. The DLC creatures' projects are
only in the single file (LE's is in `Update.bsa`). The text formats:
`converter/include/bethconv/animation/animation_data.hpp`.

## `vpath.idx`

Tab-separated, sorted, one line per virtual path, after two comment lines:

```
# bethconv vpath index v7
# virtual path\tcontent hash\tkind\twinning source
meshes/clutter/apple01.nif\t3f9c…\tmesh\tSkyrim - Meshes0.bsa
```

`kind` is `mesh`, `texture`, `script`, `lod`, `animation` or `sound`. An unknown kind means a newer writer;
say so rather than guess.

Text because it is for debugging mod overrides: people read it and tools diff
it. About 20 MB for vanilla SE, 3% of the pack.

Several paths mapping to one hash is normal (dedupe); each gets a line.

`include/bethconv/pack/vpath_index.hpp` implements the format and both writer
and reader use it.

### Texture references in meshes

A pack's GLBs have no `images` or `textures`: glTF resolves image URIs against
the document, which a content-addressed store has no layout for, and Godot's
runtime glTF loader fails on URIs it cannot open. Each material names its
textures in `extras.bethconv.texture_slots` (`"0"` diffuse, `"1"` normal,
`"2"` glow, and so on, each with a `role` and a `path` in the `vpath.idx`
form). The engine resolves those itself.

`bethconv view` adds glTF images back for other consumers: slot 0 as base
colour, slot 1 as normal map unless `model_space_normals`, slot 2 as emissive
when `has_glowmap`, with URIs relative to the view.

### Collision in meshes

Havok collision is never glTF geometry (which would render). The node a
`bhkCollisionObject` hangs off carries `extras.bethconv.collision`, an array
with one entry per leaf shape (MOPP trees, list and transform shapes are
unwrapped):

| Key | Meaning |
| --- | --- |
| `kind` | `box`, `sphere`, `capsule`, `cylinder`, `convex_vertices`, `compressed_mesh`, `mesh`, or `unsupported` |
| `block`, `node` | Havok block name; the owning node's name |
| `layer` | Skyrim collision layer (1 static, 2 animated static, 4 clutter, 13 terrain, 15 non-collidable, ...) |
| `motion_type`, `quality_type` | `hkMotionType`; `hkpCollidableQualityType` (0 fixed, 1 keyframed, 2 to 7 moving, 8 character, 9 keyframed reporting) |
| `mass`, `friction`, `restitution` | the rigid body's values (mass in kg) |
| `havok_material` | the shape's material id |
| `transform` | `translation`, `rotation` (quaternion): a `bhkRigidBodyT`'s transform composed with every transform shape above the leaf |
| `half_extents` | box |
| `radius` | sphere, capsule, cylinder; a box's or convex hull's convex radius |
| `point_a`, `point_b` | capsule and cylinder end points (a cylinder's convex radius is already added to its radius and ends) |
| `vertices` | flat `x, y, z` list: convex hull points, or mesh vertices |
| `indices` | mesh triangles, three per triangle. `compressed_mesh`: big triangles, then per chunk its strips and the plain list after them; `mesh` (`bhkNiTriStripsShape`): its strips parts' triangles |

A `bhkPlaneShape` (one vanilla mesh) becomes `convex_vertices`: the flat
polygon where the plane cuts its bounding box. Flat hulls need thickening in
engines that cannot build a hull without volume, as Havok's do through their
convex radius.

Everything is in the owning node's frame but in **Havok units**: multiply
lengths (including translations) by 69.99124 to get game units, the node's
own unit. `motion_type` is unreliable on its own: vanilla static architecture
often says `5` (box stabilized); `quality_type` is the one to read. Vanilla SE:
static architecture is layer 1 with quality 0, animated statics layer 2 with
quality 1, clutter layer 4 with quality 4. (nif.xml numbers the quality from 1
for fixed; the data does not fit that.)

### Phantom trigger volumes in meshes

A node whose `bhkSPCollisionObject` points at a `bhkSimpleShapePhantom` (pressure
plates, trigger boxes) carries `extras.bethconv.phantoms` instead of
`collision`: entries in the same format (`kind` is only `box` or `sphere`,
`layer` is the phantom's collision filter, `transform` composes the phantom's own
4x4 with every transform shape above the leaf, Havok units, node frame). Other
phantom shapes are skipped with a converter warning.

### Path encodings differ between `vpath.idx` and GLB URIs

In `vpath.idx` and in extras a path is unescaped: lowercase, forward slashes.

In a GLB written outside a pack (`bethconv mesh`), or added by the view, it is
a URI reference (RFC 3986 §4.2), produced in three steps:

1. `to_uri`: backslashes to slashes, lowercase (the `vpath.idx` form).
2. `uri_escape`: percent-encode everything outside `pchar` except `/`. `:` is
   escaped too, because a colon in the first segment would read as a scheme.
   49 vanilla texture paths contain spaces; 67 vanilla GLBs referenced a
   FaceGen slot literally named `textures\<0x08>NOR`, now `textures/%08nor`.
3. `shield_percents`: `%` → `%25`. `fastgltf::URI` decodes escapes on
   construction and writes the decoded form, so this cancels that decode.

To get the `vpath.idx` key from a URI: undo the doubling, then percent-decode
once. When comparing, escape before, never after.

GLBs placed outside the pack root also carry a `../` prefix for their depth.
`pack_view.cpp` is a consumer that uses only what this document promises.

## `report.json`

Same formatting as the manifest. Keys: `pack_format_version`, `totals`,
`bytes`, `assets`, `failures` (uncapped; `vpath`, `stage`, `kind`, `detail`),
`warnings`, `deferred`, and `alpha_coverage` unless `--alpha-coverage off`
(which textures the materials alpha-test: counts, the thresholds in conflict
and every treated texture with its threshold; see
`converter/docs/format-notes/dds-textures.md`).

`failures` is uncapped because large broken load orders are exactly where it is
needed. `kind` is an `io::ErrorKind` name, for counting by class. Warnings do
not fail a file and are only visible here.

## Loading in an engine

No import step. The engine maps the blob, reads a mesh's GLB bytes and builds
its scene with a runtime glTF loader (Godot's `GLTFDocument`), and loads DDS
bytes as they are, block-compressed. Measured on 4,115 Riverwood meshes: about
1 ms per mesh against 1.7 ms for a baked `.scn`, 0.1 ms per texture.

Earlier versions baked packs into a Godot `.pck` through `--import`. It read
about 16 times its input and wrote five small files per mesh, which on a slow
disk took longer than the conversion and on an SMR disk behind ntfs-3g hung
the machine.

## Determinism

- No wall-clock time anywhere in a pack. Timings go to the terminal.
- No hash-table iteration order in files. `vpath.idx` and all `report.json`
  arrays are sorted.

## Not promised in v7

- **Resolved text.** There is no per-language string file; names cannot be
  resolved from a pack alone.
- **A view.** `bethconv view` builds a derived tree for glTF-only consumers. It
  is optional; put nothing in a pack that only the view can find.
- **All Havok shapes.** `bhkCylinderShape`, `bhkNiTriStripsShape` and
  `bhkPlaneShape` are `CollisionKind::unsupported` with their block name.
  Consumers must handle that kind.
- **All image references resolving.** 572 of 120,313 in vanilla SE point to
  files the game does not ship. `bethconv view` lists them.
- **Finite binary data.** Non-finite floats are kept out of JSON, but a NaN
  vertex reaches the BIN chunk unchanged. Renderer behavior is untested.
- **Migration.** A version mismatch is refused; there is no upgrade tool.

## Code

Paths relative to `converter/`; schemas in `formats/schema/`.

| Piece | Implementation |
| --- | --- |
| layout, dedupe, manifest, report | `include/bethconv/pack/pack_writer.hpp` |
| `assets.idx`, the blob, loose files | `include/bethconv/pack/asset_store.hpp` |
| `vpath.idx`, asset paths, kinds | `include/bethconv/pack/vpath_index.hpp` |
| content hash | `include/bethconv/pack/content_hash.hpp` |
| `world.fb` writer and reader | `include/bethconv/pack/world.hpp` |
| `world.fb` schema | `formats/schema/world.fbs` |
| script assets | `include/bethconv/pack/script_asset.hpp`, `formats/schema/script.fbs` |
| PEX decoding | `include/bethconv/script/pex.hpp` |
| consumer using only this document | `src/pack/pack_view.cpp` |
| complete pack without game data | `tools/testpack/` (`bethconv-testpack`) |
| pack loaded in Godot | `engine/extension/src/assets/` |
