# SPDX-License-Identifier: GPL-3.0-or-later
#
# Navmesh paths on the real SE pack (SKYDOT_SSE_PACK). East of Nightcaller Temple the path towards it
# keeps to the navmesh (with the triangles turned the wrong way, Godot's funnel cut a straight line
# across the gap between ImpExtIceRubble02 and ImpExtIceWall04, 2.9 m off the mesh, and Erandur stuck
# there). In DarklightTower01 Illia, on dunDarklightEscortPlayerRun01 with the player behind her, climbs
# the tower stairs (she stuck on ImpTowerOutStairs01). Headless; skips without the pack.
extends SceneTree

const TEMPLE_DOOR := 0x24301
const EAST_START := Vector3(41925.8, 100222.7, -7155.0)  # where Erandur set off, east of the temple
const RUN01 := 188680  # dunDarklightEscortPlayerRun01
const OFF_MESH := 0.3  # metres a path point may lie beside the mesh
const CLIMB := 1000.0  # units Illia rises on the stairs (about 2000 to the top)

var failures := 0


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_nav_paths_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


func _run(pack_dir: String) -> void:
	await process_frame
	var pack := SkydotPack.new()
	if pack.open(pack_dir) != OK:
		printerr("smoke_nav_paths_sse: ", pack.get_error())
		quit(1)
		return
	var world := pack.open_world()
	await _east_of_temple(world)
	await _illia(pack, world)
	print("smoke_nav_paths_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)


func _east_of_temple(world: SkydotWorld) -> void:
	var space := world.find_world("Tamriel")
	var goal: Vector3 = world.get_ref_info(world.get_ref_cell(TEMPLE_DOOR), TEMPLE_DOOR).get("position", Vector3())
	var start := EAST_START
	var root3d := Node3D.new()
	root.add_child(root3d)
	var gx := floori(start.x / SkydotWorld.CELL_UNITS)
	var gy := floori(start.y / SkydotWorld.CELL_UNITS)
	for dx in range(-1, 2):
		for dy in range(-1, 2):
			var cell := world.build_exterior(space, gx + dx, gy + dy)
			if cell != null:
				root3d.add_child(cell)
	var map: RID = root3d.get_world_3d().navigation_map
	var path := PackedVector3Array()
	for i in 120:  # until the map has synced the regions
		await physics_frame
		path = NavigationServer3D.map_get_path(map, SkydotWorld.skyrim_position(start),
			SkydotWorld.skyrim_position(goal), true)
		if path.size() >= 2:
			break
	expect(path.size() >= 2, "a path leads from the east to the temple (%d corners)" % path.size())
	var worst := 0.0
	var worst_at := Vector3()
	for i in range(1, path.size()):
		for k in 20:
			var q: Vector3 = path[i - 1].lerp(path[i], k / 20.0)
			var on := NavigationServer3D.map_get_closest_point(map, q)
			var off := Vector2(on.x - q.x, on.z - q.z).length()
			if off > worst:
				worst = off
				worst_at = SkydotWorld.godot_to_skyrim(q)
	expect(worst < OFF_MESH, "the path keeps to the navmesh (%.2f m off at %s)" % [worst, worst_at])
	root3d.queue_free()
	await process_frame


func _illia(pack: SkydotPack, world: SkydotWorld) -> void:
	var papyrus := SkydotPapyrus.new()
	papyrus.setup(pack, world)
	var ai := SkydotAi.new()
	ai.setup(world, papyrus)
	ai.set_combat(false)
	ai.days = 12.0 / 24.0
	var space := world.find_cell("DarklightTower01")
	ai.set_space(space)
	ai.place_actors()
	var ref := world.find_actor_of(world.find_npc("dunDarklightIllia"))
	expect(ref != 0, "Illia is found")
	var root3d := Node3D.new()
	root.add_child(root3d)
	root3d.add_child(world.build_cell(space))
	ai.attach_built(root3d)
	var illia: Node3D = null
	for a in root3d.find_children("*", "SkydotActor", true, false):
		if a.get_meta("skydot_ref", 0) == ref:
			illia = a
	expect(illia != null, "Illia 0x%X is built" % ref)
	if illia == null:
		return
	var from := SkydotWorld.godot_to_skyrim(illia.global_position)
	var player := from
	ai.push_package(ref, RUN01)
	var top := from.z
	for f in 60 * 60:
		if f % 30 == 0:
			# Behind her, halfway.
			player = player.lerp(SkydotWorld.godot_to_skyrim(illia.global_position), 0.5)
		ai.set_player(player, true)
		ai.update(1.0 / 60.0)
		papyrus.update(1.0 / 60.0)
		await physics_frame
		top = maxf(top, SkydotWorld.godot_to_skyrim(illia.global_position).z)
	expect(top - from.z > CLIMB, "Illia climbs the tower stairs (%.0f units up)" % (top - from.z))
