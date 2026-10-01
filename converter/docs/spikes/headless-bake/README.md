# Headless-bake fixtures

Scripts to reproduce [the headless-bake spike](../headless-bake.md) on other
hardware or Godot versions. Throwaway code; no game data.

```sh
python3 gen_png.py  src_png 60          # source textures, Skyrim-like sizes
python3 gen_gltf.py glb 1000 src_png    # synthetic .glb statics
basisu -ktx2 -mipmap -output_path ktx2 src_png/*.png
godot --headless --import               # in a project with these under res://
```

`dds2ktx2.py` tries a lossless DDS → KTX2 rewrap. Godot does not accept its
output yet; see the open question in the write-up.
