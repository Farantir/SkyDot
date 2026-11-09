# SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks of a ragdoll that has come to rest, from the skeleton's bone poses (a frozen ragdoll leaves
# its bodies' poses in the bones) and the race's ragdoll data (SkydotWorld.get_ragdoll): every joint
# within its limits (Ragdoll.angles, Havok's measure), no body inside another one the game lets it
# collide with, the engine's bones against the NIF's bind; and a dump of the bones in SkyrimRemote's
# @ragdoll shape (skyrim-remote/SPEC.md) to compare with the game's.
extends RefCounted


## The rotation of the body of data `body` in the skeleton's space (its bone's pose and the body's frame).
static func _turn(skeleton: Skeleton3D, body: Dictionary) -> Basis:
	var index := skeleton.find_bone(String(body["bone"]))
	var t: Transform3D = body.get("body_transform", Transform3D.IDENTITY)
	return skeleton.get_bone_global_pose(index).basis.orthonormalized() * t.basis.orthonormalized()


## {worst (degrees past a limit), at (constraint), checked, angles: [...]} over the constraints whose
## bodies are both on the skeleton.
static func limits(skeleton: Skeleton3D, data: Dictionary) -> Dictionary:
	var bodies: Array = data.get("bodies", [])
	var worst := 0.0
	var at := ""
	var checked := 0
	for c: Dictionary in data.get("constraints", []):
		var a: Dictionary = bodies[int(c["body_a"])]
		var b: Dictionary = bodies[int(c["body_b"])]
		if skeleton.find_bone(String(a["bone"])) < 0 or skeleton.find_bone(String(b["bone"])) < 0:
			continue
		checked += 1
		var past := rad_to_deg(Ragdoll.beyond_limits(c, Ragdoll.angles(c, _turn(skeleton, a), _turn(skeleton, b))))
		if past > worst:
			worst = past
			at = "%s (%s)" % [a["bone"], c["type"]]
	return {"worst": worst, "at": at, "checked": checked}


## The capsules and spheres of every body, in world space metres: [{body, a, b, r}].
static func _capsules(skeleton: Skeleton3D, data: Dictionary) -> Array:
	var out := []
	var bodies: Array = data.get("bodies", [])
	for i in bodies.size():
		var body: Dictionary = bodies[i]
		var index := skeleton.find_bone(String(body["bone"]))
		if index < 0:
			continue
		var bone := skeleton.global_transform * skeleton.get_bone_global_pose(index)
		var units := skeleton.global_transform.basis.get_scale().x
		for shape: Dictionary in body.get("shapes", []):
			var kind := String(shape["kind"])
			if kind != "capsule" and kind != "sphere":
				continue
			var placed: Transform3D = shape.get("transform", Transform3D.IDENTITY)
			var to_world := func(p: Vector3) -> Vector3:
				return bone * ((placed * p) / Ragdoll.METRES_PER_UNIT)
			var a: Vector3 = to_world.call(Vector3(shape.get("point_a", Vector3.ZERO)))
			var b: Vector3 = to_world.call(Vector3(shape.get("point_b", Vector3.ZERO)))
			out.append({"body": i, "a": a, "b": b, "r": float(shape["radius"]) * units / Ragdoll.METRES_PER_UNIT})
	return out


static func _segment_distance(p1: Vector3, q1: Vector3, p2: Vector3, q2: Vector3) -> float:
	var c := Geometry3D.get_closest_points_between_segments(p1, q1, p2, q2)
	return c[0].distance_to(c[1])


## {worst (metres one body is inside another, past the smaller radius), pair}: over the pairs the game
## lets collide (Ragdoll.collides) that are not joined by a constraint.
static func overlaps(skeleton: Skeleton3D, data: Dictionary) -> Dictionary:
	var bodies: Array = data.get("bodies", [])
	var joined := {}
	for c: Dictionary in data.get("constraints", []):
		joined[Vector2i(int(c["body_a"]), int(c["body_b"]))] = true
		joined[Vector2i(int(c["body_b"]), int(c["body_a"]))] = true
	var caps := _capsules(skeleton, data)
	var worst := 0.0
	var pair := ""
	for i in caps.size():
		for j in range(i + 1, caps.size()):
			var x: Dictionary = caps[i]
			var y: Dictionary = caps[j]
			var bx := int(x["body"])
			var by := int(y["body"])
			if bx == by or joined.has(Vector2i(bx, by)):
				continue
			if not Ragdoll.collides(int(bodies[bx].get("flags_and_part", 0)), int(bodies[by].get("flags_and_part", 0))):
				continue
			var inside := float(x["r"]) + float(y["r"]) - _segment_distance(x["a"], x["b"], y["a"], y["b"])
			var past := inside - minf(float(x["r"]), float(y["r"]))
			if past > worst:
				worst = past
				pair = "%s / %s" % [bodies[bx]["bone"], bodies[by]["bone"]]
	return {"worst": worst, "pair": pair}


## How far the engine's skeleton at rest (the hkx animation skeleton) lies from the NIF bind the bodies
## were made on: {cm, degrees, at}, positions relative to the first body's bone.
static func bind_against_rest(skeleton: Skeleton3D, data: Dictionary) -> Dictionary:
	var bodies: Array = data.get("bodies", [])
	var root_rest := Vector3.ZERO
	var root_bind := Vector3.ZERO
	var have_root := false
	var cm := 0.0
	var deg := 0.0
	var at := ""
	for body: Dictionary in bodies:
		var index := skeleton.find_bone(String(body["bone"]))
		if index < 0:
			continue
		var rest := skeleton.get_bone_global_rest(index)
		var bind: Transform3D = body.get("bone_bind", Transform3D.IDENTITY)
		var rest_m := rest.origin * Ragdoll.METRES_PER_UNIT
		if not have_root:
			root_rest = rest_m
			root_bind = bind.origin
			have_root = true
		var off := ((rest_m - root_rest) - (bind.origin - root_bind)).length() * 100.0
		var turn := rad_to_deg(Quaternion(rest.basis.orthonormalized()).angle_to(Quaternion(bind.basis.orthonormalized())))
		if off > cm:
			cm = off
			at = String(body["bone"])
		deg = maxf(deg, turn)
	return {"cm": cm, "degrees": deg, "at": at}


## The bones of `node` in SkyrimRemote's @ragdoll shape: {ref, nodes: [{name, pos, rot, scale}], bodies:
## [{node, pos}]}: Skyrim space, game units, rot row-major with world = R local.
static func dump(ref: int, node: Node3D, skeleton: Skeleton3D, data: Dictionary) -> Dictionary:
	var nodes := []
	var by_name := {}
	for i in skeleton.get_bone_count():
		var world := skeleton.global_transform * skeleton.get_bone_global_pose(i)
		var origin := SkydotWorld.godot_to_skyrim(world.origin)
		var rows := []
		var axes := []
		for k in 3:
			var local := Vector3.ZERO
			local[k] = 1.0
			axes.append((SkydotWorld.godot_to_skyrim(world * local) - origin).normalized())
		for r in 3:
			for k in 3:
				rows.append(axes[k][r])
		var entry := {"name": skeleton.get_bone_name(i), "pos": [origin.x, origin.y, origin.z], "rot": rows, "scale": 1.0}
		nodes.append(entry)
		by_name[skeleton.get_bone_name(i)] = entry
	var bodies := []
	for body: Dictionary in data.get("bodies", []):
		if by_name.has(String(body["bone"])):
			bodies.append({"node": body["bone"], "pos": by_name[String(body["bone"])]["pos"], "mass": body.get("mass", 0.0)})
	var at := SkydotWorld.godot_to_skyrim(node.global_position)
	return {"ref": "0x%08X" % ref, "ref_pos": [at.x, at.y, at.z], "nodes": nodes, "bodies": bodies}


## Our dump against the game's rest dump: bones both name, per-bone position error (game units),
## mass-weighted centre of the bodies and highest body above `floor_z`, the bodies' extents.
static func compare(ours: Dictionary, game: Dictionary, floor_z: float) -> Dictionary:
	var theirs := {}
	for n: Dictionary in game.get("nodes", []):
		theirs[String(n["name"])] = n["pos"]
	var errors := []
	for n: Dictionary in ours["nodes"]:
		if theirs.has(String(n["name"])):
			var a := Vector3(n["pos"][0], n["pos"][1], n["pos"][2])
			var b := Vector3(theirs[String(n["name"])][0], theirs[String(n["name"])][1], theirs[String(n["name"])][2])
			errors.append(a.distance_to(b))
	errors.sort()
	var stat := func(dump: Dictionary, key: String) -> Dictionary:
		var lo := Vector3.INF
		var hi := -Vector3.INF
		var sum := Vector3.ZERO
		var mass := 0.0
		for b: Dictionary in dump.get("bodies", []):
			var p := Vector3(b[key][0], b[key][1], b[key][2])
			var m := maxf(float(b.get("mass", 1.0)), 0.001)
			lo = lo.min(p)
			hi = hi.max(p)
			sum += p * m
			mass += m
		var com := sum / maxf(mass, 0.001)
		return {"com_height": com.z - floor_z, "top": hi.z - floor_z, "extent": hi - lo}
	return {"matched": errors.size(), "median": errors[errors.size() / 2] if not errors.is_empty() else -1.0,
		"max": errors.back() if not errors.is_empty() else -1.0, "ours": stat.call(ours, "pos"), "game": stat.call(game, "com")}
