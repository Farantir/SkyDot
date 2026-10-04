# SPDX-License-Identifier: GPL-3.0-or-later
#
# The scene oracle: builds an interior and/or a block of exterior cells with
# the engine's defaults, lets a few frames pass in the tree, and prints a
# canonical text dump of every node under the built roots. Two dumps of the
# same pack differ only if the built scenes differ, so a refactor of the
# engine proves itself by diffing before and after.
#
#   godot4.7 --headless --path game --script res://tools/scene_dump.gd -- \
#       --pack <pack> [--cell EDITOR_ID] \
#       [--world NAME --x X --y Y [--radius R]] [--frames 5] [--out FILE]
#
# One line per node, sorted by path: path, child index, class, the properties
# that differ from the class default, and a summary of its mesh, material,
# light or collision shapes. Resources (materials, meshes, shapes, textures)
# are summarised once in a table at the end, by a hash of their content, and
# nodes refer to them by that hash. Nothing in it depends on time, instance
# ids or the order of dictionaries. Names Godot makes up for siblings
# ("@Node3D@12") are numbered by a global counter, so the number is dropped.
#
# What would move from frame to frame or run to run is held still: the tree is
# paused as the roots enter it, so animators, flickers (both read the clock)
# and actors (random wandering) never run a frame, and a node is dumped as
# its _ready left it. AnimationPlayer positions and skeleton poses are not
# dumped (their presence and every other property are).
extends SceneTree

## Properties left out, by name prefix: those that depend on the frame or the
## clock, and node metadata (dumped on its own).
const SKIPPED := ["current_animation_position", "assigned_animation", "bones/", "frame_progress", "metadata/"]

var _lines: PackedStringArray = []
var _table := {}
var _by_object := {}


func _initialize() -> void:
    var argv := OS.get_cmdline_user_args()
    var args := {"--radius": "1", "--frames": "5"}
    for i in range(0, argv.size() - 1):
        args[argv[i]] = argv[i + 1]
    if not args.has("--pack") or not (args.has("--cell") or args.has("--world")):
        printerr("scene_dump: --pack and --cell or --world are required")
        quit(2)
        return
    _run(args)


func _run(args: Dictionary) -> void:
    # Not before the tree runs: nodes added in _initialize get _ready later.
    await process_frame
    var pack := SkydotPack.new()
    if pack.open(args["--pack"]) != OK:
        printerr("scene_dump: ", pack.get_error())
        quit(1)
        return
    var world := pack.open_world()
    var roots := {}
    if args.has("--cell"):
        var cell: int = world.find_cell(args["--cell"])
        if cell == 0:
            printerr("scene_dump: no such cell")
            quit(1)
            return
        await _wait_for(func() -> bool: return world.request_cell(cell) == 0)
        var node: Node3D = world.build_cell(cell)
        if node == null:
            printerr("scene_dump: cell did not build")
            quit(1)
            return
        roots["cell " + args["--cell"]] = node
    if args.has("--world"):
        var ws: int = world.find_world(args["--world"])
        if ws == 0:
            printerr("scene_dump: no such worldspace")
            quit(1)
            return
        var radius := int(args["--radius"])
        var keys: Array[Vector2i] = []
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                keys.append(Vector2i(int(args["--x"]) + dx, int(args["--y"]) + dy))
        await _wait_for(func() -> bool:
            var pending := 0
            for key in keys:
                pending += world.request_exterior(ws, key.x, key.y)
            return pending == 0)
        for key in keys:
            var node: Node3D = world.build_exterior(ws, key.x, key.y)
            var label := "exterior %s %d,%d" % [args["--world"], key.x, key.y]
            if node == null:
                _lines.append("%s: none (cell 0x%08x)" % [label, world.get_exterior_cell(ws, key.x, key.y)])
                continue
            roots[label] = node
    for label in roots:
        root.add_child(roots[label])
    paused = true
    # Deferred work after _ready (particles, navigation) happens here.
    for i in int(args["--frames"]):
        await process_frame
    for label in roots:
        _dump_root(label, roots[label])
    _lines.sort()
    for key in _table.keys():
        _lines.append("resource %s: %s" % [key, _table[key]])
    var text := "\n".join(_lines) + "\n"
    if args.has("--out"):
        var file := FileAccess.open(args["--out"], FileAccess.WRITE)
        file.store_string(text)
        file.close()
    else:
        print(text)
    print("scene_dump: %d lines, %d resources" % [_lines.size(), _table.size()])
    for label in roots:
        roots[label].free()
    quit(0)


## Call `done` each frame until it is true; fails after a minute.
func _wait_for(done: Callable) -> void:
    var started := Time.get_ticks_msec()
    while not done.call():
        if Time.get_ticks_msec() - started > 60000:
            printerr("scene_dump: timed out waiting for resources")
            quit(1)
            return
        await process_frame


func _dump_root(label: String, top: Node) -> void:
    var stack: Array[Node] = [top]
    while not stack.is_empty():
        var node: Node = stack.pop_back()
        _lines.append(_node_line(label, top, node))
        stack.append_array(node.get_children())


func _node_line(label: String, top: Node, node: Node) -> String:
    var path := str(top.get_path_to(node))
    path = _unname(path)
    var parts: PackedStringArray = [label + ":" + path, "#%d" % node.get_index(), node.get_class()]
    if node.get_script() != null:
        parts.append("script=" + str(node.get_script().resource_path))
    parts.append("process_mode=%d" % node.process_mode)
    if node is Node3D:
        parts.append("visible=%s" % node.visible)
        parts.append("transform=" + _fmt(node.transform))
    elif node is CanvasItem:
        parts.append("visible=%s" % node.visible)
    parts.append_array(_changed_properties(node))
    if node is MeshInstance3D:
        parts.append_array(_mesh_instance(node))
    elif node is Light3D:
        parts.append_array(_light(node))
    elif node is CollisionObject3D:
        parts.append_array(_body(node))
    elif node is CollisionShape3D:
        parts.append("shape=" + _resource(node.shape))
    elif node is MultiMeshInstance3D:
        parts.append("multimesh=" + _resource(node.multimesh))
    elif node is NavigationRegion3D:
        parts.append("navmesh=" + _resource(node.navigation_mesh))
    elif node is AnimationPlayer:
        parts.append("animations=%s" % ",".join(node.get_animation_list()))
        parts.append("playing=%s" % node.is_playing())
    elif node is Skeleton3D:
        parts.append("bones=%d" % node.get_bone_count())
    for key in _sorted(node.get_meta_list()):
        var value: Variant = node.get_meta(key)
        # The converter's extras are big and say where a model came from, not
        # what was built.
        parts.append("meta:%s=%s" % [key, "<%d keys>" % value.size() if key == "extras" else _fmt(value)])
    return " | ".join(parts)


## "@MeshInstance3D@12" -> "@MeshInstance3D@".
func _unname(path: String) -> String:
    var regex := RegEx.new()
    regex.compile("@([^@/]*)@\\d+")
    return regex.sub(path, "@$1@", true)


## Storage properties whose value is not the class default, sorted by name.
func _changed_properties(object: Object) -> PackedStringArray:
    var out: PackedStringArray = []
    var cls := object.get_class()
    var skip := ["name", "transform", "visible", "process_mode", "script", "owner", "scene_file_path",
        "resource_path", "resource_name"] if object is Node else ["resource_path", "script"]
    var list := object.get_property_list()
    list.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return a["name"] < b["name"])
    for info in list:
        var prop: String = info["name"]
        if not (info["usage"] & PROPERTY_USAGE_STORAGE) or prop in skip or _skipped(prop) \
                or (object is ShaderMaterial and prop.begins_with("shader_parameter/")):
            continue
        var value: Variant = object.get(prop)
        var default: Variant = ClassDB.class_get_property_default_value(cls, prop)
        if is_same(value, default) or (typeof(value) == typeof(default) and typeof(value) != TYPE_OBJECT
                and _fmt(value) == _fmt(default)):
            continue
        if value is Object and (value as Object).get_class() in ["Node", "Node3D"]:
            continue
        out.append("%s=%s" % [prop, _fmt(value)])
    return out


func _skipped(prop: String) -> bool:
    for prefix in SKIPPED:
        if prop.begins_with(prefix):
            return true
    return false


func _mesh_instance(mi: MeshInstance3D) -> PackedStringArray:
    var out: PackedStringArray = ["mesh=" + _resource(mi.mesh)]
    if mi.mesh != null:
        var mats: PackedStringArray = []
        for i in mi.mesh.get_surface_count():
            var mat: Material = mi.get_active_material(i)
            mats.append(_resource(mat))
        out.append("surfaces=[%s]" % ", ".join(mats))
    return out


func _light(light: Light3D) -> PackedStringArray:
    var out: PackedStringArray = ["color=" + _fmt(light.light_color), "negative=%s" % light.light_negative,
        "shadow=%s" % light.shadow_enabled]
    out.append("energy=" + _fmt(light.light_energy))
    if light is OmniLight3D:
        out.append("range=" + _fmt(light.omni_range))
    elif light is SpotLight3D:
        out.append("range=" + _fmt(light.spot_range))
        out.append("angle=" + _fmt(light.spot_angle))
    return out


func _body(body: CollisionObject3D) -> PackedStringArray:
    var shapes: PackedStringArray = []
    for child in body.get_children():
        if child is CollisionShape3D:
            shapes.append(child.shape.get_class() if child.shape != null else "none")
    shapes.sort()
    return ["layer=%d" % body.collision_layer, "mask=%d" % body.collision_mask,
        "shapes=[%s]" % ",".join(shapes)]


# ---- values -------------------------------------------------------------

func _sorted(list: Array) -> Array:
    var out := list.duplicate()
    out.sort()
    return out


## A float at 1e-4, without "-0".
func _num(x: float) -> String:
    if is_nan(x):
        return "nan"
    if is_inf(x):
        return "inf" if x > 0.0 else "-inf"
    var r := snappedf(x, 0.0001)
    if r == 0.0:
        r = 0.0
    return "%.4f" % r


func _fmt(v: Variant, depth := 0) -> String:
    match typeof(v):
        TYPE_NIL:
            return "null"
        TYPE_BOOL, TYPE_INT:
            return str(v)
        TYPE_FLOAT:
            return _num(v)
        TYPE_STRING, TYPE_STRING_NAME, TYPE_NODE_PATH:
            return '"%s"' % v
        TYPE_VECTOR2:
            return "(%s, %s)" % [_num(v.x), _num(v.y)]
        TYPE_VECTOR3:
            return "(%s, %s, %s)" % [_num(v.x), _num(v.y), _num(v.z)]
        TYPE_VECTOR4, TYPE_QUATERNION:
            return "(%s, %s, %s, %s)" % [_num(v.x), _num(v.y), _num(v.z), _num(v.w)]
        TYPE_COLOR:
            return "(%s, %s, %s, %s)" % [_num(v.r), _num(v.g), _num(v.b), _num(v.a)]
        TYPE_BASIS:
            return "[%s; %s; %s]" % [_fmt(v.x), _fmt(v.y), _fmt(v.z)]
        TYPE_TRANSFORM3D:
            return "{%s @ %s}" % [_fmt(v.basis), _fmt(v.origin)]
        TYPE_AABB:
            return "[%s + %s]" % [_fmt(v.position), _fmt(v.size)]
        TYPE_RECT2:
            return "[%s + %s]" % [_fmt(v.position), _fmt(v.size)]
        TYPE_PLANE:
            return "(%s, %s)" % [_fmt(v.normal), _num(v.d)]
        TYPE_ARRAY:
            if depth > 3 or v.size() > 16:
                return "<Array %d #%08x>" % [v.size(), hash(v)]
            return "[%s]" % ", ".join(v.map(func(e: Variant) -> String: return _fmt(e, depth + 1)))
        TYPE_DICTIONARY:
            return _dictionary(v, depth)
        TYPE_OBJECT:
            return _resource(v)
        TYPE_PACKED_BYTE_ARRAY:
            return "<bytes %d #%08x>" % [v.size(), hash(v)]
        TYPE_PACKED_INT32_ARRAY, TYPE_PACKED_INT64_ARRAY, TYPE_PACKED_FLOAT32_ARRAY, TYPE_PACKED_FLOAT64_ARRAY, \
        TYPE_PACKED_VECTOR2_ARRAY, TYPE_PACKED_VECTOR3_ARRAY, TYPE_PACKED_COLOR_ARRAY, \
        TYPE_PACKED_VECTOR4_ARRAY, TYPE_PACKED_STRING_ARRAY:
            return "<%s %d #%08x>" % [type_string(typeof(v)), v.size(), _packed_hash(v)]
        _:
            return "%s:%s" % [type_string(typeof(v)), str(v)]


## Hash of a packed array's content; floats are rounded first, so the hash
## agrees with what is printed.
func _packed_hash(v: Variant) -> int:
    match typeof(v):
        TYPE_PACKED_FLOAT32_ARRAY, TYPE_PACKED_FLOAT64_ARRAY, TYPE_PACKED_VECTOR2_ARRAY, \
        TYPE_PACKED_VECTOR3_ARRAY, TYPE_PACKED_COLOR_ARRAY, TYPE_PACKED_VECTOR4_ARRAY:
            return hash(v.to_byte_array())
    return hash(v)


func _dictionary(d: Dictionary, depth: int) -> String:
    if depth > 3 or d.size() > 24:
        return "<Dictionary %d>" % d.size()
    var keys := _sorted(d.keys())
    var items: PackedStringArray = []
    for key in keys:
        items.append("%s: %s" % [_fmt(key, depth + 1), _fmt(d[key], depth + 1)])
    return "{%s}" % ", ".join(items)


# ---- resources ----------------------------------------------------------

## "Class#hash" for a resource, whose description goes in the table.
func _resource(res: Variant) -> String:
    if res == null:
        return "null"
    if not (res is Resource):
        return (res as Object).get_class()
    var id: int = res.get_instance_id()
    if _by_object.has(id):
        return _by_object[id]
    _by_object[id] = res.get_class() + "#cycle"
    var text := _describe(res)
    var name := "%s#%s" % [res.get_class(), text.sha256_text().substr(0, 10)]
    _by_object[id] = name
    _table[name] = text
    return name


func _describe(res: Resource) -> String:
    var parts: PackedStringArray = []
    if res.resource_name != "":
        parts.append("name=" + res.resource_name)
    if res.resource_path != "":
        parts.append("path=" + res.resource_path)
    if res is ShaderMaterial:
        parts.append(_shader_material(res))
    elif res is Texture2D:
        parts.append("size=%dx%d" % [res.get_width(), res.get_height()])
        if res is ImageTexture or res is CompressedTexture2D:
            var image: Image = res.get_image()
            if image != null:
                parts.append("format=%d mips=%s" % [image.get_format(), image.has_mipmaps()])
    elif res is ArrayMesh:
        parts.append(_array_mesh(res))
    elif res is Shader:
        parts.append("code=" + res.code.sha256_text().substr(0, 12))
    if not (res is ArrayMesh or res is Shader):
        parts.append_array(_changed_properties(res))
    return " ".join(parts)


func _shader_material(mat: ShaderMaterial) -> String:
    var shader := mat.shader
    if shader == null:
        return "shader=null"
    var names: PackedStringArray = []
    for info in shader.get_shader_uniform_list():
        names.append(info["name"])
    names.sort()
    var params: PackedStringArray = []
    for name in names:
        params.append("%s=%s" % [name, _fmt(mat.get_shader_parameter(name))])
    return "shader=%s params={%s}" % [shader.code.sha256_text().substr(0, 12), ", ".join(params)]


func _array_mesh(mesh: ArrayMesh) -> String:
    var out: PackedStringArray = ["aabb=" + _fmt(mesh.get_aabb())]
    for i in mesh.get_surface_count():
        var arrays := mesh.surface_get_arrays(i)
        var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
        var index: Variant = arrays[Mesh.ARRAY_INDEX]
        var item := "s%d(%s, %d vertices #%08x, %d indices #%08x, %s)" % [i, mesh.surface_get_primitive_type(i),
            verts.size(), hash(verts.to_byte_array()), index.size() if index != null else 0,
            hash(index) if index != null else 0, _resource(mesh.surface_get_material(i))]
        out.append(item)
    return " ".join(out)
