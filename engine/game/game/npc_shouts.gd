# SPDX-License-Identifier: GPL-3.0-or-later
#
# NPC shouts in the scene (GAMEPLAY-PLAN.md, 7b-4): what a shouter's combat AI decides
# (SkydotAi.combat_shout: "start" it inhales, standing and facing the target; "release" the words are
# said) is carried out with the player's shout (Magic.npc_shout: SkydotPapyrus.cast_shout, the cone on
# every actor in it, the sounds, the stagger and the push the shout's own scripts and effects do). The
# third-person clips are the behaviour project's: `shoutinhale` and `shoutexhale` (a draugr's) or
# `mt_shout_inhale` and `mt_shout_exhale` with `_medium`/`_long` for two and three words (a human's),
# played with play_action. A project without them (a creature) shouts without a clip, with one
# warning. A dragon (clip_shout_inhale_* and clip_shout_exhale_* in its project, firebreath for a
# concentration spell and fireball for a fire-and-forget one, told apart by the spell's casting type) breathes
# with the exhale clip's events: the spell leaves its head at Voice_SpellFire_Event, a concentration one is
# held, landing every Magic.TICK_SECONDS, until shoutStop or the end of the clip (shoutStop is its last key,
# which a looping action does not always reach). The voice line is the VoicePowers topic of the words said (the same topics as the player's),
# spoken by the actor's own voice type.
class_name NpcShouts
extends RefCounted

## The VoicePowers topics by the number of words (as GameSession.SHOUT_TOPICS).
const TOPICS := {1: [0x13E01], 2: [0x13E00, 0x13DFF], 3: [0x13E00, 0x13DFE]}

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _magic: Magic
var _combat: Combat
var _speech: Speech
var _pack: SkydotPack
var _warn := WarnOnce.new("npc shout: ")
var _breaths := {}  # ref -> {spell, info, target, concentration, state: "wait"|"fire", tick, left}

const EVENT_FIRE := "Voice_SpellFire_Event"
const EVENT_STOP := "shoutStop"

## An actor said a shout: the actor, the shout, the words and the spell of the last one.
signal shouted(ref: int, shout: int, words: int, spell: int)


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, magic: Magic, combat: Combat, speech: Speech, pack: SkydotPack) -> void:
	_world = world
	_papyrus = papyrus
	_magic = magic
	_combat = combat
	_speech = speech
	_pack = pack


## SkydotAi.combat_shout.
func on_shout(ref: int, target: int, shout: int, phase: String) -> void:
	if _papyrus.is_actor_dead(ref):
		return
	match phase:
		"start":
			_combat.npc_face(ref, target)
			# The voice slot holds the shout (the VoicePowers lines are picked by the equipped shout).
			_papyrus.equip_shout(ref, shout)
			print("npc shout: 0x%X inhales for 0x%08X" % [ref, shout])
			var kind := _breath_kind(ref, shout)
			_play(ref, ["clip_shout_inhale_" + kind] if kind != "" else ["shoutinhale", "mt_shout_inhale"], "inhale")
		"release":
			_combat.npc_face(ref, target)
			var kind := _breath_kind(ref, shout)
			if kind != "":
				_breathe(ref, target, shout, kind)
				return
			var result := _magic.npc_shout(ref, target, shout)
			if not bool(result["ok"]):
				return
			var words := int(result["words"])
			var suffix: String = ["", "", "_medium", "_long"][clampi(words, 1, 3)]
			_play(ref, ["shoutexhale", "mt_shout_exhale" + suffix, "mt_shout_exhale"], "exhale")
			_voice(ref, words)
			shouted.emit(ref, shout, words, int(result["spell"]))


## "firebreath" or "fireball" if the project of `ref` has the breath clips of the last word's spell (told by
## its casting type: concentration or not), else "".
func _breath_kind(ref: int, shout: int) -> String:
	var words: Array = _world.get_shout(shout).get("words", [])
	if words.is_empty():
		return ""
	var info := _magic.info_of(int(words[words.size() - 1]["spell"]))
	var kind := "firebreath" if int(info.get("casting_type", 0)) == Magic.CAST_CONCENTRATION else "fireball"
	return kind if _combat.clip_of(ref, "clip_shout_exhale_" + kind) != "" else ""


## The exhale: the shout is said (cost and recovery), its clip plays and its events release the spell.
func _breathe(ref: int, target: int, shout: int, kind: String) -> void:
	var result := _magic.npc_shout(ref, target, shout, false)
	if not bool(result["ok"]):
		return
	var node := _world.find_ref_node(ref)
	var length := 0.0
	if node != null and node.has_method("play_action"):
		length = float(node.play_action(_combat.clip_of(ref, "clip_shout_exhale_" + kind), 1.0))
		print("npc shout: 0x%X plays clip_shout_exhale_%s (%.2f s)" % [ref, kind, length])
		if not node.has_meta("skydot_breath_events") and node.has_signal("animation_event"):
			node.set_meta("skydot_breath_events", true)
			node.connect("animation_event", func(event: String) -> void: _on_event(ref, event))
	var info: Dictionary = result["info"]
	_breaths[ref] = {"spell": int(result["spell"]), "info": info, "target": target,
		"concentration": int(info.get("casting_type", 0)) == Magic.CAST_CONCENTRATION, "state": "wait", "tick": 0.0,
		"left": length}
	var words := int(result["words"])
	_voice(ref, words)
	shouted.emit(ref, shout, words, int(result["spell"]))


func _on_event(ref: int, event: String) -> void:
	if not _breaths.has(ref):
		return
	var breath: Dictionary = _breaths[ref]
	if event == EVENT_FIRE and breath["state"] == "wait":
		breath["state"] = "fire"
		print("npc shout: 0x%X's %s leaves its head" % [ref, "breath" if breath["concentration"] else "fireball"])
		_deliver(ref, breath, false)
		if not breath["concentration"]:
			_breaths.erase(ref)
	elif event == EVENT_STOP:
		if breath["state"] == "fire":
			print("npc shout: 0x%X's breath ends" % ref)
		_breaths.erase(ref)


## Advance the held breaths by `delta` seconds.
func step(delta: float) -> void:
	for ref in _breaths.keys():
		var breath: Dictionary = _breaths[ref]
		breath["left"] -= delta
		if _papyrus.is_actor_dead(ref) or breath["left"] <= 0.0:
			_breaths.erase(ref)
			continue
		if breath["state"] == "fire" and breath["concentration"]:
			breath["tick"] += delta
			if breath["tick"] >= Magic.TICK_SECONDS:
				breath["tick"] = 0.0
				_deliver(ref, breath, true)


## The spell leaves the head towards the target (a tick of a held one: `again`).
func _deliver(ref: int, breath: Dictionary, again: bool) -> void:
	var eye := _combat.head_point_of(ref)
	var forward := (_combat.body_point_of(int(breath["target"])) - eye).normalized()
	if forward.length() < 0.5:
		var node := _world.find_ref_node(ref) as Node3D
		forward = -node.global_transform.basis.z if node != null else Vector3.FORWARD
	_magic.deliver_from(ref, Magic.RIGHT, int(breath["spell"]), breath["info"], eye, forward,
		forward.cross(Vector3.UP).normalized(), again)


func _voice(ref: int, words: int) -> void:
	for topic: int in TOPICS[clampi(words, 1, 3)]:
		var line := _papyrus.topic_line(ref, topic)
		if line.is_empty() or String(line.get("voice", "")).is_empty():
			print("npc shout: 0x%X has no voice for topic 0x%X" % [ref, topic])
			continue
		print("npc shout: 0x%X voice %s" % [ref, line["voice"]])
		_speech.say(ref, line)


func _play(ref: int, stems: Array, what: String) -> void:
	var node := _world.find_ref_node(ref)
	for stem: String in stems:
		var file := _clip(ref, stem)
		if file != "":
			if node != null and node.has_method("play_action"):
				node.play_action(file, 1.0)
				print("npc shout: 0x%X plays %s" % [ref, stem])
			return
	_warn.once("noclip" + what, "no %s clip in the project of 0x%X (tried %s)" % [what, ref, stems])


## The clip file of `stem` for `ref`: its behaviour project's own clip, else the file of that name
## beside the project (a draugr's shoutinhale.hkx is in meshes/actors/draugr/animations/ but not in the
## clips its graph lists); "" if there is none.
func _clip(ref: int, stem: String) -> String:
	var file := _combat.clip_of(ref, stem)
	if file != "":
		return file
	var behaviour := _combat.behaviour_of(ref)
	if behaviour == "":
		return ""
	var path := behaviour.get_base_dir() + "/animations/" + stem + ".hkx"
	return path if _pack.has(path) else ""
