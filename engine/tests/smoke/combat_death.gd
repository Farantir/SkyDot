# SPDX-License-Identifier: GPL-3.0-or-later
#
# Death through GameSession on the real SE pack (SKYDOT_SSE_PACK), headless:
# the bandit 0x39FE4 of BleakFallsBarrow01 is killed with the Iron Sword (the
# hits go through Combat, so the impulse has a direction); his node stops
# (SkydotActor.set_dead: no collision, no animation), a PhysicalBoneSimulator3D
# ragdoll runs under his skeleton (Jolt simulates it headless too), freezes
# when it rests and stores the corpse place; he is still pickable (Search), the
# corpse place survives a save, and entering the cell again lays him down at
# that place without an impulse. A draugr gets the same ragdoll; a skeleton
# without the human bones (a frostbite spider) logs "death: no ragdoll" and lies
# on its side. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const DRAUGR := 0x380B5
const SPIDER := 0x3A1E1
const IRON_SWORD := 0x12EB7
const STEP := 1.0 / 60.0
const CELL := "BleakFallsBarrow01"

var failures := 0
var _session: GameSession
var _player: FakePlayer
var _camera: Camera3D
var _feet := Vector3.ZERO


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false
	var loaded_position := Vector3.ZERO
	var loaded_yaw := 0.0
	var loaded_pitch := 0.0

	func place_at(_at: Vector3, new_yaw: float, new_pitch: float) -> void:
		yaw = new_yaw
		pitch = new_pitch

	func set_view_distance(_metres: float) -> void:
		pass

	func save_section() -> PackedByteArray:
		return PackedByteArray()

	func load_section(_bytes: PackedByteArray) -> bool:
		return true


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_combat_death: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _frames(count: int) -> void:
	for i in count:
		_session.step(STEP, _feet)
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


func _skeleton_of(node: Node) -> Skeleton3D:
	var found := node.find_children("*", "Skeleton3D", true, false)
	return found.front() if not found.is_empty() else null


func _pelvis(node: Node) -> Vector3:
	var skeleton := _skeleton_of(node)
	return skeleton.global_transform * skeleton.get_bone_global_pose(skeleton.find_bone("NPC Pelvis [Pelv]")).origin


## Distance of every bone from its parent, in skeleton units.
func _bone_lengths(skeleton: Skeleton3D) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	for i in skeleton.get_bone_count():
		var parent := skeleton.get_bone_parent(i)
		out.append(0.0 if parent < 0 else skeleton.get_bone_global_pose(i).origin.distance_to(skeleton.get_bone_global_pose(parent).origin))
	return out


## Bones keep their lengths (within 5%): no stretching.
## Only bones whose parent is a ragdoll body are held to their length (the
## root bones above the pelvis stay where the actor stood), with 4 units of
## slack for the joints of the shortest.
func _check_lengths(skeleton: Skeleton3D, reference: PackedFloat32Array, who: String) -> void:
	var bodies := {}
	for entry: Array in Corpses.BONES:
		bodies[skeleton.find_bone(entry[0])] = true
	var bad := []
	for i in skeleton.get_bone_count():
		var parent := skeleton.get_bone_parent(i)
		if parent < 0 or not bodies.has(parent) or not bodies.has(i) or absf(reference[i]) > 1.0e3:
			continue
		var now := skeleton.get_bone_global_pose(i).origin.distance_to(skeleton.get_bone_global_pose(parent).origin)
		if absf(now - reference[i]) > maxf(0.05 * reference[i], 4.0):
			bad.append("%s %.1f->%.1f" % [skeleton.get_bone_name(i), reference[i], now])
	print(who, " stretched bones: ", bad)
	expect(bad.is_empty(), "%s: every bone keeps its length: %s" % [who, bad.slice(0, 6)])


func _open_session(pack_dir: String) -> bool:
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	config.wander = false
	_player = FakePlayer.new()
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	return _session.open(config, _camera, _player) and _session.start_game()


func _run(pack_dir: String) -> void:
	await process_frame
	expect(_open_session(pack_dir), "the session opens")
	expect(_session.enter_cell(CELL, SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null), "the cell is entered")
	var papyrus := _session.papyrus
	var node := _session.world.find_ref_node(BANDIT) as SkydotActor
	expect(node != null, "the bandit is built")
	if node == null:
		quit(1)
		return
	await _frames(30)
	_session.give([Vector2i(IRON_SWORD, 1)])
	_session.equip(IRON_SWORD)
	# The player stands in front of the bandit so the hits push him back.
	var forward := -node.global_transform.basis.z
	_feet = node.global_position + forward * 1.2
	_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT
	_camera.look_at(node.global_position + Vector3.UP * 1.2, Vector3.UP)
	var standing := _pelvis(node)
	var reference := _bone_lengths(_skeleton_of(node))
	var start := node.global_position
	expect(standing.y - start.y > 0.7, "standing: the pelvis is %.2f m above the feet" % (standing.y - start.y))
	expect(node.collision_layer != 0, "a living actor is on a physics layer")

	var hits := _session.kill_actor(BANDIT)
	expect(hits > 1 and papyrus.is_actor_dead(BANDIT), "killed after %d hits" % hits)
	await _frames(2)
	expect(node.is_dead() and node.collision_layer == 0 and node.collision_mask == 0, "the node is dead: no collision")
	var skeleton := _skeleton_of(node)
	expect(skeleton.get_node_or_null("Ragdoll") != null and _session.corpses.simulating() >= 1, "a ragdoll runs under the skeleton")
	var player := node.get_node("AnimationPlayer") as AnimationPlayer
	expect(not player.is_playing() and not player.active, "the animation stopped")
	expect(not papyrus.get_corpse_place(BANDIT).is_empty(), "a corpse place is stored at once")
	var took := await _until(func() -> bool: return _session.corpses.simulating() == 0, 8.0)
	print("ragdoll rested after ", took, " s")
	expect(took > 0.0 and took <= 5.5, "the ragdoll rests (or times out) within 5 s: %f" % took)
	expect(skeleton.get_node_or_null("Ragdoll") == null, "the ragdoll is freed once frozen")
	var lying := _pelvis(node)
	print("pelvis: standing ", standing, " lying ", lying, " feet ", start)
	expect(lying.y - start.y < 0.5, "the pelvis lies %.2f m above the floor" % (lying.y - start.y))
	expect(Vector2(lying.x - start.x, lying.z - start.z).length() > 0.05, "the hit pushed him: moved %.2f m" % Vector2(lying.x - start.x, lying.z - start.z).length())
	await _frames(30)
	expect(_pelvis(node).distance_to(lying) < 0.01, "and it stays frozen")
	_check_lengths(_skeleton_of(node), reference, "bandit")
	var place := papyrus.get_corpse_place(BANDIT)
	var cell: int = _session.world.get_actor(BANDIT)["cell"]
	print("corpse place ", place)
	expect(place.get("space", 0) == _session.world.get_cell_space(cell), "in the cell's space")
	var back := SkydotWorld.godot_to_skyrim(Vector3(lying.x, start.y, lying.z))
	expect(Vector2(place["position"].x - back.x, place["position"].y - back.y).length() < 20.0, "at the pelvis: %s vs %s" % [place["position"], back])

	# Still pickable: a ray at the body finds him, and the prompt says Search.
	var from := lying + Vector3(0.0, 1.0, 0.0) + Vector3(0.6, 0.0, 0.0)
	var picked := _session.world.pick_ref(_session.host, from, lying)
	expect(picked.get("ref", 0) == BANDIT, "the corpse is picked: %s" % [picked.get("ref", 0)])
	var prompt := ActivationPrompt.new(_session.world, papyrus, _session.host, _camera).describe(cell, BANDIT)
	expect(prompt.get("verb", "") == "Search", "the prompt is Search: %s" % [prompt])

	# The draugr: the same bones.
	var draugr := _session.world.find_ref_node(DRAUGR) as SkydotActor
	if draugr != null:
		var draugr_feet := draugr.global_position
		var draugr_reference := _bone_lengths(_skeleton_of(draugr))
		_session.kill_actor(DRAUGR)
		await _frames(2)
		expect(draugr.is_dead() and _session.corpses.simulating() == 1, "the draugr has a ragdoll too")
		await _until(func() -> bool: return _session.corpses.simulating() == 0, 8.0)
		var down := _pelvis(draugr)
		print("draugr pelvis ", down, " feet ", draugr_feet)
		_check_lengths(_skeleton_of(draugr), draugr_reference, "draugr")
		expect(draugr.is_dead() and down.y - draugr_feet.y < 0.7, "the draugr lies down: %.2f m" % (down.y - draugr_feet.y))
		expect(not papyrus.get_corpse_place(DRAUGR).is_empty(), "the draugr has a corpse place")
	else:
		expect(false, "the draugr is built")

	# Save, then enter the cell again: he is dead where the place says.
	expect(_session.save_game("user://combat_death.skydot"), "saves")
	expect(_session.load_game("user://combat_death.skydot"), "loads")
	var after_load := papyrus.get_corpse_place(BANDIT)
	expect(after_load.get("position", Vector3.ZERO).is_equal_approx(place["position"]), "the corpse place survives the save")
	expect(_session.enter_cell(CELL, SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null), "the cell is entered again")
	var again := _session.world.find_ref_node(BANDIT) as SkydotActor
	expect(again != null and again != node, "he is built again")
	if again != null:
		expect(again.is_dead() or papyrus.is_actor_dead(BANDIT), "dead")
		await _frames(10)
		expect(again.is_dead(), "the node was laid down")
		var spot := SkydotWorld.godot_to_skyrim(again.global_position)
		expect(Vector2(spot.x - place["position"].x, spot.y - place["position"].y).length() < 20.0, "built at the corpse place: %s vs %s" % [spot, place["position"]])
		await _until(func() -> bool: return _session.corpses.simulating() == 0, 8.0)
		var settled := _pelvis(again)
		expect(settled.y - again.global_position.y < 0.5, "it settles lying: %.2f m" % (settled.y - again.global_position.y))
		expect(Vector2(settled.x - lying.x, settled.z - lying.z).length() < 1.5, "near where he lay: %.2f m" % Vector2(settled.x - lying.x, settled.z - lying.z).length())

	# A skeleton without the human bones.
	var spider := _session.world.find_ref_node(SPIDER) as SkydotActor
	expect(spider != null, "the spider is built")
	if spider != null:
		_session.kill_actor(SPIDER)
		await _frames(5)
		expect(spider.is_dead() and _session.corpses.simulating() == 0, "the spider has no ragdoll")
		expect(absf(spider.transform.basis.y.y) < 0.1, "and lies on its side")
		expect(not papyrus.get_corpse_place(SPIDER).is_empty(), "with a corpse place")
	print("smoke_combat_death: failures=%d" % failures)
	quit(failures)
