# bethconv's JSON output

What the pack tool (`engine/game/packtool/`) and any other front end read.
Every document is one line on stdout. With `--json`, `convert` prints only JSON
lines on stdout and its human-readable text on stderr; read both pipes, or the
converter stalls when stderr fills up.

Every document carries `json_version` (now 1). It goes up when a key changes
meaning or disappears; new keys do not change it. A failing command prints
`{"json_version": 1, "error": "..."}` and exits non-zero; a command line
bethconv does not understand exits 106 (CLI11) without JSON.

Paths are UTF-8; bytes that are not valid UTF-8 or are control characters are
percent-encoded (`io::path_text`). Messages are free text and may contain
newlines.

## `bethconv detect --json`

Skyrim installs on this machine (`install/game_install.hpp`): Steam libraries
from `libraryfolders.vdf` and the app manifests; on Windows also Steam's and
the Bethesda launcher's registry keys.

```json
{"json_version": 1,
 "steam_roots": ["/home/u/.local/share/Steam"],
 "local_app_data": "",
 "installs": [
  {"edition": "se", "edition_name": "Skyrim Special Edition", "source": "steam",
   "root": ".../common/Skyrim Special Edition", "data": ".../Skyrim Special Edition/Data",
   "app_id": "489830", "build_id": "24914197",
   "tested_build_id": "24914197", "build_tested": true,
   "plugins_txt": ".../compatdata/489830/pfx/.../Skyrim Special Edition/Plugins.txt",
   "plugins_txt_expected": "..."}]}
```

- `edition`: `le`, `se` (also Anniversary), `vr` or `unknown`.
- `source`: `steam`, `registry` or `folder`.
- `build_tested`: whether `build_id` is the build the corpus harness last
  passed against (`tested_build_id`); `null` if either is unknown.
- `plugins_txt`: the game's own list if it exists, else `null`;
  `plugins_txt_expected` says where the game would write it (under Proton,
  inside the app's prefix).

## `bethconv mo2 <instance> [--profile P] --json`

A Mod Organizer 2 instance (`install/mo2.hpp`) and one profile, the selected
one by default.

```json
{"json_version": 1, "dir": "/mods/FUS", "game_name": "Skyrim VR", "edition": "vr",
 "game_path_written": "D:\\SteamLibrary\\steamapps\\common\\SkyrimVR",
 "game_path": null, "game_data": null,
 "selected_profile": "FUS RO DAH (Basic + Appearance + Gameplay)",
 "profiles": ["CANGAR", "FUS (Basic)", "..."],
 "mods_dir": "/mods/FUS/mods", "overwrite_dir": "/mods/FUS/overwrite",
 "profile": {"name": "FUS (Basic)", "mods_enabled": 131, "mods_disabled": 409,
             "separators": 76, "unmanaged": 0, "missing": [],
             "plugins_file": ".../profiles/FUS (Basic)/plugins.txt", "plugins_listed": 248,
             "plugins_active": 247}}
```

`game_path` is `null` when the instance's game folder is not on this machine
(a list installed on Windows, read on Linux); `convert --mo2` then needs
`--data`. `profile` is `null` with a `profile_error` when the profile cannot be
read; the exit code is then 1.

## `bethconv target <dir> --json`

Whether `dir` is a good place for a pack. Nothing is written.

```json
{"json_version": 1, "path": "/packs/se", "exists": true, "is_pack": true, "empty": false,
 "free_bytes": 1368594378752,
 "filesystem": {"mount_point": "/", "fs_type": "ext4", "source": "/dev/nvme1n1p1",
                "disk": "nvme1n1", "fuse": false, "rotational": false, "zoned": "none"},
 "blob":  {"verdict": "ok", "reason": ""},
 "loose": {"verdict": "ok", "reason": ""}}
```

`verdict` is `ok`, `warn` (bethconv proceeds and warns) or `refuse` (bethconv
stops unless given `--allow-slow-target`), for each asset layout.
`filesystem` is `null` where nothing could be probed (everywhere but Linux).

## `bethconv info <pack> --json`

```json
{"json_version": 1, "path": "/packs/se", "plugins": 10,
 "manifest": {"pack_format_version": 5, "converter": "bethconv 0.0.1", "input": {...}, ...},
 "disk": {"files": 6, "bytes": ...},
 "blob": {"bytes": ..., "entries": ..., "live_entries": ..., "live_bytes": ...,
          "stale_bytes": ...}}
```

`manifest` is `manifest.json` without `load_order` and `source_hashes`.
`blob` is `null` for a loose pack. `stale_bytes` are blob bytes no `vpath.idx`
entry uses any more (reconversions leave them); `convert --prune` removes them.

## `bethconv cell <pack> --worlds --json`, `--list --json`

```json
{"json_version": 1, "worlds": [{"id": 60, "editor_id": "Tamriel", "parent": 0,
  "uses_parent_land": false, "bounds": [-233472.0, -176128.0, 253952.0, 208896.0]}]}
{"json_version": 1, "total": 73725, "cells": [{"id": 91560, "editor_id": "WhiterunBreezehome",
  "interior": true, "refs": 530}]}
```

Bounds are game units (min x, min y, max x, max y). `--list` names only cells
with an editor id.

## `bethconv convert ... --json`

Events, one per line, in this order:

1. `progress` with phase `mount`, once per mounted archive or folder (an MO2
   profile on a slow disk takes most of a minute).
2. `start`: what is about to be converted.
3. `progress` for the phases `merge` (start and end only), `world`,
   `assets` (every 250 files), `finish` (hashing plugins, writing indexes).
4. `done`, or `error` at any point.

```json
{"event": "start", "json_version": 1, "out": "/packs/fus", "target_warning": "",
 "input": {"kind": "mo2", "edition": "vr", "data": ".../SkyrimVR/Data",
           "plugin_list": ".../plugins.txt", "mo2_instance": "/mods/FUS",
           "mo2_profile": "FUS RO DAH (...)", "mods": 426},
 "plugins": 295, "load_order_problems": [], "sources": 494, "mount_failures": [],
 "unloaded_archives": [".../UHDAP - MusicHQ.bsa"], "unique_paths": 231219}
{"event": "progress", "phase": "assets", "done": 2500, "total": 3000, "elapsed": 120.7}
{"event": "done", "json_version": 1, "exit": 0, "elapsed": 121.3, "jobs": 12, "out": "/packs/fus",
 "forms": 1274726, "cells": 74028, "refs": 931207, "bases": 35822,
 "assets": {"written": ..., "deduped": ..., "distinct": ..., "meshes": ..., "textures": ...,
            "scripts": ..., "lod": ..., "bytes_written": ..., "store_bytes": ...},
 "textures": {"max_size": 0, "shrunk": 0, "kept_large": 0, "uncompressed": "keep",
              "encoded": 0, "not_encoded": 0, "bytes_saved": 0},
 "failed": 0, "warnings": 0, "orphaned_assets": 0, "pruned": false,
 "first_failures": [{"vpath": "...", "stage": "mesh", "detail": "..."}]}
{"event": "error", "json_version": 1, "message": "...", "exit": 2}
```

- `elapsed` is seconds since the command started.
- `jobs`: the threads that read, hashed and converted assets (`--jobs`; the
  default is every core, at most 64). The pack does not depend on it. The
  `assets` progress events count inputs whose results the writer has taken, in
  work-list order, so they still advance steadily.
- `forms`, `cells`, `refs` and `bases`: the merged load order's forms and the
  counts in `world.fb`; all 0 with `--no-world`.
- `unloaded_archives`: archives in mod folders that no loaded plugin is named
  after; the game does not load them, so they are not mounted
  (`install/mount_plan.hpp`).
- `textures`: with `--max-texture-size`, how many textures lost top mip
  levels and how many stayed larger (no smaller level stored; each is a
  warning in `report.json`); with `--encode-uncompressed`, how many
  uncompressed textures were block-compressed and how many were left so
  (cubemaps, volumes; also warnings); `bytes_saved` counts both.
- `exit` 1 in `done` means some files failed (they are in `report.json`); the
  pack is still written. Exit 2 is a refused output folder.

## Inputs

`convert --data <Data>` mounts that folder (every archive in it, then its
loose files). `--list` names the load order; without it the game's rule
applies (implicit masters, then by modification time). With a list, the
Creation Club plugins in the install's `Skyrim.ccc` load right after the
masters, as in the game.

`convert --data <Data> --mo2 <instance> [--profile P]` mounts the game's Data
folder under the profile's mods and the overwrite folder; plugins and their
archives come from the mod folders in the profile's priority, and the load
order from the profile's `plugins.txt`. The manifest's `input` key records
which (`formats/pack-format.md`).
