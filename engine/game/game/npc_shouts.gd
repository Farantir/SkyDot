# SPDX-License-Identifier: GPL-3.0-or-later
#
# NPC shouts in the scene (GAMEPLAY-PLAN.md, 7b-4): what a shouter's combat AI decides
# (SkydotAi.combat_shout: "start" it inhales, standing and facing the target; "release" the words are
# said) is carried out with the player's shout (Magic.npc_shout: SkydotPapyrus.cast_shout, the cone on
# every actor in it, the sounds, the stagger and the push the shout's own scripts and effects do). The
# third-person clips are the behaviour project's: `shoutinhale` and `shoutexhale` (a draugr's) or
# `mt_shout_inhale` and `mt_shout_exhale` with `_medium`/`_long` for two and three words (a human's),
# played with play_action. A project without them (a creature) shouts without a clip, with one
# warning. The voice line is the VoicePowers topic of the words said (the same topics as the player's),
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
			_play(ref, ["shoutinhale", "mt_shout_inhale"], "inhale")
		"release":
			_combat.npc_face(ref, target)
			var result := _magic.npc_shout(ref, target, shout)
			if not bool(result["ok"]):
				return
			var words := int(result["words"])
			var suffix: String = ["", "", "_medium", "_long"][clampi(words, 1, 3)]
			_play(ref, ["shoutexhale", "mt_shout_exhale" + suffix, "mt_shout_exhale"], "exhale")
			_voice(ref, words)
			shouted.emit(ref, shout, words, int(result["spell"]))


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
