# NIF → glTF fixtures

Scripts to reproduce [the NIF → glTF spike](../nif-gltf.md). Throwaway code; they
only operate on trees the converter produced from a local install.

```sh
B=build/linux-debug-asan/tools/bethconv-cli/bethconv
SE="$SL/Skyrim Special Edition/Data"

# 1. convert a stratified sample
for f in actors/ armor/ weapons/ architecture/ clutter/ \
         landscape/ plants/ dungeons/ effects/ furniture/; do
    $B mesh --source "$SE/Skyrim - Meshes0.bsa" --source "$SE/Skyrim - Meshes1.bsa" \
            --filter "$f" --limit 35 -o proj
done

# 2. extract the textures those GLBs reference
python3 referenced_textures.py proj > textures.txt
$B extract --source "$SE/Skyrim - Textures"{0,1,2,3,4,5,6,7,8}.bsa \
           --from textures.txt -o proj -q

# 3. complete mip chains so Godot loads them
python3 fix_mip_tail.py proj/textures

# 4. check with both consumers
godot4.7 --headless --path proj --import
godot4.7 --headless --path proj --script verify_scenes.gd
blender --background --factory-startup --python blender_check.py -- proj/meshes
```

`bethconv view` now does steps 1–3 in one command.

`blender_view_check.py` runs over a `bethconv view` tree and checks that Blender
actually found each referenced texture, which `blender_check.py` does not.

`fix_mip_tail.py` is superseded by `src/texture/` and must not be used on real
trees: it corrupts cubemaps. Use `bethconv texture`.
