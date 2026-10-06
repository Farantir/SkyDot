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
discards fragments over marked cells. Cells not yet built keep their LOD, so
streaming shows no gaps. Objects that cross a cell border are cut there.

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
