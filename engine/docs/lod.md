# Distant LOD

`SkydotLod` (`extension/src/render/lod.*`) shows a worldspace's LOD beyond the
cells loaded at full detail: the game's terrain LOD (`.btr`), object LOD
(`.bto`) and tree LOD (`.btt` with the `.lst` billboard list), as the
converter carries them (`../formats/pack-format.md`, "LOD").

## Quads

The LOD settings give a grid (for Tamriel 256 cells from (-96, -96)) split into
quads of 4, 8, 16 and 32 cells. Each update walks it as a quadtree from 32
down: a quad of L cells splits while the camera is within `split_distance`
(1.5) times L cells of it. The quads it ends on show their terrain and
objects; level-4 quads within `tree_distance` (16 cells) show their trees. A
quad stays until everything that replaces it is built, so moving never opens
holes. Scenes load on the loader's threads; building is limited per frame.

Around Riverwood that is 44 level-4, 49 level-8, 41 level-16 and 50 level-32
quads, 114 with object LOD and about 7,700 trees.

## Hiding under loaded cells

LOD and full-detail cells overlap. The viewer marks each cell it has built
(`set_cell_loaded`) in a one-byte-per-cell mask texture; every LOD shader
discards fragments over cells marked 1. Cells not yet built keep their LOD, so
streaming shows no gaps. Objects that cross a cell border are cut there.

The mask has a second state, 0.5: the cell's large references are drawn as
models (next section), so only the object shapes made of them are discarded.

## Large references

The game draws, from the full-detail cells (uGridsToLoad 5, radius 2) out to
uLargeRefLODGridSize (11, radius 5), the references the Creation Kit lists as
large (bounds above fLargeRefMinSize: cliffs, rocks, big buildings) as full
models, seen only. Beyond that only object LOD shows them. Without this layer
rocks and cliffs a cell or two outside the full-detail radius were missing.

`SkydotLargeRefs` (`render/large_refs.*`, a child of `SkydotLod`) reads them
from pack format 11 (`world.fb` per worldspace `large_refs`, with `large_cells`
holding for each grid square the references standing in it or reaching into
it; an older pack has none and the layer is not made). Each update:

1. `large_refs::select` (`data/large_refs.*`, pure): the union of the lists of
   the squares within `radius` (Chebyshev) of the camera's square, each
   reference once, minus those whose own square is built at full detail (that
   cell builds them: they are ordinary references of their cell). Squares inside
   the full-detail radius are in the union on purpose: a reference stays drawn
   until its cell is built, so moving opens no holes.
2. References that draw nothing are skipped and counted: initially disabled
   (own flag, or the enable parent's state, as the cell builder evaluates it,
   found through the parent's cell), no base, base without a model, editor
   markers.
3. Models load on the asset cache's threads, all asked for at once; instances
   are built nearest first within 2 ms a frame, through the cell builder's
   decoration (`Decoration::visual_only`: root transform, draw order,
   materials, projected snow material, billboards, animated textures; no
   collision, lights, add-ons, scripts or tags), so they look as in a full
   cell. Shadows are off (the game casts none from distant objects either).
4. The LOD hides what object LOD already draws of them. In the vanilla `.bto`
   the large references with the visible-when-distant flag are separate shapes
   named `obj-LargeRef`, `objHD-LargeRef` or `objsnowHD-LargeRef` (others
   `Obj`, `Obj2`, ...). Those shapes get a material with `large_ref_shape` set
   and discard where the mask is 0.5 or 1. A cell is marked 0.5 once every
   reference in its list is built, is not drawable, or stands in a built cell.

Not hidden: a LargeRef shape that spills into a cell just beyond the radius
whose own list is not drawn (a one-cell fringe, at most a sliver of coincident
surfaces). `--large-refs off` and `--large-ref-radius N` (viewer),
`SkydotStreamer.large_refs` / `large_ref_radius` and `SkydotLod.large_refs` /
`large_ref_radius` control it; `get_stats()` of the LOD has a `large_refs`
entry (wanted, shown, pending, marked cells, skipped by reason).
`SkydotWorld.get_large_refs(world, x, y, radius)` is the query.

## Shading

- Terrain: the quad's own texture and its model-space normal map. Its NIF
  normal is (red, blue, green) * 2 - 1: "up" is in green. Found by comparing
  the map with the mesh's vertex normals over five quads and every channel
  order and sign (mean dot 0.79 to 0.91, runner-up 0.57 to 0.74; not higher
  because the mesh is decimated), and carried to Godot's axes by the node
  transform, times a detail term from `textures/terrain/noise.dds` (the game's
  tiling grey noise, mean 0.215; the shader keeps the mean and applies half of
  its deviation, one tile per 128 m: chosen, the game's weight and scale are
  unknown). The flat LOD water shape gets a
  plain water colour.
- Objects: the atlas (or landscape texture for "HD" pieces) times the vertex
  colour, with the tangent-space normal map in the DirectX convention (the
  maps are blue-dominant, mean (0.44, 0.44, 0.84), and the meshes carry
  tangents, so they are tangent-space; the green sign is not measured).
- Trees: one MultiMesh per quad of camera-facing billboards, sized by the tree
  list times the instance scale, cut from the atlas by its rectangle, alpha
  tested at 0.5.

Nothing casts shadows. Measured on the Riverwood flythrough (debug build):
median frame 1.7 ms without LOD, 3.3 ms with.

## Not checked against the game

- Brightness. Lit by the same sun and ambient as full terrain, LOD terrain
  comes out about 1.2 to 1.6 times brighter and warmer than full terrain's
  texture times vertex colour (unlit, over Whiterun's plains: LOD 87, 76, 48
  against 56, 54, 39 of 255; lit the factors agree), so the border shows a
  step. No correction is applied: it may be the LOD textures' own baking, and
  the only game image (the ref-18 view) is dominated by fog, which hides it
  (the plains there are 145, 168, 160 here against 139, 148, 133 in the game,
  and the horizon haze 153, 189, 207 against 154, 179, 188: the fog far colour
  or amount is bluer or heavier than the game's).

- Split distances and tree distance: chosen, not taken from the game's INI.
- LOD water colour (fixed, not the worldspace's WATR).
- Cracks where LOD terrain meets full terrain; none were visible around
  Riverwood.
- The LOD shaders are not in `warm_up`, so their first use may compile.
