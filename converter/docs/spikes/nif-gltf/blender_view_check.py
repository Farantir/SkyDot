# Counts the images Blender actually found in a `bethconv view` tree.
#
#   blender --background --factory-startup --python blender_view_check.py -- <dir>
#
# blender_check.py counts TEX_IMAGE nodes, which exist even when the file is
# missing. Image counts here are per datablock, so they will not match Godot's
# per-slot totals.
import bpy, sys, glob, os, json, collections

root = sys.argv[sys.argv.index('--') + 1]
files = sorted(glob.glob(os.path.join(root, '**', '*.glb'), recursive=True))
tot = collections.Counter()
failed = []
for f in files:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    try:
        bpy.ops.import_scene.gltf(filepath=f)
    except Exception as e:  # noqa: BLE001
        failed.append((os.path.relpath(f, root), str(e)[:120]))
        continue
    tot['files'] += 1
    for m in bpy.data.materials:
        if not m.use_nodes:
            continue
        tot['materials'] += 1
    for img in bpy.data.images:
        if img.name == 'Render Result':
            continue
        tot['images'] += 1
        # `has_data` stays False in background mode until a load is forced, so
        # check the path and call reload().
        path = bpy.path.abspath(img.filepath)
        if path and os.path.exists(path):
            tot['images_resolved'] += 1
        else:
            tot['images_missing'] += 1
        try:
            img.reload()
            if img.size[0] > 0 and img.size[1] > 0:
                tot['images_decoded'] += 1
        except Exception:  # noqa: BLE001
            pass
tot['failed'] = len(failed)
print("BLENDER-RESULT " + json.dumps(dict(tot), sort_keys=True))
for f, e in failed[:10]:
    print("FAILED", f, e)
