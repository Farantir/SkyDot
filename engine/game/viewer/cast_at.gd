# SPDX-License-Identifier: GPL-3.0-or-later
#
# --cast-at 0xREF[,left|right]: the player readies his hands, faces that actor and casts the
# spell in the hand at it (for shots and tests), as a finger on the attack button would: a
# fire-and-forget spell is held until it is charged and released, a concentration spell is held
# until the actor is dead or the Magicka is gone. Shared by the flat viewer and the VR shell;
# `step()` runs every frame.
class_name CastAt
extends RefCounted

var _session: GameSession
var _rig: PlayerRig
var _ref: int
var _hand: int
var _pressed := false
var _done := false
var _warned := false


func _init(session: GameSession, rig: PlayerRig, ref: int, hand: int) -> void:
	_session = session
	_rig = rig
	_ref = ref
	_hand = hand


func is_done() -> bool:
	return _done


func step() -> void:
	if _done or _ref == 0:
		return
	var node := _session.world.find_ref_node(_ref) as Node3D
	if node == null:
		return
	var magic := _session.magic
	var spell := magic.spell_in(_hand)
	if spell == 0:
		if not _warned:
			_warned = true
			print("--cast-at: nothing in hand %d (see --equip-spell)" % _hand)
		_done = true
		return
	if _session.papyrus.is_actor_dead(_ref):
		if _pressed:
			_release()
		print("--cast-at: 0x%08X is dead" % _ref)
		_done = true
		return
	_face(node)
	if _hand == Magic.POWER:
		print("--cast-at: power 0x%08X: %s" % [spell, _session.cast_power()])
		_done = true
		return
	if magic.ready_state() == Magic.Ready.SHEATHED:
		_session.toggle_weapon()
		return
	if magic.ready_state() != Magic.Ready.READY:
		return
	if not _pressed:
		_pressed = true
		print("--cast-at: 0x%08X at 0x%08X with hand %d" % [spell, _ref, _hand])
		if _hand == Magic.LEFT:
			_session.attack_left_pressed()
		else:
			_session.attack_pressed()
		return
	if not magic.is_casting(_hand):
		_done = true  # out of Magicka, or the hand was emptied
		return
	if int(magic.info_of(spell).get("casting_type", 1)) != Magic.CAST_CONCENTRATION and magic.phase_of(_hand) == "charged":
		_release()
		print("--cast-at: 0x%08X released" % spell)
		_done = true


func _release() -> void:
	if _hand == Magic.LEFT:
		_session.attack_left_released()
	else:
		_session.attack_released()


## Look at the actor's chest, as --attack-at does.
func _face(node: Node3D) -> void:
	var eye := _rig.camera.global_position
	var chest := node.global_position + Vector3.UP * 1.2
	var offset := chest - eye
	var flat := Vector2(offset.x, offset.z).length()
	_rig.apply_look(atan2(-offset.x, -offset.z), atan2(offset.y, flat))
