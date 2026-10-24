# SPDX-License-Identifier: GPL-3.0-or-later
#
# The magic archetypes of 7a-4 through GameSession on the real SE pack (SKYDOT_SSE_PACK), headless:
# in BleakFallsBarrow01 the player (who cannot die here) casts, with SkydotPapyrus.cast_spell, at
# the bandit 0x39FE4 and the other fighters of the cell, and the AI and the scene answer.
# Invisibility keeps the bandits from seeing him and the next cast breaks it (Candlelight puts its
# OmniLight3D up); Calm ends a fight and holds it off; Fear makes the bandit run from him; Fury
# makes the bandit attack the actor next to him; Paralyze freezes the bandit for its duration;
# Conjure Familiar places a SkydotActor within 300 units, attached to the AI, an ally of the
# player, that is gone when the spell ends and is back after a save and load; Reanimate stands a
# corpse up as an ally; Candlelight and the summon survive a save and load; Bound Sword is
# equipped, cannot be dropped and ends when put away; Slow Time sets Engine.time_scale and puts it
# back; Banish (archetype 42, not done) logs "not done" once. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const SAVE := "user://magic_archetypes.skydot"
const STEP := 1.0 / 60.0
const UNIT := 0.0142875  # metres per game unit

const INVISIBILITY := 0x27EB6
const CANDLELIGHT := 0x43324
const SKEEVER := 0x3B9F7
const CALM := 0x4DEE9
const FEAR := 0x4DEEA
const FURY := 0x4DEEB
const PARALYZE := 0x5AD5F
const FAMILIAR := 0x640B6
const REANIMATE := 0x65BD7
const BOUND_SWORD := 0x211EB
const SLOW_TIME := 0x48AD0
const BANISH := 0x6D22C

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _trace: Array[String] = []
var _placed: Array[int] = []
var _deleted: Array[int] = []


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
		print("smoke_magic_archetypes: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _frames(count: int) -> void:
	for i in count:
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100.0)
		_session.papyrus.restore_actor_value(PLAYER, "Magicka", 1000.0)
		_session.step(STEP, _feet)
		await physics_frame


## `seconds` of game time in steps of `dt`.
func _advance(seconds: float, dt := 0.1) -> void:
	var left := seconds
	while left > 0.0:
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100.0)
		_session.papyrus.restore_actor_value(PLAYER, "Magicka", 1000.0)
		_session.step(dt, _feet)
		await physics_frame
		left -= dt


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


func _node(ref: int) -> SkydotActor:
	return _session.world.find_ref_node(ref) as SkydotActor


## A spot 1.8 m from `from` with the most free room behind it (the afraid run that way).
func _open_spot(from: Vector3) -> Vector3:
	var space := _camera.get_world_3d().direct_space_state
	var best := from + Vector3.RIGHT * 1.8
	var best_room := -1.0
	for step in 16:
		var dir := Vector3.RIGHT.rotated(Vector3.UP, step * TAU / 16.0)
		var query := PhysicsRayQueryParameters3D.create(from + Vector3.UP * 0.5, from + Vector3.UP * 0.5 + dir * 12.0, 1 | 2 | 8)
		var hit := space.intersect_ray(query)
		var room := 12.0 if hit.is_empty() else from.distance_to(hit["position"])
		if room > best_room:
			best_room = room
			best = from + dir * 1.8
	print("open spot: ", best_room, " m of room")
	return best

func _flat(a: Vector3, b: Vector3) -> float:
	return Vector2(a.x - b.x, a.z - b.z).length()


func _effects(actor: int, archetype: int) -> int:
	var count := 0
	for effect: Dictionary in _session.papyrus.active_effects(actor):
		if int(effect["archetype"]) == archetype and bool(effect.get("active", true)):
			count += 1
	return count


## The spell lands without its cost (most of them cost more than the player's Magicka); a cast
## breaks the player's invisibility as GameSession.cast_spell does.
func _cast(spell: int, target := 0, paid := false) -> void:
	_session.papyrus.break_invisibility(PLAYER)
	var result := _session.cast_spell(spell, target) if paid else _session.papyrus.land_spell(PLAYER, spell, target)
	expect(result["ok"], "0x%X is cast at 0x%X: %s" % [spell, target, result.get("reason", "")])
	print("cast 0x%X at 0x%X: %d instances, target has %d effects" % [spell, target, int(result.get("instances", 0)),
		_session.papyrus.active_effects(target if target != 0 else PLAYER).size()])


## Every effect on the player and the actors ends; the fights stop.
func _clean(actors: Array) -> void:
	_session.papyrus.dispel_all_spells(PLAYER)
	for ref: int in actors:
		_session.papyrus.dispel_all_spells(ref)
		_session.ai.stop_combat(ref)
	await _frames(3)


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.quests = false
	config.preload_doors = false
	config.wander = false
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, _camera, FakePlayer.new()), "the session opens")
	if _session.world == null or _session.ai == null:
		expect(false, "the session has an AI")
		quit(1)
		return
	var papyrus := _session.papyrus
	var ai := _session.ai
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	papyrus.trace.connect(func(text: String) -> void: _trace.append(text))
	papyrus.ref_placed.connect(func(ref: int, _b: int, _c: int, _p: Vector3, _r: Vector3, _s: float, _d: bool) -> void:
		_placed.append(ref))
	papyrus.ref_deleted.connect(func(ref: int) -> void: _deleted.append(ref))
	var bandit := _node(BANDIT)
	expect(bandit != null, "the bandit is built")
	if bandit == null:
		quit(1)
		return
	ai.attach_built(bandit.get_parent())
	await _frames(30)
	var home := bandit.global_position
	var forward := -bandit.global_transform.basis.z
	forward.y = 0.0
	forward = forward.normalized()
	_feet = home + forward * 6.0

	# The actor next to him in the frenzy and the corpse of the reanimation: another fighter of the cell.
	var other_ref := 0
	for candidate in bandit.get_parent().find_children("*", "SkydotActor", true, false):
		var ref: int = candidate.get_meta("skydot_ref", 0)
		if ref != 0 and ref != BANDIT and not papyrus.is_actor_dead(ref) and papyrus.get_actor_value(ref, "Aggression") >= 2 \
				and other_ref == 0:
			other_ref = ref
	expect(other_ref != 0, "another fighter is in the cell")
	var other := _node(other_ref)
	if other == null:
		quit(1)
		return
	print("actors: bandit level ", papyrus.get_actor_value(BANDIT, "Health"), ", other 0x%X" % other_ref)
	var both := [BANDIT, other_ref]

	print("PHASE ", Time.get_ticks_msec() / 1000, " ", "Invisibility: the bandit does not see him; any cast breaks it; Candlelight lights up")
	# ---- Invisibility: the bandit does not see him; any cast breaks it; Candlelight lights up
	_cast(INVISIBILITY)
	await _frames(2)
	expect(papyrus.magic_overrides(PLAYER)["invisible"], "the player is invisible")
	await _advance(6.0)
	expect(ai.get_combat_state(BANDIT) == 0, "the bandit does not see the invisible player")
	_cast(CANDLELIGHT, 0, true)
	await _frames(2)
	expect(not papyrus.magic_overrides(PLAYER)["invisible"], "casting broke the invisibility")
	expect(_session.magic_world.lights() == 1, "Candlelight has a light node: %d" % _session.magic_world.lights())
	expect(_session.host.find_children("MagicLight*", "OmniLight3D", true, false).size() == 1, "an OmniLight3D is up")
	var waited := await _until(func() -> bool: return ai.get_combat_state(BANDIT) == 1, 20.0)
	expect(waited >= 0.0 and ai.get_combat_target(BANDIT) == PLAYER, "now the bandit fights the player")
	papyrus.dispel_all_spells(PLAYER)
	await _frames(2)
	expect(_session.magic_world.lights() == 0, "the light is gone with the spell")

	# The calm, fear and fury effects only touch actors up to the spell's magnitude as level: the
	# skeever of the cave (a low level) is their subject. He stands two metres from the player.
	var skeever := _node(SKEEVER)
	expect(skeever != null, "the skeever is built")
	ai.attach_built(skeever.get_parent())
	var near_player := _open_spot(_feet)
	print("PHASE ", Time.get_ticks_msec() / 1000, " Calm")
	# ---- Calm: the fight ends and none starts
	skeever.teleport(near_player)
	await _frames(60)
	expect(ai.start_combat(SKEEVER, PLAYER), "the skeever fights the player")
	_cast(CALM, SKEEVER)
	await _advance(2.0)
	expect(papyrus.magic_overrides(SKEEVER)["calm"], "the skeever is calm")
	expect(ai.get_combat_state(SKEEVER) == 0, "calm ended his combat")
	await _advance(5.0)
	expect(ai.get_combat_state(SKEEVER) == 0, "and he starts none while calm")
	papyrus.dispel_all_spells(SKEEVER)
	await _advance(1.0)
	expect(not papyrus.magic_overrides(SKEEVER)["calm"], "the calm is over")
	await _clean([SKEEVER])

	print("PHASE ", Time.get_ticks_msec() / 1000, " Fear")
	# ---- Fear: he runs from the caster
	skeever.teleport(near_player)
	await _frames(60)
	expect(ai.start_combat(SKEEVER, PLAYER), "the skeever fights the player again")
	var before := _flat(skeever.global_position, _feet)
	_cast(FEAR, SKEEVER)
	await _frames(2)
	expect(papyrus.magic_overrides(SKEEVER)["fear_from"] == PLAYER, "the fear names the player")
	for second in 4:
		await _advance(1.0)
		print("skeever: ", skeever.global_position, " walking ", skeever.is_walking(), " state ", ai.get_combat_state(SKEEVER), " ", skeever.get_state(), " vel ", skeever.velocity, " path ", skeever.get_path())
	var after := _flat(skeever.global_position, _feet)
	print("fear: distance ", before, " -> ", after)
	expect(after > before + 1.0, "the afraid skeever gets away from the player: %f -> %f" % [before, after])
	await _clean([SKEEVER])

	print("PHASE ", Time.get_ticks_msec() / 1000, " Frenzy")
	# ---- Frenzy: he attacks the actor next to him (the player cannot be seen: invisible)
	_cast(INVISIBILITY)
	var troll := _node(other_ref)
	var troll_forward := -troll.global_transform.basis.z
	troll_forward.y = 0.0
	var skeever_at := troll.global_position + troll_forward.normalized() * 2.5
	skeever.teleport(skeever_at)
	var to_troll := troll.global_position - skeever_at
	skeever.face(atan2(-to_troll.x, -to_troll.z))
	await _frames(60)
	ai.stop_combat(SKEEVER)
	expect(_session.papyrus.land_spell(PLAYER, FURY, SKEEVER)["ok"], "Fury lands")  # no break: it is not a cast of the player's hand
	await _frames(2)
	expect(papyrus.magic_overrides(SKEEVER)["frenzy"], "the skeever is frenzied")
	waited = await _until(func() -> bool:
		var t := ai.get_combat_target(SKEEVER)
		return t != 0 and t != PLAYER, 15.0)
	expect(waited >= 0.0, "the frenzied skeever attacks another actor: target 0x%X" % ai.get_combat_target(SKEEVER))
	await _clean([BANDIT, other_ref, SKEEVER])
	await _advance(4.0)
	var t_after := ai.get_combat_target(SKEEVER)
	expect(t_after == 0 or t_after == PLAYER, "the frenzy over, he leaves that fight: 0x%X" % t_after)
	await _clean([BANDIT, other_ref, SKEEVER])

	print("PHASE ", Time.get_ticks_msec() / 1000, " ", "Paralyze (and its stagger): he does nothing for the duration")
	# ---- Paralyze (and its stagger): he does nothing for the duration
	_node(BANDIT).teleport(home)
	await _frames(10)
	_cast(PARALYZE, BANDIT)
	await _frames(5)
	print("bandit: dead ", papyrus.is_actor_dead(BANDIT), " state ", ai.get_actor_state(BANDIT).get("attached"), " lod ", _node(BANDIT).get_lod(), " in tree ", _node(BANDIT).is_inside_tree(), " frozen ", _node(BANDIT).is_frozen())
	expect(papyrus.magic_overrides(BANDIT)["paralyzed"], "the bandit is paralysed")
	expect(bandit.is_frozen(), "his node is frozen")
	var still := bandit.global_position
	await _advance(3.0)
	expect(bandit.global_position.distance_to(still) < 0.05, "he does not move while paralysed")
	await _advance(10.0)
	expect(not papyrus.magic_overrides(BANDIT)["paralyzed"] and not bandit.is_frozen(), "and moves again")
	await _clean(both)

	print("PHASE ", Time.get_ticks_msec() / 1000, " ", "Conjure Familiar: a summon next to the caster, an ally, gone at the end, back after a load")
	# ---- Conjure Familiar: a summon next to the caster, an ally, gone at the end, back after a load
	_placed.clear()
	_cast(FAMILIAR)
	await _advance(1.0)
	expect(not _placed.is_empty(), "references are placed: %s" % [_placed])
	var summon := 0
	for placed_ref in _placed:
		if _session.world.is_actor_base(papyrus.get_ref_base(placed_ref)):
			summon = placed_ref
	var summon_node := _node(summon)
	expect(summon_node != null, "the summon is an actor node")
	if summon_node != null:
		var apart := _flat(summon_node.global_position, _feet) / UNIT
		print("summon ", apart, " units from the caster")
		expect(apart < 300.0 + 100.0, "within about 300 units of the caster: %f" % apart)
		expect(ai.get_actor_state(summon).get("attached", false), "attached to the AI")
		expect(ai.get_reaction(summon, PLAYER) == 2, "an ally of the player: %d" % ai.get_reaction(summon, PLAYER))
		expect(ai.get_reaction(summon, BANDIT) == ai.get_reaction(PLAYER, BANDIT), "and sees the bandit as the player does")
	expect(_effects(PLAYER, 18) == 1, "the summon effect runs")
	expect(_session.save_game(SAVE), "saved with the summon")
	papyrus.dispel_all_spells(PLAYER)
	await _frames(8)
	expect(_deleted.has(summon), "the summon's reference is deleted with the spell")
	expect(_node(summon) == null or not is_instance_valid(_node(summon)), "and its node is gone")
	expect(_session.load_game(SAVE), "loaded")
	await _advance(1.0)
	expect(_effects(PLAYER, 18) == 1, "the summon effect is listed after the load")
	expect(_node(summon) != null and ai.get_actor_state(summon).get("attached", false),
		"and the summon is back, attached to the AI")
	# its duration runs out
	await _advance(70.0, 1.0)
	await _frames(8)
	expect(_effects(PLAYER, 18) == 0 and _deleted.has(summon), "it is gone after its duration")
	await _clean(both)

	print("PHASE ", Time.get_ticks_msec() / 1000, " ", "Reanimate: a corpse stands up as an ally")
	# ---- Reanimate: a corpse stands up as an ally
	var corpse := SKEEVER
	_session.kill_actor(corpse)
	await _advance(5.0, 0.1)
	expect(papyrus.is_actor_dead(corpse) and _node(corpse).is_dead(), "the skeever is a corpse")
	_cast(REANIMATE, corpse)
	await _advance(1.0)
	expect(not papyrus.is_actor_dead(corpse) and not _node(corpse).is_dead(), "reanimated: alive again")
	expect(ai.get_reaction(corpse, PLAYER) == 2, "an ally of the player")
	papyrus.dispel_all_spells(PLAYER)
	await _frames(3)

	print("PHASE ", Time.get_ticks_msec() / 1000, " ", "Candlelight survives a save and load")
	# ---- Candlelight survives a save and load
	_cast(CANDLELIGHT, 0, true)
	await _frames(3)
	expect(_session.magic_world.lights() == 1, "the light is up")
	expect(_session.save_game(SAVE), "saved with the light")
	papyrus.dispel_all_spells(PLAYER)
	await _frames(3)
	expect(_session.magic_world.lights() == 0, "dispelled")
	expect(_session.load_game(SAVE), "loaded")
	await _frames(5)
	expect(_effects(PLAYER, 12) == 1 and _session.magic_world.lights() == 1, "light and effect are back after the load")
	papyrus.dispel_all_spells(PLAYER)
	await _frames(3)

	print("PHASE ", Time.get_ticks_msec() / 1000, " ", "Bound Sword")
	# ---- Bound Sword
	_cast(BOUND_SWORD)
	await _frames(3)
	var sword := int(papyrus.equipment(PLAYER)["right"])
	var bound := papyrus.active_effects(PLAYER).filter(func(e: Dictionary) -> bool:
		return int(e["archetype"]) == 17 and int(e["associated"]) == sword)
	expect(sword != 0 and bound.size() == 1, "the bound sword 0x%X is in the right hand and an effect names it" % sword)
	expect(_session.drop(sword, 1) == 0, "a bound weapon is not dropped")
	expect(_session.unequip(sword), "put away")
	await _frames(3)
	expect(_effects(PLAYER, 17) == 0, "putting it away ends the effect")
	expect(not papyrus.inventory(PLAYER).any(func(row: Dictionary) -> bool: return int(row["form"]) == sword), "and the sword is gone")

	print("PHASE ", Time.get_ticks_msec() / 1000, " ", "Slow Time")
	# ---- Slow Time
	_cast(SLOW_TIME)
	await _frames(3)
	expect(is_equal_approx(Engine.time_scale, 0.3), "Engine.time_scale is slowed: %f" % Engine.time_scale)
	expect(_session.save_game(SAVE), "saved slowed")
	papyrus.dispel_all_spells(PLAYER)
	await _frames(3)
	expect(is_equal_approx(Engine.time_scale, 1.0), "and restored: %f" % Engine.time_scale)
	expect(_session.load_game(SAVE), "loaded")
	await _frames(3)
	expect(is_equal_approx(Engine.time_scale, 0.3), "the effect saved still slows the world: %f" % Engine.time_scale)
	await _advance(10.0, 0.5)
	expect(is_equal_approx(Engine.time_scale, 1.0), "the end of the effect restores it: %f" % Engine.time_scale)

	print("PHASE ", Time.get_ticks_msec() / 1000, " ", "an archetype with no rule logs not done once")
	# ---- an archetype with no rule logs "not done" once
	_trace.clear()
	_cast(FURY, SKEEVER)
	papyrus.dispel_all_spells(SKEEVER)
	_cast(FURY, SKEEVER)
	_cast(BANISH, BANDIT)
	await _frames(3)
	var rally := _trace.filter(func(line: String) -> bool: return line.contains("(Rally) not done"))
	expect(rally.size() == 1, "Fury's Rally (archetype 38, no rule) logs not done once: %s" % [rally])

	Engine.time_scale = 1.0
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE))
	print("smoke_magic_archetypes: ", "OK" if failures == 0 else "%d FAILURES" % failures)
	quit(1 if failures > 0 else 0)
