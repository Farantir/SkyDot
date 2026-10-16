# Script mods may use any licence; native mods stay GPL

SkyDot is GPL-3.0-or-later, and code mods that call into the engine (GDScript
against `SkydotModAPI`) would likely form a combined work with it and have to
be GPL as well. Skyrim's mod culture is mostly all-rights-reserved, so we grant
a GPL §7 additional permission (`LICENSE-EXCEPTION.md`): mods made only of
scripts and data that use only the Mod API may be licensed freely. Native mods
(GDExtension, C++) get no exception: whoever needs native functionality should
contribute it to the engine instead. This was decided on 2026-10-16, while
the project still had a single copyright holder; adding the exception later
would need every contributor's consent.

## Considered options

- **Plain GPL, no exception:** keeps the whole ecosystem free, but deters most
  mod authors, and Data mods (assets, plugins) never needed it anyway.
- **Exception for native mods too:** rejected; it would recreate the SKSE
  situation of closed native plugins that the engine cannot fix or port
  (ARM, VR). Native features belong in the engine.
- **API facade under MIT/LGPL:** messy, since the facade calls into GPL code.

## Consequences

- The converter (`converter/`) links nifly (GPL-3) and is not covered; mods
  never load into it.
- Before M3 ships, the exception text should get a check by someone with
  legal training.
