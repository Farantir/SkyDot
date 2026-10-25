# SPDX-License-Identifier: GPL-3.0-or-later
#
# A dragon dies and the player absorbs its soul (step 7b-7) on the real SE pack (SKYDOT_SSE_PACK),
# headless, in phases: EncDragon01Fire 0x1CA03 on the plain between Whiterun and the Western Watchtower,
# Skyrim (13100, -13650), killed by the player 25 m away. The game's own scripts run: DragonActorScript
# (onDeath, deadAndWaiting, deadDisintegrated) and MQKillDragonScript.deathSequence (the absorb effects,
# SetGhost, DragonSouls +1 with the message, MQKillDragonFaction, the skin items). Later phases add the
# skeleton (the underskin shows, the skin is not built: SkinDragonHider covers its slots), the loot (the
# death item: dragon bones and scales) and a save and load with both corpses. The death plays the dragon's death clip (mtdeathintro) and holds its
# last frame, no ragdoll; a dragon killed in the air crashes first and lies on the ground. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const DRAGON := 0x1CA03
const STEP := 1.0 / 60.0
const ICE_SPIKE := 0x2B96C
const DRAGON_BONE := 0x3ADA4
const DRAGON_SCALES := 0x3ADA3
const SPOT := Vector3(13100.0, -13650.0, -3000.0)  # Skyrim units; the ground is below

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _ref := 0
var _node: SkydotActor
var _effects: Array[Array] = []  # [effect, ref, target, seconds, playing]
var _animations: Array[Array] = []  # [ref, name]
var _messages: Array[String] = []
var _shaders := 0
var _player: FakePlayer
var _dead: Array[int] = []  # the dragons that died, in order


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
		print("smoke_dragon_soul: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _frames(count: int) -> void:
	for i in count:
		_camera.global_position = _feet + Vector3.UP * GameSession.EYE_HEIGHT
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100000.0)
		_session.step(STEP, _feet)
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


func _ground(at: Vector3) -> Vector3:
	var space := _camera.get_world_3d().direct_space_state
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(at + Vector3.UP * 200.0, at - Vector3.UP * 400.0,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN))
	return hit["position"] if not hit.is_empty() else at


## The visible meshes on the skeleton of `node` by name.
func _shown(node: Node) -> Array[String]:
	var out: Array[String] = []
	for mesh in node.find_children("*", "MeshInstance3D", true, false):
		if mesh.get_parent() is Skeleton3D and mesh.visible:
			out.append(String(mesh.name))
	return out


## A dead dragon shows the skeleton and none of its skin: the underskin is there, SkinDragonHider is worn and
## no skin mesh is built (what it wears decides, not the fade).
func _expect_skeleton(node: Node, who: String) -> void:
	var shown := _shown(node)
	expect(shown.has("Underskin"), "%s shows the underskin: %s" % [who, shown])
	var skin := 0
	for mesh in node.find_children("*", "MeshInstance3D", true, false):
		if mesh.get_parent() is Skeleton3D and mesh.get_meta("skydot_skin", false):
			skin += 1
	expect(skin == 0, "%s has no skin mesh: %d" % [who, skin])
	expect(shown.size() <= 2, "and nothing else but the hider's shape: %s" % [shown])


## The corpse holds the death item (dragon bones and scales), shows it in the loot list and none of what it wears.
func _expect_loot(ref: int, who: String) -> void:
	var rows: Array = _session.papyrus.inventory(ref)
	var counts := {}
	for row: Dictionary in rows:
		counts[int(row["form"])] = int(row["count"])
	expect(counts.get(DRAGON_BONE, 0) > 0 and counts.get(DRAGON_SCALES, 0) > 0, "%s holds dragon bones and scales: %s" % [who, counts])
	var listed := ItemRows.visible(rows).map(func(row: Dictionary) -> int: return int(row["form"]))
	expect(listed.has(DRAGON_BONE) and listed.has(DRAGON_SCALES), "the loot list shows them: %s" % [listed])
	for worn in [_quest_form("::SkinDragonHider_var"), _quest_form("::DragonUnderskin_var")]:
		expect(counts.has(worn) and not listed.has(worn), "%s: 0x%X is carried but not listed" % [who, worn])
	expect(listed.size() == rows.filter(ItemRows.is_listed).size(), "only playable, named rows are listed")
	expect(ItemRows.visible(rows).all(func(row: Dictionary) -> bool: return row["name"] != ""), "no nameless row is listed")
	expect(rows.any(func(row: Dictionary) -> bool: return row["name"] == ""), "(the corpse does carry nameless items)")


func _state() -> String:
	for script in _session.papyrus.get_scripts(_ref):
		if String(script["name"]).to_lower() == "dragonactorscript":
			return script["state"]
	return "(no DragonActorScript)"


func _souls() -> float:
	return _session.papyrus.get_actor_value(PLAYER, "DragonSouls")


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.quests = false
	config.preload_doors = false
	config.wander = false
	config.radius = 1
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	_player = FakePlayer.new()
	expect(_session.open(config, _camera, _player), "the session opens")
	if _session.world == null or _session.ai == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	_session.papyrus.visual_effect.connect(func(effect: int, ref: int, target: int, seconds: float, playing: bool) -> void:
		_effects.append([effect, ref, target, seconds, playing]))
	_session.papyrus.sub_graph_animation.connect(func(ref: int, name: String) -> void: _animations.append([ref, name]))
	_session.papyrus.message.connect(func(text: String, _box: bool) -> void: _messages.append(text))
	_session.papyrus.effect_shader.connect(func(_shader: int, _ref: int, _playing: bool) -> void: _shaders += 1)
	_feet = SkydotWorld.skyrim_position(SPOT)
	expect(_session.enter_world("Tamriel", _feet, null), "the worldspace is entered")
	await _frames(240)

	await _spawn_dragon(false)
	if _node == null:
		quit(1)
		return
	await _ground_death()
	await _air_death()
	await _save_load()
	print("smoke_dragon_soul: failures=%d" % failures)
	quit(failures)


## A dragon at the spot, 25 m from the player, unaware of him; `air`: placed 3000 units up (it flies).
func _spawn_dragon(air: bool) -> void:
	var centre := _ground(SkydotWorld.skyrim_position(SPOT))
	_feet = _ground(centre + Vector3(25.0, 0.0, 0.0))
	var at := SkydotWorld.godot_to_skyrim(centre)
	if air:
		at.z += 3000.0
	_ref = _session.papyrus.spawn_actor(DRAGON, _session.place.cell_id, at, 0.0)
	expect(_ref != 0, "the dragon is spawned")
	if not air:
		_session.papyrus.set_allowed_to_fly(_ref, false)
	await _frames(30)
	_node = _session.world.find_ref_node(_ref) as SkydotActor
	expect(_node != null, "the dragon is built")
	expect(_node.has_death_clip() and _node.get_death_clip().ends_with("mtdeathintro.hkx"), "it has a death clip: '%s'" % _node.get_death_clip())
	expect(_session.papyrus.get_scripts(_ref).any(func(s: Dictionary) -> bool: return String(s["name"]).to_lower() == "dragonactorscript"), "it runs DragonActorScript")
	expect(_state() != "deadAndWaiting" and _state() != "deadDisintegrated", "alive, it is in no death state: '%s'" % _state())
	if air:
		await _frames(120)
		expect(_node.is_flying(), "it flies: %s" % _node.get_flight_mode())


## The bone that moves most between the rest pose and now, and how far (skeleton units).
func _pose_shift(node: SkydotActor) -> Array:
	var skeleton := node.find_children("*", "Skeleton3D", true, false).front() as Skeleton3D
	var best := -1
	var far := 0.0
	for i in skeleton.get_bone_count():
		var moved := (skeleton.get_bone_global_pose(i).origin - skeleton.get_bone_global_rest(i).origin).length()
		if moved > far:
			far = moved
			best = i
	return [best, far, skeleton.get_bone_global_pose(best).origin if best >= 0 else Vector3.ZERO]


func _quest_form(name: String) -> int:
	var quest := _session.world.find_quest("MQKillDragon")
	return _session.papyrus.get_variable(quest, "MQKillDragonScript", name)


## Leave the dragon 1 health, then kill it (`how`: "fist" the --kill path, "spell" Ice Spike landed by the player).
func _kill(how: String) -> void:
	var papyrus := _session.papyrus
	papyrus.damage_actor_value(_ref, "Health", papyrus.get_actor_value(_ref, "Health") - 1.0)
	if how == "spell":
		var result := papyrus.land_spell(PLAYER, ICE_SPIKE, _ref)
		expect(result["ok"], "the spell lands: %s" % [result])
		await _frames(2)
	else:
		expect(_session.kill_actor(_ref) >= 1, "the player kills it")
	expect(papyrus.is_actor_dead(_ref), "it is dead")


## The death sequence: the player within range, the absorb, the souls `expected`.
func _absorb(expected: float) -> void:
	var papyrus := _session.papyrus
	var hider := _quest_form("::SkinDragonHider_var")
	var underskin := _quest_form("::DragonUnderskin_var")
	var faction := _quest_form("::MQKillDragonFaction_var")
	expect(_state() == "deadAndWaiting", "DragonActorScript runs onDeath and waits for the player: '%s'" % _state())
	_feet = _ground(_node.global_position + Vector3(10.0, 0.0, 0.0))
	await _frames(60)
	print("dragon soul: player at ", _feet, " dragon at ", _node.global_position, " state ", _state())
	var done := await _until(func() -> bool: return _state() == "deadDisintegrated", 20.0)
	expect(done >= 0.0, "it reaches deadDisintegrated: '%s'" % _state())
	var ended := await _until(func() -> bool: return _souls() >= expected, 60.0)
	expect(ended >= 0.0, "the player absorbs the soul: DragonSouls %s" % _souls())
	print("dragon soul: absorbed after %.1f s of the sequence, %d effects, %d animations, %d shaders" % [ended, _effects.size(), _animations.size(), _shaders])
	await _frames(300)
	expect(_souls() == expected, "exactly one soul: %s" % _souls())
	expect(_effects.any(func(e: Array) -> bool: return e[1] == _ref and e[2] == PLAYER and e[4]), "a visual effect plays dragon -> player: %s" % [_effects])
	expect(_effects.any(func(e: Array) -> bool: return e[1] == PLAYER and e[2] == _ref and e[4]), "and one player -> dragon")
	expect(_animations.any(func(a: Array) -> bool: return a[0] == _ref and a[1] == "SkinFadeOut"), "SkinFadeOut plays on the dragon: %s" % [_animations])
	expect(papyrus.is_actor_ghost(_ref), "the dragon is a ghost")
	expect(_messages.count("Dragon Soul Absorbed") == int(expected), "the player is told once per soul: %s" % [_messages])
	expect(papyrus.faction_rank(_ref, faction) >= 0, "the dragon is in MQKillDragonFaction: %d" % papyrus.faction_rank(_ref, faction))
	var worn: Array = papyrus.equipment(_ref)["worn"]
	expect(worn.has(hider), "it wears SkinDragonHider: %s" % [worn])
	expect(worn.has(underskin), "and DragonUnderskin")
	_expect_skeleton(_node, "the corpse")
	_expect_loot(_ref, "the corpse")
	_dead.append(_ref)
	expect(_session.visual_effects.stream_count() == 0, "no stream is left: %d" % _session.visual_effects.stream_count())
	var result := papyrus.apply_hit(PLAYER, _ref, 0, 0, "attackStart", 0)
	expect(result.has("error"), "a hit on the dead ghost lands nowhere: %s" % [result])
	await _frames(300)
	expect(_souls() == expected, "no second absorb: %s" % _souls())


## Killed on the ground, far from the player: the death clip plays and its last frame is held, no ragdoll;
## then the whole death sequence runs to its end.
func _ground_death() -> void:
	var papyrus := _session.papyrus
	expect(_souls() == 0.0, "no dragon souls yet: %s" % _souls())
	expect(not papyrus.is_actor_ghost(_ref), "alive it is no ghost")
	expect(papyrus.faction_rank(_ref, _quest_form("::MQKillDragonFaction_var")) == -1, "and in no MQKillDragonFaction")
	var errors: int = papyrus.get_error_count()
	await _kill("fist")
	await _frames(10)
	expect(_node.get_death_state() == "clip", "the death clip plays: '%s'" % _node.get_death_state())
	expect(_session.corpses.simulating() == 0, "no ragdoll")
	var held := await _until(func() -> bool: return _node.get_death_state() == "held", 15.0)
	expect(held >= 0.0, "the pose is held: '%s'" % _node.get_death_state())
	var shift := _pose_shift(_node)
	print("dragon soul: death pose held after %.1f s, bone %d is %.0f units from its rest" % [held, shift[0], shift[1]])
	expect(shift[1] > 100.0, "the death pose is far from the rest pose: %.0f units" % shift[1])
	expect(_node.has_meta("skydot_bounds"), "the corpse is pickable")
	expect(_session.corpses.simulating() == 0, "still no ragdoll")
	# At 25 m (1750 units) it is beyond deathFXrange (1000 units): it keeps waiting.
	await _frames(240)
	expect(_state() == "deadAndWaiting" and _effects.is_empty(), "out of range it waits: '%s', %d effects" % [_state(), _effects.size()])
	await _absorb(1.0)
	var later := _pose_shift(_node)
	expect(later[0] == shift[0] and (later[2] as Vector3).distance_to(shift[2]) < 1.0, "the death pose is still held: %.2f units off" % (later[2] as Vector3).distance_to(shift[2]))
	expect(papyrus.get_error_count() == errors, "the VM logged no new errors: %d" % (papyrus.get_error_count() - errors))


## A dragon killed by a spell in the air crashes to the ground, plays the death clip there and holds it;
## DragonActorScript.onDeath runs for a death that is no melee hit; the absorb follows (2 souls).
func _air_death() -> void:
	_effects.clear()
	_animations.clear()
	await _spawn_dragon(true)
	var height_before := _height_over_ground(_node)
	print("dragon soul: the second dragon flies %.1f m over the ground" % height_before)
	expect(height_before > 10.0, "it is high in the air: %.1f m" % height_before)
	await _kill("spell")
	var crashed := [false]
	_node.flight_crashed.connect(func() -> void: crashed[0] = true)
	await _frames(10)
	expect(_node.get_death_state() == "crash", "it crashes first: '%s' (%s)" % [_node.get_death_state(), _node.get_flight_mode()])
	var modes := {}
	var held := await _until(func() -> bool:
		modes[_node.get_flight_mode()] = true
		return _node.get_death_state() == "held", 30.0)
	expect(held >= 0.0, "it lies on the ground with the death pose held: '%s'" % _node.get_death_state())
	print("dragon soul: crash modes ", modes.keys(), ", held after %.1f s, %.2f m over the ground" % [held, _height_over_ground(_node)])
	expect(modes.has("crash"), "the mover crashed it: %s" % [modes.keys()])
	expect(crashed[0], "flight_crashed was raised")
	expect(_height_over_ground(_node) < 2.0, "it ends on the ground: %.2f m" % _height_over_ground(_node))
	expect(_session.corpses.simulating() == 0, "no ragdoll")
	var shift := _pose_shift(_node)
	await _frames(180)
	var later := _pose_shift(_node)
	expect((later[2] as Vector3).distance_to(shift[2]) < 1.0 and later[1] > 100.0, "the death pose is not replaced by the gait: %.2f units off" % (later[2] as Vector3).distance_to(shift[2]))
	expect(_node.has_meta("skydot_bounds"), "the corpse is pickable")
	await _absorb(2.0)


func _height_over_ground(node: Node3D) -> float:
	var space := _camera.get_world_3d().direct_space_state
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(node.global_position + Vector3.UP * 0.5,
		node.global_position + Vector3.DOWN * 500.0, SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN))
	return node.global_position.y - hit["position"].y if not hit.is_empty() else INF


## F5 and F9 with both corpses lying: they are still there (the actors a script created persist), dead, the death
## clip's last frame held, wearing the hider and the underskin, still deadDisintegrated and ghosts, the souls
## unchanged and not absorbed a second time; the loot is still in them and can be taken.
func _save_load() -> void:
	var papyrus := _session.papyrus
	expect(_dead.size() == 2, "two dragons died: %s" % [_dead])
	var hider := _quest_form("::SkinDragonHider_var")
	var underskin := _quest_form("::DragonUnderskin_var")
	var souls := _souls()
	var scripts_before := {}
	for ref in _dead:
		scripts_before[ref] = papyrus.get_scripts(ref).size()
	var path := "user://dragon_soul.skydot"
	_player.loaded_position = _feet
	_messages.clear()
	_effects.clear()
	expect(_session.save_game(path), "saves")
	expect(_session.load_game(path), "loads")
	await _until(func() -> bool: return _session.world.find_ref_node(_dead[0]) != null and _session.world.find_ref_node(_dead[1]) != null, 30.0)
	await _frames(30)
	for ref in _dead:
		var who := "the dragon 0x%X after the load" % ref
		var node := _session.world.find_ref_node(ref) as SkydotActor
		expect(node != null, "%s is built again" % who)
		if node == null:
			continue
		expect(papyrus.is_actor_dead(ref) and node.is_dead(), "%s is dead" % who)
		expect(node.get_death_state() == "held", "%s holds the death pose: '%s'" % [who, node.get_death_state()])
		var shift := _pose_shift(node)
		expect(shift[1] > 100.0, "%s lies in the death pose: %.0f units from the rest pose" % [who, shift[1]])
		var worn: Array = papyrus.equipment(ref)["worn"]
		expect(worn.has(hider) and worn.has(underskin), "%s wears the hider and the underskin: %s" % [who, worn])
		_expect_skeleton(node, who)
		await _until(func() -> bool: return _height_over_ground(node) < INF, 10.0)
		expect(_height_over_ground(node) < 2.0, "%s lies on the ground: %.2f m" % [who, _height_over_ground(node)])
		expect(papyrus.is_actor_ghost(ref), "%s is a ghost" % who)
		var state := ""
		for script in papyrus.get_scripts(ref):
			if String(script["name"]).to_lower() == "dragonactorscript":
				state = script["state"]
		expect(state == "deadDisintegrated", "%s: DragonActorScript is in '%s'" % [who, state])
		_expect_loot(ref, who)
	expect(_souls() == souls, "the souls are unchanged: %s" % _souls())
	# The player stays near for a while: nothing is absorbed a second time.
	_feet = _ground(_session.world.find_ref_node(_dead[0]).global_position + Vector3(8.0, 0.0, 0.0))
	await _frames(600)
	expect(_souls() == souls, "no second absorb: %s" % _souls())
	expect(not _messages.has("Dragon Soul Absorbed"), "and no message: %s" % [_messages])
	expect(_effects.is_empty(), "and no stream: %d" % _effects.size())
	# The loot can be taken.
	var player_bones := papyrus.get_item_count(PLAYER, DRAGON_BONE)
	var held := papyrus.get_item_count(_dead[0], DRAGON_BONE)
	expect(_session.transfer(_dead[0], PLAYER, DRAGON_BONE, held) == held and held > 0, "the bones are taken: %d" % held)
	expect(papyrus.get_item_count(PLAYER, DRAGON_BONE) == player_bones + held, "the player has them")
	expect(papyrus.equipment(_dead[0])["worn"].has(hider), "the corpse still wears the hider")
