# SPDX-License-Identifier: GPL-3.0-or-later
#
# Active magic effects on the real SE pack (SKYDOT_SSE_PACK), through a GameSession (ADR 0008):
# Healing cast at the hurt player for a few ticks (Health rises, never above the maximum, Magicka
# is spent, its regeneration waits), a potion of healing, a Fortify Health potion (the maximum
# rises and returns), a save and a load in the middle of one (the maximum is raised once, the
# duration goes on), Flames cast at a skeever in BleakFallsBarrow01 (Health drops per second,
# resist applied), an AddSpell / HasSpell round trip. The log lines are the oracles. Headless; skips
# without the pack.
extends SceneTree

const PLAYER := 0x14
const HEALING := 0x12FCC
const FLAMES := 0x12FCD
const FIREBOLT := 0x12FD0
const POTION_HEALTH := 0x3EADE      # RestoreHealth02: 50 points
const POTION_FORTIFY := 0x3EAF4     # FortifyHealth02: +40 maximum for 60 s
const SKEEVER := 0x3B9F7
const SAVE := "user://magic_effects.skydot"
const TICK := 0.1

var failures := 0
var _session: GameSession
var _log: Array[String] = []


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
		print("smoke_magic_effects: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _lines(needle: String) -> int:
	var n := 0
	for line in _log:
		if line.contains(needle):
			n += 1
	return n


## Advance the VM by `seconds` in ticks of TICK; with `cast`, the player re-casts it on `target`
## before every tick (a concentration spell held).
func _run_for(seconds: float, spell := 0, target := 0, check_max := false) -> void:
	var papyrus := _session.papyrus
	var t := 0.0
	while t < seconds - 1e-9:
		if spell != 0:
			papyrus.cast_spell(PLAYER, spell, target, TICK)
		papyrus.update(TICK)
		if check_max:
			var health := papyrus.get_actor_value(PLAYER, "Health")
			expect(health <= papyrus.get_actor_value_max(PLAYER, "Health") + 1e-6, "Health never goes above the maximum: %f" % health)
		t += TICK


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, viewpoint, FakePlayer.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	var papyrus := _session.papyrus
	papyrus.trace.connect(func(text: String) -> void: _log.append(text))
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")

	# ---- Healing: concentration, on the caster
	var max_health := papyrus.get_actor_value_max(PLAYER, "Health")
	var max_magicka := papyrus.get_actor_value_max(PLAYER, "Magicka")
	expect(max_health > 0.0 and max_magicka > 0.0, "the player has Health %f and Magicka %f" % [max_health, max_magicka])
	papyrus.damage_actor_value(PLAYER, "Health", 60.0)
	var hurt := papyrus.get_actor_value(PLAYER, "Health")
	expect(absf(hurt - (max_health - 60.0)) < 1.0, "the player is hurt: %f" % hurt)
	var magicka_before := papyrus.get_actor_value(PLAYER, "Magicka")
	var cast := _session.cast_spell(HEALING, 0, TICK)
	expect(cast["ok"], "Healing is cast: %s" % [cast])
	print("healing cast: ", cast)
	_run_for(2.0, HEALING, PLAYER, true)
	var healed := papyrus.get_actor_value(PLAYER, "Health")
	print("health ", hurt, " -> ", healed)
	expect(healed > hurt + 15.0 and healed < hurt + 25.0, "Healing restores about 10 a second: %f -> %f" % [hurt, healed])
	var magicka_after := papyrus.get_actor_value(PLAYER, "Magicka")
	expect(magicka_after < magicka_before - 15.0, "Magicka is spent: %f -> %f" % [magicka_before, magicka_after])
	expect(_lines("magic: 0x14 casts 0x12FCC on 0x14 (cost") == 1, "the cast is logged once: %d" % _lines("magic: 0x14 casts 0x12FCC"))
	expect(_lines("magic: effect 0x1CEA4 on 0x14 start mag 10 dur 1") == 1, "the effect starts once")
	_run_for(30.0, HEALING, PLAYER, true)  # until it is full, never over
	expect(papyrus.get_actor_value(PLAYER, "Health") == max_health, "Health stops at the maximum")
	_run_for(1.5)  # nobody re-casts: it runs out
	expect(_lines("magic: effect 0x1CEA4 on 0x14 end") == 1, "the effect ends")
	expect(not papyrus.active_effects(PLAYER).any(func(e: Dictionary) -> bool: return e["source"] == HEALING), "no Healing effect is left")
	papyrus.restore_actor_value(PLAYER, "Magicka", 1000.0)  # the held spell drained it
	var again := _session.cast_spell(HEALING, 0, 0.0)  # a second of it costs about 10.6
	expect(again["ok"], "cast again: %s" % [again])
	papyrus.damage_actor_value(PLAYER, "Magicka", papyrus.get_actor_value(PLAYER, "Magicka"))
	var broke := _session.cast_spell(HEALING, 0, 0.0)
	expect(not broke["ok"] and broke["reason"] == "not enough magicka", "no Magicka, no cast: %s" % [broke])
	papyrus.dispel_all_spells(PLAYER)

	# ---- the regeneration waits after a spend
	papyrus.restore_actor_value(PLAYER, "Magicka", 1000.0)
	_run_for(1.0)
	papyrus.spend_magicka(PLAYER, 20.0)
	var spent := papyrus.get_actor_value(PLAYER, "Magicka")
	_run_for(0.3)
	expect(absf(papyrus.get_actor_value(PLAYER, "Magicka") - spent) < 1e-6, "Magicka waits: %f -> %f" % [spent, papyrus.get_actor_value(PLAYER, "Magicka")])
	_run_for(3.0)
	expect(papyrus.get_actor_value(PLAYER, "Magicka") > spent, "and then comes back")

	# ---- a potion of healing
	papyrus.damage_actor_value(PLAYER, "Health", 70.0)
	var low := papyrus.get_actor_value(PLAYER, "Health")
	_session.give([Vector2i(POTION_HEALTH, 1)])
	expect(_session.use_item(POTION_HEALTH), "the potion is drunk")
	expect(absf(papyrus.get_actor_value(PLAYER, "Health") - (low + 50.0)) < 1.0, "it restores 50: %f -> %f" % [low, papyrus.get_actor_value(PLAYER, "Health")])
	expect(papyrus.get_item_count(PLAYER, POTION_HEALTH) == 0, "and is used up")
	expect(_lines("magic: effect 0x3EB15 on 0x14 start mag 50") == 1, "its effect is logged")
	expect(not _session.use_item(POTION_HEALTH), "a potion that is not carried cannot be used")

	# ---- a Fortify Health potion: peak, held, undone
	_run_for(120.0)
	_session.give([Vector2i(POTION_FORTIFY, 1)])
	expect(_session.use_item(POTION_FORTIFY), "the fortify potion is drunk")
	expect(absf(papyrus.get_actor_value_max(PLAYER, "Health") - (max_health + 40.0)) < 1e-6, "the maximum rises by 40: %f" % papyrus.get_actor_value_max(PLAYER, "Health"))
	_run_for(30.0)
	expect(absf(papyrus.get_actor_value_max(PLAYER, "Health") - (max_health + 40.0)) < 1e-6, "and stays raised")
	_run_for(31.0)
	expect(papyrus.get_actor_value_max(PLAYER, "Health") == max_health, "and returns at the end: %f" % papyrus.get_actor_value_max(PLAYER, "Health"))
	expect(papyrus.get_actor_value(PLAYER, "Health") <= max_health, "Health with it")

	# ---- a save in the middle of one
	_session.give([Vector2i(POTION_FORTIFY, 1)])
	expect(_session.use_item(POTION_FORTIFY), "drunk again")
	_run_for(10.0)
	expect(_session.save_game(SAVE), "the game is saved")
	_run_for(30.0)
	expect(_session.load_game(SAVE), "and loaded: 30 s earlier")
	expect(absf(papyrus.get_actor_value_max(PLAYER, "Health") - (max_health + 40.0)) < 1e-6, "the maximum is raised once: %f" % papyrus.get_actor_value_max(PLAYER, "Health"))
	var fortify := papyrus.active_effects(PLAYER).filter(func(e: Dictionary) -> bool: return e["source"] == POTION_FORTIFY)
	expect(fortify.size() == 1 and absf(fortify[0]["remaining"] - 50.0) < 0.5, "50 seconds remain: %s" % [fortify])
	_run_for(45.0)
	expect(absf(papyrus.get_actor_value_max(PLAYER, "Health") - (max_health + 40.0)) < 1e-6, "still raised after 45 s")
	_run_for(6.0)
	expect(papyrus.get_actor_value_max(PLAYER, "Health") == max_health, "it ends in time: %f" % papyrus.get_actor_value_max(PLAYER, "Health"))
	expect(papyrus.active_effects(PLAYER).all(func(e: Dictionary) -> bool: return e["source"] != POTION_FORTIFY), "nothing of it is left")

	# ---- Flames at the skeever
	var skeever_max := papyrus.get_actor_value_max(SKEEVER, "Health")
	var skeever_before := papyrus.get_actor_value(SKEEVER, "Health")
	expect(skeever_max > 0.0, "the skeever has Health %f" % skeever_max)
	var resist := papyrus.get_actor_value(SKEEVER, "ResistFire")
	papyrus.restore_actor_value(PLAYER, "Magicka", 1000.0)
	var player_magicka := papyrus.get_actor_value(PLAYER, "Magicka")
	_run_for(1.0, FLAMES, SKEEVER)
	var effects := papyrus.active_effects(SKEEVER)
	print("skeever effects: ", effects)
	var fire := effects.filter(func(e: Dictionary) -> bool: return e["mgef"] == 0x13CA9)
	expect(fire.size() == 1, "FireDamageConcAimed runs on the skeever")
	if fire.size() == 1:
		expect(absf(fire[0]["magnitude"] - 8.0 * (1.0 - minf(resist, 85.0) / 100.0)) < 1e-4, "its magnitude is 8 cut by ResistFire %f: %f" % [resist, fire[0]["magnitude"]])
	var lost := skeever_before - papyrus.get_actor_value(SKEEVER, "Health")
	print("skeever lost ", lost, " of ", skeever_before, " (resist ", resist, ")")
	expect(absf(lost - 8.0 * (1.0 - minf(resist, 85.0) / 100.0)) < 0.5, "Health drops per second: %f" % lost)
	expect(papyrus.get_actor_value(PLAYER, "Magicka") < player_magicka - 10.0, "Flames costs the player Magicka")
	expect(_lines("magic: archetype 7 (Demoralize) not done") <= 1, "a Demoralize effect is logged as not done at most once: %d" % _lines("not done"))
	print("not done lines: ", _log.filter(func(l: String) -> bool: return l.contains("not done")))
	_run_for(2.0)  # nobody holds the spell: it ends
	expect(papyrus.get_actor_value(SKEEVER, "Health") > 0.0 or papyrus.is_actor_dead(SKEEVER), "the skeever is hurt or dead")

	# ---- spells added, held, taken
	var firebolt := _session.world.get_spell(FIREBOLT)
	expect(firebolt.get("editor_id", "") == "Firebolt", "Firebolt is 0x12FD0: %s" % [firebolt.get("editor_id")])
	expect(not papyrus.has_spell(PLAYER, FIREBOLT), "the player does not know Firebolt")
	expect(papyrus.add_spell(PLAYER, FIREBOLT) and papyrus.has_spell(PLAYER, FIREBOLT), "AddSpell teaches it")
	expect(papyrus.spells_of(PLAYER).has(FIREBOLT) and papyrus.spells_of(PLAYER).has(FLAMES) and papyrus.spells_of(PLAYER).has(HEALING), "the list has Firebolt, Flames and Healing")
	expect(papyrus.equip_spell(PLAYER, FIREBOLT, 1) and papyrus.equipped_spell(PLAYER, 1) == FIREBOLT, "Firebolt is held in the right hand")
	expect(papyrus.remove_spell(PLAYER, FIREBOLT) and not papyrus.has_spell(PLAYER, FIREBOLT), "RemoveSpell takes it")
	expect(papyrus.equipped_spell(PLAYER, 1) == 0, "and puts it away")
	expect(papyrus.remove_spell(PLAYER, FLAMES) and not papyrus.has_spell(PLAYER, FLAMES) and papyrus.add_spell(PLAYER, FLAMES), "a base spell can be removed and taught again")
	var flames_cost := _session.papyrus.spell_cost(PLAYER, FLAMES)
	var healing_cost := _session.papyrus.spell_cost(PLAYER, HEALING)
	print("spell costs at the player's skills: Flames ", flames_cost, " Healing ", healing_cost)
	expect(flames_cost > 11.0 and flames_cost < 14.0, "Flames costs about 14 x (1 - (skill/400)^0.65): %f" % flames_cost)
	expect(healing_cost > 9.0 and healing_cost < 12.0, "Healing costs about 12 x (1 - (skill/400)^0.65): %f" % healing_cost)
	print("smoke_magic_effects: ", "FAIL" if failures > 0 else "OK")
	quit(1 if failures > 0 else 0)
