# SPDX-License-Identifier: GPL-3.0-or-later
#
# The start and test options that act on the game after the player has entered
# (--damage-actor, --learn-enchantment, --equip, --ready-weapon, --drink, --cast,
# --attack-at, --equip-spell, --cast-at, --shout, --spawn, --kill, --npc-give,
# --strip-melee, --fight, --npc-draw; --god, --shout-wait), and their per-frame
# steps. Owned by ShellLoop; both shells run the same ones. The print lines are
# what tests match.
class_name StartOptions
extends RefCounted

var _settings: ViewerSettings
var _session: GameSession
var _rig: PlayerRig
var _fight_pending: Array[int] = []  # --fight: the actors still to start (not built yet)
var _kill_in := 0.0  # --kill-after: seconds left before --kill
var _attack_at := 0  # --attack-at: the actor still to attack
var _cast_at: CastAt  # --cast-at: the cast still to make
var _shout_at: ShoutAt  # --shout: the shout still to say


func _init(settings: ViewerSettings, session: GameSession, rig: PlayerRig) -> void:
	_settings = settings
	_session = session
	_rig = rig


## --equip, --equip-spell, --ready-weapon, --attack-at, --cast-at and --kill, once the player has what --give gave.
func apply() -> void:
	for ench in _settings.learn_enchantment:
		print("--learn-enchantment: 0x%08X %s" % [ench, "learned" if _session.papyrus.learn_enchantment(ench) else "not learned"])
	for form in _settings.equip:
		if not _session.equip(form):
			print("--equip: 0x%08X cannot be equipped" % form)
	if _settings.ready_weapon and _session.weapon_state() == Combat.State.SHEATHED:
		_session.toggle_weapon()
	for form in _settings.drink:
		if _session.papyrus.get_item_count(SkydotPapyrus.PLAYER_REF, form) == 0:
			_session.give([Vector2i(form, 1)])
		print("--drink: 0x%08X %s" % [form, "used" if _session.use_item(form) else "cannot be used"])
	for entry: Dictionary in _settings.cast:
		var result := _session.cast_spell(entry["spell"], entry["target"])
		print("--cast: 0x%08X %s %s" % [entry["spell"], "cast" if result["ok"] else "refused", result["reason"]])
	_attack_at = _settings.attack_at
	for entry: ViewerSettings.SpellEquip in _settings.equip_spell:
		if not _session.papyrus.has_spell(SkydotPapyrus.PLAYER_REF, entry.form):
			_session.papyrus.add_spell(SkydotPapyrus.PLAYER_REF, entry.form)
		print("--equip-spell: 0x%08X in hand %d: %s" % [entry.form, entry.hand,
			"ok" if _session.equip_spell(entry.form, entry.hand) else "cannot be equipped"])
	if _settings.cast_at != 0:
		_cast_at = CastAt.new(_session, _rig, _settings.cast_at, _settings.cast_at_hand)
	if _settings.shout_wait.is_empty():
		_shout_at = ShoutAt.apply(_session, _rig, _settings)
	if not _settings.spawn.is_empty():
		SpawnActors.spawn(_session, _rig, _settings.spawn, _settings.spawn_distance)
	_kill_in = _settings.kill_after
	if _kill_in <= 0.0:
		_kill_now()
	for entry: ViewerSettings.NpcGive in _settings.npc_give:
		_session.papyrus.add_item(entry.ref, entry.form, entry.count)
	for ref in _settings.strip_melee:
		while _session.papyrus.melee_weapon(ref) != 0:
			if _session.papyrus.transfer(ref, SkydotPapyrus.PLAYER_REF, _session.papyrus.melee_weapon(ref), 1) <= 0:
				break
	_fight_pending = _settings.fight.duplicate()
	for ref in _settings.npc_draw:
		print("--npc-draw: 0x%08X %s" % [ref, "draws" if _session.npc_draw(ref, true) else "cannot draw"])


## The steps that wait for something in the game: --kill-after, --god, --shout-wait, --fight.
func step(delta: float) -> void:
	if _kill_in > 0.0:
		_kill_in -= delta
		if _kill_in <= 0.0:
			_kill_now()
	_god_step()
	_shout_wait_step()
	_fight_step()


## --attack-at, --cast-at and --shout: the ones that aim the player.
func aim_step() -> void:
	_attack_at_step()
	if _cast_at != null:
		_cast_at.step()
	if _shout_at != null:
		_shout_at.step()


## --god: the player's Health is restored every frame.
func _god_step() -> void:
	if _settings.god:
		_session.papyrus.restore_actor_value(SkydotPapyrus.PLAYER_REF, "Health", 100000.0)


## --shout-wait EDID:STAGE: apply the shout flags once that quest stage is done.
func _shout_wait_step() -> void:
	if _settings.shout_wait.is_empty():
		return
	var parts: PackedStringArray = _settings.shout_wait.split(":")
	var quest := _session.world.find_quest(parts[0])
	if quest == 0 or parts.size() != 2:
		return
	var state: Dictionary = _session.papyrus.get_quest_state(quest)
	if not (state.get("stages_done", []) as Array).has(int(parts[1])):
		return
	print("--shout-wait: %s done" % _settings.shout_wait)
	_settings.shout_wait = ""
	_shout_at = ShoutAt.apply(_session, _rig, _settings)


## --kill: hit the actors until they are dead.
func _kill_now() -> void:
	for ref in _settings.kill:
		print("--kill: 0x%08X took %d hits" % [ref, _session.kill_actor(ref)])


## --attack-at: face the actor and attack when it is in reach, once.
func _attack_at_step() -> void:
	if _attack_at == 0 or _session.weapon_state() != Combat.State.DRAWN:
		return
	var node := _session.world.find_ref_node(_attack_at) as Node3D
	if node == null or _session.papyrus.is_actor_dead(_attack_at):
		return
	var eye := _rig.camera.global_position
	var chest := node.global_position + Vector3.UP * 1.2
	var offset := chest - eye
	var flat := Vector2(offset.x, offset.z).length()
	_rig.apply_look(atan2(-offset.x, -offset.z), atan2(offset.y, flat))
	if flat - float(node.get("radius")) <= _session.combat.reach_metres():
		print("--attack-at: 0x%08X in reach (%.2f m)" % [_attack_at, flat])
		_session.attack(_settings.attack_power)
		_attack_at = 0


## --fight: start the fights of actors that are built now (an exterior streams them in).
func _fight_step() -> void:
	for ref in _fight_pending.duplicate():
		if _session.start_fight(ref):
			print("--fight: 0x%08X fights" % ref)
			_fight_pending.erase(ref)
