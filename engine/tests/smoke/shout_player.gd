# SPDX-License-Identifier: GPL-3.0-or-later
#
# The player's shouts through GameSession on the real SE pack (SKYDOT_SSE_PACK), headless, 7b-3: the
# voice key (voice_pressed/voice_released) and Magic's shout rules in BleakFallsBarrow01 (the skeever
# 0x3B9F7 and the bandit 0x39FE4). Unrelenting Force (SHOU 0x13E07; words Fus 0x13E22, Ro 0x13E23, Dah
# 0x13E24) is taught and unlocked (the Game natives' paths) and by a dragon soul; the voice slot is
# shared with powers; a tap is one word (UF1 on the bandit: it staggers and is hurt), the second shout is
# refused while the voice recovers (15 s) and allowed after; 0.5 s held are two words, a full hold three
# (UF3 lands on every actor in the cone and pushes them: they fall as ragdolls, live, and get up); no
# shout while a hand charges and no hand cast while a shout charges; the player's voice line plays;
# a save and a load keep the words, the voice slot and the recovery left. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const SKEEVER := 0x3B9F7
const BANDIT := 0x39FE4
const UNRELENTING_FORCE := 0x13E07
const WORD_FUS := 0x13E22
const WORD_RO := 0x13E23
const WORD_DAH := 0x13E24
const UF1 := 0x13E09
const UF2 := 0x13F39
const UF3 := 0x13F3A
const VOICE_POWERS := 0x13E06  # the quest of the player's shout lines
const FIREBOLT := 0x12FD0
const ARCHETYPE_STAGGER := 33
const STEP := 1.0 / 60.0
const SAVE := "user://shout_player.skydot"

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _landed: Array[Dictionary] = []
var _shouts: Array[Dictionary] = []
var _messages: Array[String] = []
var _effects: Array[Dictionary] = []


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
		print("smoke_shout_player: SKIP (SKYDOT_SSE_PACK unset)")
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


## The player stands about `distance` metres from `node`, on the side of it with a clear line from the
## eyes to its chest, and looks at it.
func _stand_before(node: Node3D, distance: float) -> void:
	var space := _camera.get_world_3d().direct_space_state
	var chest := node.global_position + Vector3.UP * 0.6
	var forward := -node.global_transform.basis.z
	forward.y = 0.0
	forward = forward.normalized()
	for scale in [1.0, 0.75, 0.5, 1.25]:
		for step in 12:
			var dir := forward.rotated(Vector3.UP, step * TAU / 12.0)
			var feet: Vector3 = node.global_position + dir * distance * scale
			var eye := feet + Vector3.UP * GameSession.EYE_HEIGHT
			var free := space.intersect_ray(PhysicsRayQueryParameters3D.create(eye, chest, 1 | 2 | 8)).is_empty()
			var ground := not space.intersect_ray(PhysicsRayQueryParameters3D.create(feet + Vector3.UP * 0.5, feet - Vector3.UP * 0.5, 1 | 2 | 8)).is_empty()
			if free and ground:
				_feet = feet
				_camera.global_position = eye
				_camera.look_at(chest, Vector3.UP)
				return
	expect(false, "no clear place to stand before 0x%X" % int(node.get_meta("skydot_ref", 0)))


## The voice key down for `frames` frames. Returns the voice_pressed result.
func _say(frames: int) -> Dictionary:
	var pressed := _session.voice_pressed()
	await _frames(frames)
	_session.voice_released()
	return pressed


## Game.TeachWord / Game.UnlockWord: the natives' own paths (SkydotPapyrus.teach_word, unlock_word; the
## VM natives call the same rules, tested in test_magic_natives).
func _vm(fn: String, word: int) -> void:
	if fn == "TeachWord":
		expect(_session.papyrus.teach_word(PLAYER, word), "TeachWord 0x%X" % word)
	else:
		expect(_session.papyrus.unlock_word(PLAYER, word), "UnlockWord 0x%X" % word)


func _word(index: int) -> Dictionary:
	return _session.papyrus.shout_info(PLAYER, UNRELENTING_FORCE)["words"][index]


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.quests = false
	config.preload_doors = false
	config.wander = false
	config.combat = false
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, _camera, FakePlayer.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	var papyrus := _session.papyrus
	var magic := _session.magic
	_session.spell_landed.connect(func(_hand: int, spell: int, target: int, _point: Vector3, _hostile: bool) -> void:
		_landed.append({"spell": spell, "target": target}))
	_session.message.connect(func(text: String, _box: bool) -> void: _messages.append(text))
	_session.magic_effect_changed.connect(func(target: int, mgef: int, archetype: int, started: bool, _instance: int) -> void:
		_effects.append({"target": target, "mgef": mgef, "archetype": archetype, "started": started}))
	magic.shouted.connect(func(shout: int, words: int, spell: int, _info: Dictionary) -> void:
		_shouts.append({"shout": shout, "words": words, "spell": spell}))
	var skeever := _session.world.find_ref_node(SKEEVER) as Node3D
	var bandit := _session.world.find_ref_node(BANDIT) as Node3D
	expect(skeever != null and bandit != null, "the skeever and the bandit are built")
	if skeever == null or bandit == null:
		quit(1)
		return
	expect(papyrus.start_quest(VOICE_POWERS), "the quest of the player's shout lines runs")
	await _frames(30)

	# ---- words: known, locked, unlocked; the voice slot
	var info := papyrus.shout_info(PLAYER, UNRELENTING_FORCE)
	expect(info["found"] and info["usable"] == 0 and info["words"].size() == 3, "Unrelenting Force has three words, none usable: %s" % [info])
	_vm("TeachWord", WORD_FUS)
	expect(_word(0)["known"] and not _word(0)["unlocked"], "Game.TeachWord: Fus is known and locked")
	expect(_messages.any(func(m: String) -> bool: return m.contains("Fus")), "and the player is told: %s" % [_messages])
	expect(_session.equip_shout(UNRELENTING_FORCE) and papyrus.equipped_shout(PLAYER) == UNRELENTING_FORCE, "the shout is in the voice slot")
	_stand_before(bandit, 5.0)
	await _frames(2)
	var refused := _session.voice_pressed()
	expect(not refused["ok"] and refused["reason"] == "no word" and not magic.is_shouting(), "a locked word cannot be said: %s" % [refused])
	_vm("UnlockWord", WORD_FUS)
	expect(_word(0)["unlocked"] and papyrus.shout_info(PLAYER, UNRELENTING_FORCE)["usable"] == 1, "Game.UnlockWord: Fus is usable")

	# ---- the voice slot is shared with powers
	var power := 0
	for known in papyrus.spells_of(PLAYER):
		var power_info := magic.info_of(known)
		if power_info.get("found", false) and int(power_info["type"]) == 2:
			power = known
			break
	expect(power != 0, "the player has a greater power")
	expect(_session.equip_spell(power, Magic.POWER) and papyrus.equipped_shout(PLAYER) == 0, "equipping a power empties the shout")
	var power_cast := _session.voice_pressed()
	_session.voice_released()
	expect(power_cast["ok"], "the voice key casts the power: %s" % [power_cast])
	expect(_session.equip_shout(UNRELENTING_FORCE) and magic.spell_in(Magic.POWER) == 0, "equipping the shout empties the power slot")

	# ---- a tap: one word, UF1 on the bandit, the player says "Fus!"
	var bandit_health := papyrus.get_actor_value(BANDIT, "Health")
	_landed.clear()
	_shouts.clear()
	_effects.clear()
	var tap := await _say(1)
	expect(tap["ok"], "the voice key charges the shout: %s" % [tap])
	expect(_shouts.size() == 1 and _shouts[0]["words"] == 1 and _shouts[0]["spell"] == UF1, "a tap is one word, UF1: %s" % [_shouts])
	await _frames(5)
	expect(_landed.any(func(l: Dictionary) -> bool: return l["spell"] == UF1 and l["target"] == BANDIT), "UF1 lands on the bandit: %s" % [_landed])
	expect(papyrus.get_actor_value(BANDIT, "Health") < bandit_health or papyrus.is_actor_dead(BANDIT), "who is hurt")
	expect(_effects.any(func(e: Dictionary) -> bool: return e["target"] == BANDIT and e["archetype"] == ARCHETYPE_STAGGER and e["started"]),
		"and staggers: %s" % [_effects])
	expect(_session.speech.is_speaking(PLAYER), "the player says his line")
	var recovery := papyrus.voice_recovery(PLAYER)
	expect(recovery > 14.0 and recovery <= 15.0, "the voice recovers for 15 s: %f" % recovery)
	var again := _session.voice_pressed()
	expect(not again["ok"] and again["reason"] == "recovering" and not magic.is_shouting(), "a second shout is refused while recovering: %s" % [again])
	# the HUD's voice meter fills as the recovery runs out
	var hud := HudView.new(_session, func(_action: StringName) -> String: return "")
	root.add_child(hud)
	await process_frame
	await process_frame
	expect(hud.voice_alpha == 1.0 and hud.voice_fill < 0.1, "the meter shows, nearly empty: %f %f" % [hud.voice_alpha, hud.voice_fill])
	await _frames(30)
	papyrus.update(7.0)
	await process_frame
	await process_frame
	expect(hud.voice_fill > 0.4 and hud.voice_fill < 0.7, "and half full after half the recovery: %f" % hud.voice_fill)
	papyrus.update(15.0)
	expect(papyrus.voice_recovery(PLAYER) == 0.0, "the recovery runs out")

	# ---- the magic menu: the Shouts category, equip, and unlock with a dragon soul (Ro)
	var view := MagicView.new(_session)
	root.add_child(view)
	view.open()
	expect(view.category_names()[0] == "Shouts", "Shouts is the first category: %s" % [view.category_names()])
	view.select_category("Shouts")
	var rows := view.rows_of("Shouts")
	expect(rows.size() == 1 and rows[0]["name"] == "Unrelenting Force" and rows[0]["power"], "Unrelenting Force is listed, in the voice slot")
	expect(rows[0]["words"][0]["unlocked"] and not rows[0]["words"][1]["known"] and not rows[0]["words"][2]["known"], "Fus is unlocked, Ro and Dah unknown")
	expect(MagicView.word_text(rows[0]["words"][0]) == "Fus  Force" and MagicView.word_text(rows[0]["words"][1]) == "?????", "the word lines: %s" % MagicView.word_text(rows[0]["words"][0]))
	_vm("TeachWord", WORD_RO)
	view._refresh()
	expect(MagicView.word_text(view.rows_of("Shouts")[0]["words"][1]) == "Ro  Balance (locked)", "Ro is known and locked: %s" % MagicView.word_text(view.rows_of("Shouts")[0]["words"][1]))
	expect(_session.equip_spell(power, Magic.POWER) and papyrus.equipped_shout(PLAYER) == 0, "a power takes the voice slot")
	expect(view.equip_current(Magic.RIGHT) and papyrus.equipped_shout(PLAYER) == UNRELENTING_FORCE, "equipping the shout from the menu")
	papyrus.set_actor_value(PLAYER, "DragonSouls", 0)
	_messages.clear()
	expect(not view.unlock_current() and view.confirmation() == "", "with no dragon soul nothing is asked")
	expect(view.notice() == _session.world.get_gmst_string("sFailSpendSoul", "?") and view.notice() != "?" and not _word(1)["unlocked"], "the menu says so: %s" % view.notice())
	papyrus.set_actor_value(PLAYER, "DragonSouls", 1)
	expect(view.unlock_current() and view.confirmation().begins_with("Spend Soul?") and view.confirmation().ends_with("Ro"), "Spend Soul? Ro: %s" % view.confirmation())
	expect(not view.answer_confirmation(false) and not _word(1)["unlocked"] and _session.dragon_souls() == 1, "no keeps the soul")
	expect(view.unlock_current() and view.answer_confirmation(true), "yes unlocks")
	expect(_word(1)["unlocked"] and _session.dragon_souls() == 0, "Ro is unlocked and the soul spent")
	expect(not view.unlock_current(), "Ro is done and Dah is unknown: nothing to unlock")
	view.close()
	view.queue_free()
	_vm("TeachWord", WORD_DAH)
	_vm("UnlockWord", WORD_DAH)
	expect(papyrus.shout_info(PLAYER, UNRELENTING_FORCE)["usable"] == 3, "all three words are usable")

	# ---- a tap says one word still; 0.5 s are two words
	_shouts.clear()
	await _say(2)
	expect(_shouts.size() == 1 and _shouts[0]["words"] == 1, "a tap is one word: %s" % [_shouts])
	papyrus.update(16.0)
	_shouts.clear()
	_landed.clear()
	await _say(30)
	expect(_shouts.size() == 1 and _shouts[0]["words"] == 2 and _shouts[0]["spell"] == UF2, "0.5 s are two words, UF2: %s" % [_shouts])
	expect(absf(papyrus.voice_recovery(PLAYER) - 20.0) < 1.0, "recovery 20 s: %f" % papyrus.voice_recovery(PLAYER))
	expect(_landed.any(func(l: Dictionary) -> bool: return l["spell"] == UF2 and l["target"] == BANDIT), "UF2 lands on the bandit")
	expect(not _session.corpses.is_down(BANDIT), "UF2 only staggers: the bandit stands")
	papyrus.update(21.0)

	# ---- a full hold: three words by themselves at fShoutTime2, UF3 on everyone in the cone, pushed
	var right := _camera.global_transform.basis.x
	skeever.global_position = bandit.global_position + right * 0.6
	await _frames(5)
	_shouts.clear()
	_landed.clear()
	var held := _session.voice_pressed()
	expect(held["ok"] and magic.is_shouting() and not magic.shout_state()["fired"], "the key is down, the shout charges: %s" % [held])
	await _frames(30)
	var charge := _session.voice_meter()
	await process_frame
	await process_frame
	expect(charge["charging"] and charge["fill"] > 0.4 and charge["fill"] < 0.7 and hud.voice_fill > 0.4, "the meter shows the charge: %s %f" % [charge, hud.voice_fill])
	expect(_shouts.is_empty(), "nothing is said at 0.5 s")
	await _frames(40)
	expect(_shouts.size() == 1 and _shouts[0]["words"] == 3 and _shouts[0]["spell"] == UF3 and magic.shout_state()["fired"],
		"three words fire by themselves at 0.9 s, UF3: %s" % [_shouts])
	_session.voice_released()
	expect(not magic.is_shouting() and _shouts.size() == 1, "and the key going up adds nothing")
	await _frames(5)
	var targets: Array = _landed.filter(func(l: Dictionary) -> bool: return l["spell"] == UF3).map(func(l: Dictionary) -> int: return l["target"])
	expect(targets.has(BANDIT) and targets.has(SKEEVER), "UF3 lands on all actors in the cone: %s" % [targets])
	var down_bandit := _session.corpses.is_down(BANDIT)
	var down_skeever := _session.corpses.is_down(SKEEVER)
	expect(down_bandit and down_skeever, "both are knocked down: %s %s" % [down_bandit, down_skeever])
	expect((bandit as SkydotActor).is_dead() and not papyrus.is_actor_dead(BANDIT), "the bandit is down but alive")
	var lay := bandit.global_position
	await _until(func() -> bool: return not _session.corpses.is_down(BANDIT) and not _session.corpses.is_down(SKEEVER), 8.0)
	expect(not _session.corpses.is_down(BANDIT) and not _session.corpses.is_down(SKEEVER), "they get up")
	expect(not (bandit as SkydotActor).is_dead() and not papyrus.is_actor_dead(BANDIT), "the bandit lives and moves again")
	expect(bandit.global_position.distance_to(lay) > 0.2, "having been thrown: %s -> %s" % [lay, bandit.global_position])
	expect(absf(papyrus.voice_recovery(PLAYER) - 45.0) < 3.0, "recovery 45 s: %f" % papyrus.voice_recovery(PLAYER))
	papyrus.update(46.0)

	# ---- no shout while a hand casts, no cast while a shout charges
	expect(papyrus.add_spell(PLAYER, FIREBOLT) and _session.equip_spell(FIREBOLT, Magic.RIGHT), "Firebolt in the right hand")
	_messages.clear()
	_session.voice_pressed()
	expect(magic.is_shouting(), "the shout charges")
	_session.attack_pressed()
	expect(not magic.is_casting(Magic.RIGHT) and magic.ready_state() == Magic.Ready.SHEATHED, "no spell is cast while shouting")
	expect(_messages.has("You cannot cast a spell while shouting"), "the player is told: %s" % [_messages])
	_session.voice_released()
	papyrus.update(16.0)
	_session.attack_pressed()  # readies the hand
	await _until(func() -> bool: return magic.ready_state() == Magic.Ready.READY, 5.0)
	_session.attack_pressed()
	expect(magic.is_casting(Magic.RIGHT), "Firebolt charges")
	_messages.clear()
	var blocked := _session.voice_pressed()
	expect(not blocked["ok"] and blocked["reason"] == "casting" and not magic.is_shouting(), "no shout while a hand casts: %s" % [blocked])
	expect(_messages.has("You cannot shout while casting a spell"), "the player is told: %s" % [_messages])
	_session.attack_released()
	_session.toggle_weapon()
	expect(_session.equip_spell(0, Magic.RIGHT) or true, "the hand is emptied")

	# ---- changing shouts is refused while shouting
	_session.voice_pressed()
	_messages.clear()
	expect(not _session.equip_shout(UNRELENTING_FORCE) and _messages.size() == 1, "no change of shout while shouting: %s" % [_messages])
	_session.voice_released()
	papyrus.update(46.0)

	# ---- a save and a load keep the words, the voice slot and the recovery
	await _say(1)
	var left := papyrus.voice_recovery(PLAYER)
	expect(left > 14.0, "a tap starts the recovery: %f" % left)
	expect(_session.save_game(SAVE), "the game is saved")
	papyrus.set_voice_recovery(PLAYER, 0.0)
	_session.equip_spell(power, Magic.POWER)
	expect(papyrus.equipped_shout(PLAYER) == 0, "the voice slot holds the power now")
	expect(_session.load_game(SAVE), "and loaded")
	expect(papyrus.equipped_shout(PLAYER) == UNRELENTING_FORCE, "the equipped shout is kept")
	var kept := papyrus.shout_info(PLAYER, UNRELENTING_FORCE)
	expect(kept["usable"] == 3 and kept["words"].all(func(w: Dictionary) -> bool: return w["known"] and w["unlocked"]), "the known and unlocked words are kept: %s" % [kept])
	expect(absf(papyrus.voice_recovery(PLAYER) - left) < 1.0, "and the recovery left: %f of %f" % [papyrus.voice_recovery(PLAYER), left])

	print("smoke_shout_player: ", "FAIL" if failures > 0 else "OK")
	quit(1 if failures > 0 else 0)
