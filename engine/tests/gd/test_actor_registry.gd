# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

var _host: Node3D


func before_each() -> void:
	_host = Node3D.new()
	add_child(_host)


func _actor(parent: Node) -> SkydotActor:
	var actor := SkydotActor.new()
	parent.add_child(actor)
	return actor


func _skeleton(parent: Node3D, head_name := "NPC Head [Head]") -> Skeleton3D:
	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton"
	skeleton.add_bone("Root")
	skeleton.add_bone(head_name)
	skeleton.set_bone_parent(1, 0)
	skeleton.set_bone_rest(1, Transform3D(Basis(), Vector3(0, 1.5, 0)))
	skeleton.reset_bone_poses()
	parent.add_child(skeleton)
	return skeleton


func _skinned(skeleton: Skeleton3D, at: Vector3) -> MeshInstance3D:
	var mesh := MeshInstance3D.new()
	mesh.mesh = BoxMesh.new()  # a 1 m box
	skeleton.add_child(mesh)
	mesh.position = at
	return mesh


func test_actor_in_host_is_found_and_outside_is_not() -> void:
	var registry := ActorRegistry.new(_host)
	var inside := _actor(_host)
	var outside := _actor(self)
	expect(registry.all().has(inside), "the actor under the host is registered")
	expect(not registry.all().has(outside), "an actor outside the host is not")
	outside.queue_free()


func test_removed_and_freed_actors_are_gone() -> void:
	var registry := ActorRegistry.new(_host)
	var a := _actor(_host)
	var b := _actor(_host)
	_host.remove_child(a)
	expect(not registry.all().has(a), "a removed actor is gone")
	a.free()
	b.queue_free()
	await get_tree().process_frame
	expect_eq(registry.all().size(), 0, "a freed actor is gone")


func test_reparent_keeps_one_entry() -> void:
	var registry := ActorRegistry.new(_host)
	var other := Node3D.new()
	_host.add_child(other)
	var actor := _actor(_host)
	actor.reparent(other)
	expect_eq(registry.all().size(), 1, "one entry after the reparent")
	actor.reparent(self)
	expect_eq(registry.all().size(), 0, "none once it left the host")


func test_actors_before_the_registry_are_found() -> void:
	var early := _actor(_host)
	var registry := ActorRegistry.new(_host)
	expect(registry.all().has(early), "found by the scan at connect")
	# A host that is not in the tree yet: connected when it enters.
	var late_host := Node3D.new()
	var late := _actor(late_host)
	var late_registry := ActorRegistry.new(late_host)
	expect(late_registry.all().is_empty(), "nothing before the host enters the tree")
	add_child(late_host)
	expect(late_registry.all().has(late), "the actor of a host that entered is found")
	var more := _actor(late_host)
	expect(late_registry.all().has(more) and late_registry.all().size() == 2, "and one added later")


func test_reset_and_place_left_keep_membership_and_drop_caches() -> void:
	for reset in [true, false]:
		var registry := ActorRegistry.new(_host)
		var actor := _actor(_host)
		var skeleton := _skeleton(actor)
		registry.head(actor)
		registry.chest(actor)
		expect(not registry._heads.is_empty(), "head cached")
		if reset:
			registry.reset()
		else:
			registry.on_place_left()
		expect(registry.all().has(actor), "membership stays (%s)" % reset)
		expect(registry._heads.is_empty() and registry._meshes.is_empty(), "caches dropped (%s)" % reset)
		skeleton.queue_free()
		actor.queue_free()
		await get_tree().process_frame


func test_head_finds_the_skeleton_bone() -> void:
	var registry := ActorRegistry.new(_host)
	var actor := _actor(_host)
	_skeleton(actor)
	actor.position = Vector3(10, 0, 0)
	for i in 2:  # the second goes through the cache
		var p := registry.head(actor)
		expect(p.distance_to(Vector3(10, 1.5, 0)) < 0.001, "the head bone's place, got %s" % p)
	actor.position = Vector3(20, 0, 0)
	expect(registry.head(actor).distance_to(Vector3(20, 1.5, 0)) < 0.001, "it follows the node")
	expect(registry.head(actor).is_equal_approx(Combat.head_point_in(actor)), "as head_point_in does")


func test_head_falls_back_without_a_skeleton() -> void:
	var registry := ActorRegistry.new(_host)
	var actor := Node3D.new()
	_host.add_child(actor)
	var p := registry.head(actor)
	expect(p.is_equal_approx(Combat.head_point_in(actor)), "the fallback is head_point_in's: %s" % p)
	var skeleton := _skeleton(actor, "Spine")
	expect(registry.head(actor).is_equal_approx(Combat.head_point_in(actor)), "no head bone: the same fallback")
	skeleton.free()


func test_head_looks_up_again_when_the_skeleton_is_freed() -> void:
	var registry := ActorRegistry.new(_host)
	var actor := Node3D.new()
	_host.add_child(actor)
	var old := _skeleton(actor)
	registry.head(actor)
	old.free()
	var fresh := Skeleton3D.new()
	fresh.name = "Skeleton"
	fresh.add_bone("Head")
	fresh.set_bone_rest(0, Transform3D(Basis(), Vector3(0, 2.5, 0)))
	fresh.reset_bone_poses()
	actor.add_child(fresh)
	expect(registry.head(actor).distance_to(Vector3(0, 2.5, 0)) < 0.001, "the new skeleton's head")


func test_chest_follows_a_model_swap() -> void:
	var registry := ActorRegistry.new(_host)
	var actor := Node3D.new()
	_host.add_child(actor)
	var skeleton := _skeleton(actor)
	var old := _skinned(skeleton, Vector3(0, 1, 0))
	expect(registry.chest(actor).distance_to(Vector3(0, 1, 0)) < 0.001, "the first mesh's centre")
	# Replaced and refreshed.
	old.free()
	var second := _skinned(skeleton, Vector3(0, 3, 0))
	registry.refresh(actor)
	expect(registry.chest(actor).distance_to(Vector3(0, 3, 0)) < 0.001, "the new mesh after refresh")
	# Replaced without a refresh: the freed one is noticed.
	second.free()
	_skinned(skeleton, Vector3(0, 5, 0))
	expect(registry.chest(actor).distance_to(Vector3(0, 5, 0)) < 0.001, "the new mesh without refresh")


func test_chest_falls_back_and_is_null_outside_the_tree() -> void:
	var registry := ActorRegistry.new(_host)
	var actor := Node3D.new()
	_host.add_child(actor)
	actor.position = Vector3(1, 0, 0)
	expect(registry.chest(actor).distance_to(Vector3(1, 1.2, 0)) < 0.001, "no skinned mesh: 1.2 m over the feet")
	var loose := Node3D.new()
	expect(registry.chest(loose) == null, "a node outside the tree has none")
	loose.free()
