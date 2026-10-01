# Godot notes

Observations about the editor and the bindings, with version and date.

## Versions

| | |
| --- | --- |
| Editor | Godot 4.7.2 (`rc1` locally; `stable` checked for the abort below) |
| Bindings | godot-cpp `master` at the pinned commit, `GODOTCPP_API_VERSION=4.7` |
| `compatibility_minimum` | `4.7`, in `game/skydot.gdextension` |

godot-cpp `master` ships one `extension_api-4-N.json` per minor version and
builds the one named. There is no `4.7` branch, so a `master` commit is pinned.
The two version numbers above must match.

## The first headless scan crashes on exit (2026-09-12)

`godot --headless --path game --import` on a project without `.godot/`, whose
extension registers at least one class, completes the scan and writes
`.godot/extension_list.cfg`, then crashes with `SIGSEGV` during shutdown (exit
134). Running it again exits 0.

Seen with 4.7.2 rc1 and stable, official Linux x86_64 builds. godot-cpp's own
`test/project` reproduces it. No registered class: no crash. An empty
`_bind_methods` still crashes; `reloadable = false` does not help.

`tests/smoke/scan.cmake` therefore ignores the scan's exit status and checks
that the extension list names the extension. A scan that exits 0 without
writing the list is still a failure.

Not reported upstream or bisected yet. Possibly related:
[#98062](https://github.com/godotengine/godot/issues/98062).

## Extensions load from `.godot/extension_list.cfg`

A headless `--script` run loads whatever the last editor scan listed in
`.godot/extension_list.cfg` and does not scan itself, so a fresh checkout sees
no extension until the editor has run once. Hence the scan is a ctest fixture.
(`project.godot`'s `[native_extensions] paths=` is a 4.0-era key that is no
longer read.)

## `--script` accepts an absolute path

`godot --headless --path game --script /abs/path/to/test.gd` runs a script
outside the project with `res://` still at `game/`. The smoke tests use this so
`tests/` stays outside the project.

## Static libstdc++ by default

Without hot reload, godot-cpp links `-static-libstdc++ -static-libgcc`
(`cmake/linux.cmake`, `GODOTCPP_USE_STATIC_CPP`). On Fedora without
`libstdc++-static` the link fails (`cannot find -lstdc++`). The `linux-release`
preset turns it off; distribution builds should use
`-DGODOTCPP_USE_STATIC_CPP=ON` and install the package.

## Mounting a `.pck` over `res://`

`SkydotPack.mount_baked` calls `ProjectSettings.load_resource_pack`. Scenes
load by the virtual path the bake used (`res://meshes/testpack/cube_se.scn`),
with materials and textures, in a project that did not build them:
`scenes=4 surfaces=4 albedo=4 normals=4` on the test pack, matching bethconv's
`tools/bake/verify.gd`.
