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

## Loading packs at runtime

There is no `.pck` and no import. `GLTFDocument.append_from_buffer` and
`generate_scene` build a model from a GLB's bytes (about 1 ms per Riverwood
mesh, against 1.7 ms to load a baked `.scn`), and `Image.load_dds_from_buffer`
keeps DDS block formats (cube maps are split into six faces for
`Cubemap.create_from_images`).

Two traps:

- `PackedScene.pack()` on a worker thread reads mesh data back through the
  RenderingServer, which waits for the main thread; with the main thread
  waiting for the worker, the viewer hung. The cache keeps the generated node
  tree as a template and duplicates it instead (`SkydotModel`).
- Headless runs use a renderer whose resource owners take no lock: two workers
  creating meshes at once fail with "Initializing already initialized RID".
  Headless, the cache loads on the calling thread.

A GLB without a binary chunk (780 of 33,235 vanilla SE meshes, all without
geometry) logs "Reading less data than requested" from the glTF reader; it is
harmless.

The cache keeps its own 1 to 4 `std::thread` workers: as `WorkerThreadPool`
tasks (4.7, 2026-10-05; one per asset, and as up to four looping tasks, with
`AssetCache::get` taking over unstarted work as before) loading the Riverwood
5x5 block through `request_exterior` took 5 to 9 % longer (median of 15 runs,
479 -> 501 ms and 468 -> 499 ms, 12 cores), while the viewer's
`--benchmark 20` and `preload_check.gd` came out the same.
