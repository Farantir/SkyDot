# Menus and SkyUI compatibility

Decided on 2026-11-01, before building. Covers SYSTEMS.md U3 and the UI part
of GOALS.md M4. Work starts after P6 (levelling), when the user starts it.
The research behind it is in `Projekte/skse-census/SKYUI-RESEARCH.md`
(outside the repo).

## Context

Skyrim's own menus are fine, except the item and magic menus. Nearly every
player replaces those with SkyUI, and many mods depend on SkyUI. SkyDot should
support those mods once SKSE support (M4) starts.

SkyUI's item, magic, barter, container, gift and favorites menus are not
programs that use an API. They are edited copies of Skyrim's own Flash (AS2)
menus. They rely on:

- a Scaleform runtime,
- Skyrim's undocumented menu protocol: about 75 `GameDelegate.call` names
  from menu to engine, 36 callbacks from engine to menu, and fixed
  `entryList` item fields,
- about 19 SKSE Flash functions (`ExtendData` adds formId, keywords and
  similar fields that SkyUI uses to sort items).

Running them would mean a Flash player (Ruffle is the only candidate, and it
has no Scaleform extensions) plus vanilla menu back-ends in the exact vanilla
data shapes. MCM works differently: mods talk to it through the Papyrus
script `SKI_ConfigBase` (about 35 functions and events), which drives the
"Journal Menu" with `UI.Invoke*`/`Set*` and receives `SKICP_*` mod events
back. That part can be served without Flash.

## Decision

- **SkyUI compatibility means mods that depend on SkyUI keep working, not
  SkyUI itself.** SkyDot never runs SkyUI's menus, and Flash is not run for
  them. SkyUI's menu SWFs are simply never loaded.
- **MCM**: the user's installed SkyUI Papyrus scripts (`SKI_ConfigBase`,
  `SKI_ConfigManager`, ...) run unchanged in the VM. SkyDot fakes the parts
  of the Journal Menu they address: `UI.*` calls on
  `_root.ConfigPanelFader.configPanel.*` are answered natively, a Godot panel
  draws the config page, and user input is sent back as the `SKICP_*` mod
  events. This keeps SkyUI's paging, state logic and version checks exact,
  and also covers mods that call the Journal Menu directly. It needs SkyUI
  installed, which players of SkyUI-dependent mods have anyway.
- **Menu events** (`RegisterForMenu`, `OnMenuOpen`/`OnMenuClose`, `UI.*`
  getters) are answered natively for SkyDot's own menus, under the vanilla
  menu names.
- **Widget mods** (`SKI_WidgetBase`) ship their own HUD SWFs. Not decided:
  they are researched when one matters. Ruffle is a candidate there, since
  widgets are small movies with a much simpler contract than the item menus.
- **View model**: every menu reads a view model (item list, categories,
  stats, actions) and never touches game state directly. Godot views draw
  it, in the flat window and the VR panel alike (ADR 0006). A later MCM
  layer or mod API plugs into the model, not into the views.
- **Registry**: tabs, columns, item actions (equip, drop, favorite, ...) and
  item card fields are registered as data, not hard-coded in views. SkyDot's
  own later entries and mods (M3) use the same registry.
- **One shared item list** for inventory, magic, container, barter, gift and
  the favorites menu:
  - horizontal tabs. Inventory: Favorites, Weapons, Apparel, Potions,
    Scrolls, Food, Ingredients, Books, Misc, Keys. Magic: Favorites,
    Alteration, Conjuration, Destruction, Illusion, Restoration, Shouts,
    Powers, Active Effects. Sub-tabs later.
  - a table with sortable columns that change per tab (for example Name,
    Damage, Weight, Value, Value/Weight for weapons; Name, Cost, School,
    Level for spells), a search box and an item card.
- **Favorites** as in the real game: a per-item flag that scripts see
  (`Game.IsObjectFavorited`) and that is saved, toggled with one key in any
  list; a Favorites tab in both menus; the quick favorites popup (items and
  spells) and hotkeys 1-8. Groups like SkyUI's come later. VR opens the
  popup from a controller button.
- **3D preview** on hover or selection, as in the real game: on a flat
  screen the item's model renders in its own small scene (`SubViewport`,
  own camera and light) next to the list, and can be spun and zoomed; in VR
  it is a real 3D object beside the panel. The world behind the menu is
  blurred in both. The model comes from the same path as dropped items.
- **Every other menu** (tween menu, skills, map, journal, dialogue,
  lockpicking, level-up, crafting, ...) follows the vanilla look and
  behaviour.
- **Godot native UI**: `Control` nodes and one shared `Theme`. The look is
  taken from the user's install (decided with the user 2026-11-14): the
  converter extracts fonts (`interface/fonts_*.swf`), icons, bitmaps and
  vector frames and gradients from the menu SWFs, so frames, icons and
  fonts match the game; free look-alikes only as a fallback when a pack
  lacks them. Keyboard, gamepad, mouse and VR pointer all work from the
  start.
- **Layout** (confirmed with the user 2026-11-14): inventory, magic,
  container, barter and gift use the SkyUI-style table layout above, drawn
  with the game's own graphics. Every other menu keeps the vanilla layout.

## Order of work

1. View model and registry; the existing inventory, magic and container
   views move onto the shared item list (tabs, columns, search, favorites).
2. SWF extraction in the converter (fonts, icons, bitmaps, vector frames),
   then the `Theme` built from them; mouse and VR pointer. Runs alongside 1.
3. The 3D preview.
4. The remaining vanilla menus as their systems arrive.

MCM and menu events follow with M4.

## Consequences

- SkyUI's look (its exact SWFs) is not available; its features are built in.
- Mods that ship their own Flash for MCM pages (`loadCustomContent`) or
  replace other vanilla SWFs (SkyHUD, dialogue and HUD overhauls) do not
  work. How common the MCM case is, is unknown.
- SkyUI files are never bundled; only the user's install is read. SkyUI's
  licence text was not verified (no LICENSE in its repository).
