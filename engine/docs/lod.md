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

- Terrain: the quad's own texture and its model-space normal map (NIF axes,
  carried to Godot's by the node transform). The flat LOD water shape gets a
  plain water colour.
- Objects: the atlas (or landscape texture for "HD" pieces) times the vertex
  colour, with the tangent-space normal map in the DirectX convention.
- Trees: one MultiMesh per quad of camera-facing billboards, sized by the tree
  list times the instance scale, cut from the atlas by its rectangle, alpha
  tested at 0.5.

Nothing casts shadows. Measured on the Riverwood flythrough (debug build):
median frame 1.7 ms without LOD, 3.3 ms with.

## Not checked against the game

- The model-space normal map's channel order (x east, y north, z up).
- Split distances and tree distance: chosen, not taken from the game's INI.
- LOD water colour (fixed, not the worldspace's WATR).
- Cracks where LOD terrain meets full terrain; none were visible around
  Riverwood.
- The LOD shaders are not in `warm_up`, so their first use may compile.
