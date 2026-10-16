# SkyDot Script Mod Exception

Additional permission under section 7 of the GNU General Public License,
version 3 ("GPL"), granted by the copyright holders of SkyDot.

## Definitions

"SkyDot" means the SkyDot engine: the code under `engine/` in this
repository, including its Godot project, released under the GPL.

"Mod API" means the versioned mod interface SkyDot publishes for mods
(`SkydotModAPI`) together with the published mod formats (manifest, record
format and resource pack layout).

"Script Mod" means a work consisting only of interpreted scripts (such as
GDScript) and data (such as scenes, models, textures, sounds and records)
that SkyDot loads at run time and that interacts with SkyDot only through the
Mod API. A Script Mod contains no native code (no GDExtension, shared library
or other compiled machine code), and contains no part of SkyDot, or a work
based on SkyDot, other than what the Mod API requires to name its interfaces.

## Permission

You may convey a Script Mod under terms of your choice, even where a Script
Mod and SkyDot together would otherwise form a work based on SkyDot under the
GPL. This permission does not extend to:

- SkyDot itself, or any modified version of it, which stays under the GPL;
- native code loaded into SkyDot (for example GDExtension libraries), which
  is covered by the GPL without this exception;
- the converter under `converter/`, which this exception does not cover.

## Removing the exception

When you modify SkyDot, you may extend this exception to your version, but
you are not obliged to. If you do not wish to, delete this exception
statement from your version.
