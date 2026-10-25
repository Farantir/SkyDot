# Active magic effects

Step 7a-2 of the order of work (SYSTEMS.md C5, C7). Decided on 2026-10-18 with
world.fb 33, which carries spells, effect conditions and the whole MGEF.

## Context

Spells, potions, scrolls, enchantments and abilities all end in the same
thing: a magic effect (MGEF) running on an actor for a while. The census of
the first slice (`bethconv form --magic`) says what matters: peak value
modifiers (fortify, 726 uses), value modifiers (363), dual value modifiers
(46), stagger, absorb and script effects come first; the rest follow in
7a-4. A few effects hold a modifier (fortify), most act per second
(damage, restore), some once (a potion of health).

## Decision

- **`magic/` is Godot-free.** `magic_rules` holds the formulas, `active_effects`
  the `ActiveEffects` class. The world it acts on comes through `MagicHost`
  (actor values, temporary modifiers, conditions, deaths, game settings, the
  log, and `effect_changed` for the VM's OnEffectStart/Finish and for the
  game layer's presentation), as `CraftingHost` does for crafting.
- **An instance** is an effect from a source (spell, enchantment, potion,
  ingredient, scroll, ability, poison; form and item extra) cast by a caster
  on a target, with magnitude, duration, elapsed time, area in game units,
  the condition state and, for holds, the modifiers it holds right now.
  Its id comes from the range `effect_instances` (`vm/id_ranges.hpp`, cut
  from the top of the alias handles) because a Script effect's object uses
  the same number as its handle.
- **Rules.** Duration 0 acts once (and the instance is gone). A timed effect
  without Recover acts magnitude per second, accumulated per tick. Recover,
  or a constant effect (ability, worn enchantment), holds a *temporary
  modifier* and takes it away at the end. Detrimental subtracts. Restore
  never exceeds the maximum (`ActorStates::restore`). Peak: of the held
  modifiers on one actor value (per sign) only the largest counts, the next
  takes over when it ends. Dual: the second actor value gets magnitude x
  weight. Absorb: the target loses what it has, the caster gains the same.
  Hostile effects: magnitude x (1 - resist/100) for the effect's resist
  actor value, then again for MagicResist (44), each capped at
  fPlayerMaxResistance (85). Area: feet x 0.3048 / 0.0142875.
- **Conditions** (the entry's CTDAs after EFIT, then the MGEF's) are
  evaluated at the start and every second (PLACEHOLDER interval) with the
  target as subject and the caster as reference. A failing effect is kept
  but inactive: it does not act, holds nothing, has no start event, and is
  not reported by `has_effect`. A refresh re-checks at once.
- **Same caster, target, source and entry refresh** (elapsed restarts), they
  do not stack; different sources stack. This is how a concentration spell
  works: the caller re-applies it every tick and stops to end it.
- **Archetypes without rules** (everything but value modifier, peak, dual,
  absorb, script) are logged once, `magic: archetype N (Name) not done`,
  and counted, but the instance lives its duration, so HasMagicEffect and the
  menu see it.
- **Dual casting, perks and equipment** are placeholders (x1; dual casting
  logged once) until P6.
- **The temporary bucket.** `ActorStates` gets a third part beside base and
  modifier: `temp`, which counts in value and maximum and is *not saved*.
  The ACTV section is unchanged. `ActiveEffects::load` holds the modifiers
  of the loaded instances again, so a save made mid-fortify loads with the
  maximum raised once. Abilities are never saved, they are derived again.

## Spell cost

The cost of a spell is its stored SPIT cost times the skill multiplier
`1 - (skill/400)^0.65` (UESP "Magic Overview", Spell Cost; the game settings
fMagicCasterPCSkillCostBase 0.0025 and fMagicPCSkillCostScale 0.65) times
the perk and equipment multiplier (placeholder 1). The editor stores the
calculated cost in SPIT also for spells without the manual flag (0x1), and a
later plugin does not rewrite it: Dragonborn raises the base cost of
Flames' fire effect from 1.5 to 2.5 and Flames stays at 14, as UESP lists it,
so the stored value is the one that is shown. The editor's formula was
fitted to the stored costs of Skyrim.esm and reproduces them (35, 14, 107,
14, 42, 63 in the unit test): effect cost = base cost x (max(magnitude, 1) x
duration factor)^1.1, where the duration factor is duration/10 for a timed
fire-and-forget effect and 1 otherwise (concentration costs per second),
times (1 + area x fMagicAreaBaseCostMult), and fMagicAreaBaseCostMult is 0
in the game settings. `auto_calc_cost` computes it and is used only for a
record that stored nothing.

## Consequences

- Effects that need the world (summon, light, invisibility...) arrive in 7a-4
  through the same host; the instance already carries caster, target, area and
  an archetype state is added there.
- The temporary bucket is the one place that makes max and value differ from
  a script's `ModActorValue`; scripts that read `GetActorValueMax` see it.

## Casting, poisons, enchanted items (7a-3)

- **Paying and landing are apart** (`magic/casting`): `pay_spell` (cooldown, magicka
  through the host so the regeneration waits, power use noted; concentration
  x seconds) and `land_form` (a SPEL's or a scroll's effects, no cost). `cast_spell`
  is both. Scrolls are SCRL items (fire and forget, delivery of their primary
  effect, no charge time) and cost no magicka.
- **Hands**: `HandCast` is the pure state of the attack button on a hand
  (charging, charged, cast; concentration ticks and stops).
- **Enchanted weapons** (`magic/enchanted`): the charge lives in `ItemExtra`
  (`charge`, -1 = full; `charge_max` for a created enchantment, else EAMT).
  A hit costs the ENCH's cost (manual: ENIT cost, else the summed effect cost);
  it fires while any charge is left; only the player's weapons are spent. The
  inventory and equipment move to the new extra id. ItemExtras layout 2 (ITST 4)
  adds charge and poison; layout 1 loads with defaults.
- **Poisons**: one dose on the right-hand weapon unit, delivered by the next hit.
  The pack has no ALCH poison flag; a potion whose effects are all hostile counts.
- **Worn enchantments** (armor, clothing, jewelry, shield) are constant effects
  derived from what is worn (`WornEnchantments`), never saved.

## NPC magic (7a-5)

- **The choice is a pure rule** (`ai/magic_choice`): a spell is an attack (hostile, touch, aimed or at
  an actor), a heal (non-hostile self value modifier of Health without the MGEF Recover flag: Restore
  Health; with the flag it is Fortify Health), a summon (archetype 18, self or target location: the CK
  delivers conjurations to a location), a cloak or a fortify; wards, powers, abilities and the rest the
  AI does not use. `decide` picks: heal below half Health, then a buff it does not have on while a
  quarter of the Magicka stays, then an attack spell in reach (the die picks among those, the style's
  magic multiplier must beat the weapons' melee and ranged ones), else the weapons. Placeholders are
  named in the header.
- **The AI only decides** (`SkydotAi::mage_tick`): it emits `combat_cast(ref, target, spell, hand,
  phase)`, `start` when the spell is raised, `release` once the casting time has passed (fire and
  forget), `stop` when a held spell ends or the cast is broken (paralysis, flight, the end of the fight).
- **The game layer carries it out** (`NpcMagic`): the same `hand_press`/`hand_step`/`hand_release` rules
  as the player's hands (charge time, concentration paid per step through `pay_spell`, so the Magicka
  regeneration waits) and `Magic.deliver_from(caster, ...)`, the delivery all casters share (self,
  touch, projectiles, areas, the same impact and landing). Third-person clips are the first-person
  names of the actor's own behaviour project (`mrh_chargeloop`, `mrh_aimedconcentration`, `mrh_release`
  and their self forms).
- **Leveled spell lists are seeded by the reference**: two spawned actors of one NPC_ know different
  spells; tests teach the spells they need.

## Created enchantments and soul gems (7a-6)

- **A made enchantment is a value** (`vm::CreatedEnchantment`, saved in CRFT 2, ids from the created
  range, deduplicated by base, effects, cost and kind): the effects of a learned world ENCH at the
  strength the soul and the skill gave. `ItemExtra.enchantment` names a world ENCH or a created one;
  `magic::resolve_enchantment(world, created, id)` is the one reader (effects, cost per use, constant
  or not), used by `weapon_charge`, `hit_magic` and `WornEnchantments`, so no caller tells them apart.
  A weapon's charge maximum is `ItemExtra.charge_max`. CRFT 1 loads with nothing created.
- **Souls**: a gem with a filled form has the soul in its base (`ItemData.soul`); the others (Azura's
  Star, the Black Star, an empty gem whose NAM0 is not set - Petty to Greater have none in vanilla)
  hold it in `ItemExtra.soul` (ITST layout 3, section 5). A black (NPC) soul is grand-sized; which gems
  take it is the gem's `holds_npc_souls`, so the extra needs no seventh value.
- **Rules** (`magic/enchanting`, skill 15, no perks, UESP "Enchanting Effects"): skill multiplier
  `1 + s(s - 0.14)/3.4`; apparel magnitude `floor(base * soul fraction * skill)` (1/12 .. 1), a
  power-affects-duration effect scales its duration; a weapon holds the soul's charge (250 .. 3000),
  uses `3 * cost^1.1 * (1 - sqrt(skill/200))` per hit, and a dearer enchantment is scaled down until one
  use fits the gem. Disenchanting teaches the base ENCH of an item's enchantment unless it is made,
  a staff's, forbidden (MagicDisallowEnchanting on the item or an effect) or known. A reusable gem
  (ReusableSoulGem) is emptied instead of spent. Soul trap fills the smallest empty gem that takes
  the soul; an empty gem with a linked form whose soul is the right size becomes that form, any
  other gets the soul in its extra. Creature souls by level (petty 1-3, lesser 4-15, common 16-27,
  greater 28-37, grand 38+) are from memory of UESP's creature tables, not verified there.
- **The player's table** is `magic::Enchanting` over the inventory, the extras, the created forms and
  the spell book; the game layer owns the keywords' ids and moves equipment slots that held a
  changed unit (as `move_unit_extra` does for a hit).

## Shouts (7b-2)

`magic/shouts` (no Godot) holds the rules; `MagicState` owns a `ShoutBook` beside the `SpellBook`.

- **Data.** A SHOU has three words (WOOP), each with the spell cast when it is the last word spoken
  and a recovery time (Unrelenting Force: 15, 20, 45 s). The book keeps per actor the words known and
  unlocked, the shouts added and removed (AddShout, RemoveShout), the shout in the voice slot and the
  recovery left. The base shouts of a non-player actor come from `base_shouts` (its NPC_ and race
  lists, leveled lists resolved). The player (0x14) also has every shout with a known word, unless
  removed.
- **Words spoken.** Holding the button for less than `fShoutTime1` (0.2 s) speaks one word, less than
  `fShoutTime2` (0.9 s) two, else three. **(Guess)**: that these two GMSTs are the thresholds is from
  their names and UESP's tap / short hold / full hold. The player speaks at most the leading run of
  the shout's words that are known and unlocked; any other actor speaks all of them. With none
  usable the shout is refused ("no word"). A longer hold than usable speaks what is usable.
- **Spell.** Only the spell of the last word spoken is cast (SNAM[words - 1]), not the earlier ones.
  **(Guess)**, as Skyrim's shouts are built (each word's spell includes the weaker one's effect
  strength as a step). Voice spells cost nothing; the game layer lands the spell as any other
  (`land_spell` per actor hit), or `cast_shout` lands it on a given target.
- **Recovery.** The SNAM recovery of the last word times the ShoutRecoveryMult actor value (base 1,
  set for every actor; 0 at the lowest). It counts down in real seconds (`tick_magic` is not slowed by
  game time scale) **(guess)**, and a shout is refused while it runs ("recovering"). Casting and
  shouting exclude each other (sMagicCastShoutWhileCasting, sMagicCastCastWhileShouting): that is the
  game layer's, not the book's.
- **Souls.** A word is unlocked by spending one DragonSouls on a word that is known and locked
  (refused: "unknown word", "already unlocked", "no dragon soul" with the text sFailSpendSoul).
  `Game.UnlockWord` unlocks without knowing the word, as the vanilla scripts use it **(guess)**; a
  word unlocked but unknown is not spoken.
- **Voice slot.** A shout and a power exclude each other (the VoiceEquipSlot equip type: "only one
  shout or Power can be selected"): `equip_voice` puts a shout in the ShoutBook and empties the
  SpellBook's power slot, or the reverse. EquipSpell with source 2, `equip_spell` of the API and
  EquipShout all go through it. RemoveShout empties the slot if it held the shout.
- **Save.** MAGC version 2 adds the ShoutBook as a third text (own section version 1); a version 1
  section reads, with an empty ShoutBook. The condition GetEquippedShout (579) answers from the
  voice slot.
