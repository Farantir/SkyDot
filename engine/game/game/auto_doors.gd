# SPDX-License-Identifier: GPL-3.0-or-later
#
# Automatic load doors (DOOR flag 0x02, the cave mouths' AutoLoadDoor01: an invisible marker without
# collision): the player is taken through when coming within fAutoDoorActivateDistance (350 units) of one,
# as an activation of it (locks and scripts still decide). A door only fires once the player has been seen
# outside its distance, so arriving next to one does not send the player straight back.
extends RefCounted
class_name AutoDoors

const AUTOMATIC := 0x02
## Seconds between checks.
const TICK := 0.1

var _session: GameSession
var _automatic := {}  # door ref -> bool, from its base's flags
var _armed := {}  # door ref -> true once the player was outside its distance
var _clock := 0.0


func _init(session: GameSession) -> void:
	_session = session


func reset() -> void:
	_armed.clear()


func on_place_left() -> void:
	_armed.clear()


## The player's feet at `feet` (Godot): activate the first armed automatic door within reach.
func step(delta: float, feet: Vector3) -> void:
	_clock += delta
	if _clock < TICK:
		return
	_clock = 0.0
	var reach := _session.world.get_gmst_float("fAutoDoorActivateDistance", 350.0)
	var at := SkydotWorld.godot_to_skyrim(feet)
	var doors: Dictionary = _session.streamer.get_load_doors()
	for ref: int in doors:
		var node := doors[ref] as Node3D
		if node == null or not node.is_inside_tree() or not _is_automatic(ref) or _session.papyrus.is_ref_disabled(ref):
			continue
		var near := SkydotWorld.godot_to_skyrim(node.global_position).distance_to(at) <= reach
		if not near:
			_armed[ref] = true
		elif _armed.get(ref, false):
			_armed.erase(ref)
			_session.activate_ref(ref)
			return


func _is_automatic(ref: int) -> bool:
	if not _automatic.has(ref):
		var info := _session.world.get_ref_info(_session.world.get_ref_cell(ref), ref)
		var base := _session.world.get_base(int(info.get("base", 0)))
		_automatic[ref] = int(base.get("flags", 0)) & AUTOMATIC != 0
	return _automatic[ref]
