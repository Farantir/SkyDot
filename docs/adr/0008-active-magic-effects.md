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
