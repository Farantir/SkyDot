# Item instances and created forms

Step 5 of the order of work (SYSTEMS.md I8 crafting). Decided on 2026-10-17
together with crafting and harvesting.

## Context

Until step 5 an inventory was `owner -> form -> count`: every unit of a form
was the same. Crafting breaks that twice. Tempering makes one Iron Dagger
better than another (Skyrim keeps an `ExtraHealth` on that unit), and
enchanting will give a unit an enchantment and a name. Alchemy makes potions
that no plugin defines: Skyrim creates a new `AlchemyItem` form at run time
(an `0xFF......` id) and reuses it when the same effects are mixed again.
Scripts, conditions and quests still count by form: `GetItemCount(IronDagger)`
counts tempered daggers too.

## Decision

- **Extra data is a value, interned.** `ItemExtra {health = 1.0,
  enchantment = 0, name = ""}`. The session interns each distinct value
  once and refers to it by id; id 0 is "plain". Ids are not reused within a
  session; the table is saved.
- **Totals stay the truth for scripts.** `Inventory` keeps per owner and
  form the total count, as before, plus a split `extra id -> count` for the
  units that carry extra data; the rest of the total is plain. Every count
  query (natives, conditions, start inventories) sees the total. A removal
  that names no extra takes plain units first, then extras in id order;
  menus, transfers and drops name the stack (form, extra). Units with equal
  extras stack, as in Skyrim's menus.
- **Equipment remembers the extra** of what is equipped (each hand, ammo,
  each worn form).
- **Created forms have their own id range**, `created_forms`
  `0xFF700000..0xFF7FFFFF`, cut from the top of `runtime_refs` (no save
  reaches that far). A created potion is `{name, effects, value, weight,
  poison, model}` and is deduplicated by its effect list. Every item lookup
  asks the world's bases first, then the created forms.
- **Alchemy knowledge** is a 4-bit mask of known effects per ingredient.
- **Saves**: ITST version 3 adds the extras table, the splits, the
  equipped extras and the extras of dropped stacks; a new optional section `CRFT` holds the created forms and
  the alchemy knowledge. An older save loads with every unit plain, nothing
  created and nothing known.

## Consequences

- Scripts that move items without naming an extra (`RemoveItem`,
  `RemoveAllItems`) move plain units first, so a tempered weapon survives a
  quest taking "an iron dagger" if a plain one exists. Skyrim's own order is
  not measured.
- A dropped stack keeps its extra id beside its count (ITST version 3) and
  gets it back when picked up.
- Enchanting (C5, step 7) only fills `ItemExtra.enchantment` and `name`; no
  new storage is needed then.
