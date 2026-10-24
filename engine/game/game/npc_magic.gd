# SPDX-License-Identifier: GPL-3.0-or-later
#
# NPC magic in the scene (GAMEPLAY-PLAN.md, 7a-5): what a mage's combat AI decides (SkydotAi.combat_cast:
# "start" a spell in its hand, "release" a charged fire-and-forget spell, "stop" a held one) is carried
# out with the rules of the player's hands (SkydotPapyrus.hand_press, hand_step, hand_release: the
# charge time, the concentration's cost per step) and the delivery of Magic (deliver_from: self,
# touch, aimed projectiles, area, the same impacts and landings). The third-person clips are those the
# first-person arms play, of the actor's own behaviour project: `mrh_chargeloop` (`mrh_selfchargeloop`
# for a self spell), `mrh_aimedconcentration` (`mrh_selfconcentration`), `mrh_release`
# (`mrh_selfrelease`), played with play_action and repeated while the spell is held. A project without
# them (a creature) casts without a clip, with one warning. The casting art on its hands is not shown.
class_name NpcMagic
extends RefCounted

const RIGHT := 1
## Seconds between two landings of a concentration spell (as the player's).
const TICK_SECONDS := 0.1
## Where the head is, as a fraction of the actor's height.
const EYE_FRACTION := 0.93

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _magic: Magic
var _combat: Combat
var _casts := {}  # ref -> {hand, spell, info, target, phase, pending, started, timer, clip, clip_left, stem}
var _warn := WarnOnce.new("npc magic: ")


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, magic: Magic, combat: Combat) -> void:
	_world = world
	_papyrus = papyrus
	_magic = magic
	_combat = combat


## Whether `ref` holds a spell now.
func is_casting(ref: int) -> bool:
	return _casts.has(ref)


## The phase of `ref`'s cast: "" (none), "charging", "charged", "concentrating".
func phase_of(ref: int) -> String:
	return _casts[ref]["phase"] if _casts.has(ref) else ""


## SkydotAi.combat_cast.
func on_cast(ref: int, target: int, spell: int, hand: int, phase: String) -> void:
	match phase:
		"start":
			_start(ref, target, spell, hand)
		"release":
			if _casts.has(ref):
				_casts[ref]["pending"] = true
				if _casts[ref]["phase"] == "charged":
					_release(ref)
		"stop":
			_stop(ref)


func _start(ref: int, target: int, spell: int, hand: int) -> void:
	_stop(ref)
	var node := _world.find_ref_node(ref) as Node3D
	if node == null or not node.is_inside_tree() or _papyrus.is_actor_dead(ref):
		return
	if _papyrus.equipped_spell(ref, hand) != spell and not _papyrus.equip_spell(ref, spell, hand):
		_warn.once("equip%d_%d" % [ref, spell], "0x%X cannot hold 0x%08X" % [ref, spell])
		return
	var press := _papyrus.hand_press(ref, hand)
	if not press["ok"]:
		print("npc magic: 0x%X cannot start 0x%08X: %s" % [ref, spell, press["reason"]])
		return
	var info := _magic.info_of(spell)
	var conc := int(press["casting_type"]) == Magic.CAST_CONCENTRATION
	_combat.npc_face(ref, target)
	var cast := {"hand": hand, "spell": spell, "info": info, "target": target, "phase": "concentrating" if conc else "charging",
		"pending": false, "started": false, "timer": TICK_SECONDS, "clip": "", "clip_left": 0.0}
	_casts[ref] = cast
	_play(ref, cast, "concentration" if conc else "chargeloop")
	print("npc magic: 0x%X holds 0x%08X (%s)" % [ref, spell, "concentration" if conc else "charge"])


func _stop(ref: int) -> void:
	if not _casts.has(ref):
		return
	var cast: Dictionary = _casts[ref]
	_papyrus.hand_release(ref, cast["hand"])
	_casts.erase(ref)


## The charged spell leaves the hand: paid at the release, delivered, the release clip.
func _release(ref: int) -> void:
	var cast: Dictionary = _casts[ref]
	_casts.erase(ref)
	var result := _papyrus.hand_release(ref, cast["hand"])
	if String(result["event"]) != "cast":
		return
	var info: Dictionary = cast["info"]
	_play(ref, cast, "selfrelease" if int(info.get("delivery", 0)) == Magic.DELIVER_SELF else "release")
	_deliver(ref, cast, false, true)


func _deliver(ref: int, cast: Dictionary, again: bool, pay: bool) -> void:
	var node := _world.find_ref_node(ref) as Node3D
	if node == null:
		return
	var height: float = node.get("height") if node.get("height") != null else Combat.PLAYER_HEIGHT
	var eye := node.global_position + Vector3.UP * (height * EYE_FRACTION)
	var forward := (_combat.body_point_of(int(cast["target"])) - eye).normalized()
	if forward.length() < 0.5:
		forward = -node.global_transform.basis.z
	var right := forward.cross(Vector3.UP).normalized()
	_magic.npc_deliver(ref, int(cast["hand"]), int(cast["spell"]), cast["info"], eye, forward, right, again, pay)


## Advance the held spells by `delta` seconds.
func step(delta: float) -> void:
	for ref in _casts.keys():
		if not _casts.has(ref):
			continue
		var cast: Dictionary = _casts[ref]
		if _papyrus.is_actor_dead(ref) or _world.find_ref_node(ref) == null:
			_stop(ref)
			continue
		cast["clip_left"] -= delta
		var step := _papyrus.hand_step(ref, cast["hand"], delta)
		match String(step["event"]):
			"charged":
				cast["phase"] = "charged"
				if cast["pending"]:
					_release(ref)
					continue
			"tick":
				if not cast["started"]:
					cast["started"] = true
					cast["phase"] = "concentrating"
					_magic.npc_cast_started.emit(ref, cast["hand"], cast["spell"], cast["info"])
				cast["timer"] += delta
				if cast["timer"] >= TICK_SECONDS:
					cast["timer"] = 0.0
					_deliver(ref, cast, true, false)
			"stop":
				print("npc magic: 0x%X ends 0x%08X: out of Magicka" % [ref, cast["spell"]])
				_casts.erase(ref)
				continue
		if cast["clip_left"] <= 0.0 and cast["clip"] != "":
			_play_clip(ref, cast, cast["clip"])


## Play the third-person clip for `what` and remember it to repeat while the spell is held.
func _play(ref: int, cast: Dictionary, what: String) -> void:
	var info: Dictionary = cast["info"]
	var self_cast := int(info.get("delivery", Magic.DELIVER_AIMED)) == Magic.DELIVER_SELF
	var names: Array = []
	match what:
		"chargeloop":
			names = ["mrh_selfchargeloop", "mrh_chargeloop"] if self_cast else ["mrh_chargeloop"]
		"concentration":
			names = ["mrh_selfconcentration", "mrh_aimedconcentration"] if self_cast else ["mrh_aimedconcentration", "mrh_chargeloop"]
		"release":
			names = ["mrh_release"]
		"selfrelease":
			names = ["mrh_selfrelease", "mrh_release"]
	for stem in names:
		var file := _combat.clip_of(ref, stem)
		if file != "":
			cast["clip"] = file if what in ["chargeloop", "concentration"] else ""
			_play_clip(ref, cast, file)
			return
	_warn.once("noclip" + what, "no third-person clip for %s in the project of 0x%X (tried %s)" % [what, ref, names])


func _play_clip(ref: int, cast: Dictionary, file: String) -> void:
	var node := _world.find_ref_node(ref)
	if node != null and node.has_method("play_action"):
		cast["clip_left"] = float(node.play_action(file, 1.0))
		cast["clip_file"] = file
