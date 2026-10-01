import bpy, sys, glob, os, json, collections

root = sys.argv[sys.argv.index('--') + 1]
files = sorted(glob.glob(os.path.join(root, '**', '*.glb'), recursive=True))
tot = collections.Counter()
failed = []
uv_missing = []
no_normals = []
extras_seen = 0
for f in files:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    try:
        bpy.ops.import_scene.gltf(filepath=f)
    except Exception as e:                      # noqa: BLE001
        failed.append((f, str(e)))
        continue
    tot['files'] += 1
    for ob in bpy.data.objects:
        if ob.type != 'MESH':
            continue
        me = ob.data
        tot['meshes'] += 1
        tot['verts'] += len(me.vertices)
        tot['tris'] += len(me.loop_triangles) or sum(len(p.vertices) - 2 for p in me.polygons)
        if not me.uv_layers:
            uv_missing.append(f + '::' + ob.name)
        if not me.polygons:
            no_normals.append(f + '::' + ob.name)
        tot['materials'] += len(me.materials)
        for m in me.materials:
            if m and m.use_nodes:
                for n in m.node_tree.nodes:
                    if n.type == 'TEX_IMAGE':
                        tot['image_nodes'] += 1
        if ob.vertex_groups:
            tot['skinned'] += 1
    for ob in bpy.data.objects:
        if ob.type == 'ARMATURE':
            tot['armatures'] += 1
            tot['bones'] += len(ob.data.bones)
        if ob.get('bethconv') is not None or 'bethconv' in ob.keys():
            extras_seen += 1

print('BLENDER_RESULT ' + json.dumps({
    'files_seen': len(files), 'imported': tot['files'], 'failed': len(failed),
    'meshes': tot['meshes'], 'verts': tot['verts'], 'materials': tot['materials'],
    'image_nodes': tot['image_nodes'], 'skinned_objects': tot['skinned'],
    'armatures': tot['armatures'], 'bones': tot['bones'],
    'objects_with_bethconv_extras': extras_seen,
    'meshes_without_uv': len(uv_missing), 'meshes_without_faces': len(no_normals),
}))
for f, e in failed[:10]:
    print('BLENDER_FAIL', f, e)
