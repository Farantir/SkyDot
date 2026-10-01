# Navigation

Skyrim's own navmeshes (NAVM), converted rather than baked again: the
converter decodes NVNM into `world.fb` (`../formats/pack-format.md`), and
`SkydotWorld` gives every built cell one `NavigationRegion3D` per navmesh
under a node named `Navmesh`. `navigation = false` (viewer:
`--navigation off`) builds none.

## Regions (`world/navmesh.*`)

- Vertices are converted like everything else (`coordinates.md`); triangles
  keep their order. Every vanilla triangle faces up (counter-clockwise seen
  from above, 489 of 2.67 M degenerate), which is the winding Godot's path
  funnel expects; the reverse gives doubled corner points.
- Portals: an edge flagged as a link names a triangle in another navmesh,
  nearly always the next cell's. The two edges lie side by side but rarely
  share vertices: across, 97% are within 4 units; vertically they are up to
  64 units (0.9 m) apart. Godot joins free edges of different regions within
  the map's edge connection margin, set to 1 m in `project.godot`, so no link
  objects are needed. Around Riverwood (7x7 cells) all 1,237 portals join, in
  WhiterunWorld (9x9) all 274 (`game/tools/nav_check.gd`).
- Ledges: link types 1 and 2 come in equal numbers and join edges far apart.
  Each becomes a one-way `NavigationLink3D` from the edge's middle to the
  target triangle's centre (only when the target navmesh is known).
- The map's cell size is 0.01 m (also in `project.godot`), so nearby vertices
  are not merged; a navigation mesh takes the same value. About 1.6% of
  vanilla navmeshes have edges shared by three or more triangles; Godot
  skips the extra connection, and its warning is turned off.
- Triangle flags (water, door, preferred) and cover are in `world.fb` but not
  used yet; `get_navmesh` reports water counts, links and door triangles.

## Cost

Building the regions of Riverwood's 25 cells (11,256 triangles) adds about
7 ms to building the cells. The navigation map takes new regions in on its
next synchronization without a longer frame. Exterior navmeshes are all in
their own cells; none sit in a worldspace's persistent cell.

## Not done

- Load doors: door triangles are known (`get_navmesh(...)["doors"]`), but no
  path leads through a load door into another cell or worldspace.
- The search grid NVNM ends with, the cover triangle list, NAVI (preferred
  paths, islands) and ONAM/PNAM/NNAM are not used.
- Nothing walks the paths yet: there are no actors.
