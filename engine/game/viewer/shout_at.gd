# SPDX-License-Identifier: GPL-3.0-or-later
#
# The shout flags. --teach-word 0xWOOP[,...] teaches words of power (known, locked), --unlock-word
# unlocks them, --equip-shout 0xSHOU puts a shout in the voice slot (it empties the power slot) and
# --dragon-souls N sets the player's DragonSouls, in this order, once the place is built (`apply`).
# --shout N[,0xREF]: the player faces that actor (if given) and says N words of the shout in the voice
# slot (see --equip-shout), as a finger on the voice key would: the key goes down, is held for the time
# of N words (fShoutTime1 for two, fShoutTime2 for three; one word needs no hold) and goes up. A shout
# with fewer usable words fires by itself earlier. Shared by the flat viewer and the VR shell; `step()`
# runs every frame. The log tells: `--shout: …` here and `shout: …` from Magic.
class_name ShoutAt
extends RefCounted

## Seconds held past a threshold, so the frame that crosses it counts it.
const MARGIN := 0.05

var _session: GameSession
var _rig: PlayerRig
var _words: int
var _ref: int
var _pressed := false
var _done := false


## Apply the word, shout and soul flags of `settings`, logged; the --shout run to step every frame, or null.
static func apply(session: GameSession, rig: PlayerRig, settings: ViewerSettings) -> ShoutAt:
	var papyrus := session.papyrus
	for word in settings.teach_word:
		print("--teach-word: 0x%08X %s" % [word, "taught" if papyrus.teach_word(SkydotPapyrus.PLAYER_REF, word) else "not taught"])
	for word in settings.unlock_word:
		print("--unlock-word: 0x%08X %s" % [word, "unlocked" if papyrus.unlock_word(SkydotPapyrus.PLAYER_REF, word) else "not unlocked"])
	if settings.dragon_souls >= 0:
		papyrus.set_actor_value(SkydotPapyrus.PLAYER_REF, "DragonSouls", settings.dragon_souls)
		print("--dragon-souls: %d" % settings.dragon_souls)
	if settings.equip_shout != 0:
		print("--equip-shout: 0x%08X %s" % [settings.equip_shout, "ok" if session.equip_shout(settings.equip_shout) else "cannot be equipped"])
	if settings.shout_words != 0:
		return ShoutAt.new(session, rig, settings.shout_words, settings.shout_at)
	return null


func _init(session: GameSession, rig: PlayerRig, words: int, ref: int) -> void:
	_session = session
	_rig = rig
	_words = words
	_ref = ref


func is_done() -> bool:
	return _done


func step() -> void:
	if _done:
		return
	if _ref != 0:
		var node := _session.world.find_ref_node(_ref) as Node3D
		if node == null:
			return
		_face(node)
	var magic := _session.magic
	if not _pressed:
		_pressed = true
		var voice := _session.voice_pressed()
		print("--shout: %d words: voice key down: %s" % [_words, voice])
		if not voice["ok"]:
			_done = true
		return
	var state := magic.shout_state()
	if state.is_empty():
		_done = true  # the key is up already (refused, or the shout was cancelled)
		return
	var need := 0.0
	if _words == 2:
		need = _session.world.get_gmst_float(Magic.GMST_SHOUT_TIME1, Magic.SHOUT_TIME1_FALLBACK) + MARGIN
	elif _words >= 3:
		need = _session.world.get_gmst_float(Magic.GMST_SHOUT_TIME2, Magic.SHOUT_TIME2_FALLBACK) + MARGIN
	if bool(state["fired"]) or float(state["held"]) >= need:
		print("--shout: voice key up after %.2f s" % float(state["held"]))
		_session.voice_released()
		_done = true


## Look at the actor's chest, as --cast-at does.
func _face(node: Node3D) -> void:
	var eye := _rig.camera.global_position
	var chest := node.global_position + Vector3.UP * 1.2
	var offset := chest - eye
	var flat := Vector2(offset.x, offset.z).length()
	_rig.apply_look(atan2(-offset.x, -offset.z), atan2(offset.y, flat))
