# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the player would do by activating what the viewpoint looks at, and how
# the game words it: a verb, a name and, for a lock, its level. The rules are
# Skyrim's prompt rules from the data (GAMEPLAY-PLAN.md, "Game layer surface");
# a shell only draws the result. Nothing here changes the game.
class_name ActivationPrompt
extends RefCounted

## Metres the viewpoint reaches (the ray `ScriptBridge.activate_in_view` casts).
const REACH := ScriptBridge.REACH
const OPEN_STATE_OPEN := 1
const NO_LOCK := -1
const REQUIRES_KEY := 255
## Base types that are picked up.
const TAKEN := ["WEAP", "ARMO", "MISC", "ALCH", "INGR", "KEYM", "AMMO", "SLGM", "SCRL"]
## LightFlags bit 1: a lantern or torch that can be carried.
const LIGHT_CARRIED := 2

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _host: Node
var _viewpoint: Camera3D
var _world_names := {}  # worldspace id -> FULL name
var _bed_keyword := 0


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, host: Node, viewpoint: Camera3D) -> void:
	_world = world
	_papyrus = papyrus
	_host = host
	_viewpoint = viewpoint
	_bed_keyword = world.find_keyword("FurnitureBedRoleKeyword")


## {ref, verb, name, lock_level} for what the viewpoint looks at within reach,
## or {} if there is nothing to activate or nothing to call it.
func target() -> Dictionary:
	var from := _viewpoint.global_position
	var to := from - _viewpoint.global_transform.basis.z * REACH
	var hit := _world.pick_ref(_host, from, to)
	if hit.is_empty():
		return {}
	return describe(hit["cell"], hit["ref"])


## The prompt for reference `ref` placed in `cell`; {} if it has none.
func describe(cell: int, ref: int) -> Dictionary:
	var info := _world.get_ref_info(cell, ref)
	if info.is_empty():
		return _actor_prompt(ref)
	var base: Dictionary = _world.get_base(info["base"])
	if base.is_empty():
		return {}
	var type: String = info["type"]
	var name: String = base.get("name", "")
	var verb := ""
	var lock_level := NO_LOCK
	match type:
		"DOOR":
			var door: Variant = info.get("door")
			if door is Dictionary and door["destination"] != 0:
				var destination := _destination_name(door)
				if not destination.is_empty():
					name = destination
				verb = "Open"
			else:
				verb = "Close" if _papyrus.get_open_state(ref) == OPEN_STATE_OPEN else "Open"
			lock_level = _papyrus.get_lock_level(ref)
		"CONT":
			verb = "Open"
			lock_level = _papyrus.get_lock_level(ref)
		"ACTI", "FLOR":
			verb = base.get("activate_text", "")
			if verb.is_empty():
				verb = "Activate"
		"FURN":
			verb = "Sleep" if _is_bed(base) else "Sit"
		"BOOK":
			verb = "Read"
		"LIGH":
			var light: Variant = base.get("light")
			if light is Dictionary and (light["flags"] & LIGHT_CARRIED) != 0:
				verb = "Take"
		_:
			if type in TAKEN:
				verb = "Take"
	if verb.is_empty() or name.is_empty():
		return {}
	if lock_level != NO_LOCK:
		var lock: Variant = info.get("lock")
		var key: int = lock["key"] if lock is Dictionary else 0
		verb = "Unlock" if key != 0 and _papyrus.get_item_count(SkydotPapyrus.PLAYER_REF, key) > 0 else "Pick lock"
	return {"ref": ref, "verb": verb, "name": name, "lock_level": lock_level}


## "Novice" ... "Master", or "Requires key" for lock level 255.
static func lock_text(level: int) -> String:
	if level == NO_LOCK:
		return ""
	if level >= REQUIRES_KEY:
		return "Requires key"
	for step: Array in [[100, "Master"], [75, "Expert"], [50, "Adept"], [25, "Apprentice"]]:
		if level >= step[0]:
			return step[1]
	return "Novice"


func _actor_prompt(ref: int) -> Dictionary:
	var actor := _world.get_actor(ref)
	var name: String = actor.get("name", "")
	if name.is_empty():
		return {}
	return {"ref": ref, "verb": "Search" if _papyrus.is_actor_dead(ref) else "Talk", "name": name,
		"lock_level": NO_LOCK}


## The name a load door shows: the cell it leads to, else its worldspace.
func _destination_name(door: Dictionary) -> String:
	var cell := _world.get_cell(door["destination_cell"])
	var name: String = cell.get("name", "")
	if name.is_empty() and cell.get("world", 0) != 0:
		name = _world_name(cell["world"])
	return name


func _world_name(id: int) -> String:
	if _world_names.is_empty():
		for entry: Dictionary in _world.list_worlds():
			_world_names[entry["id"]] = entry.get("name", "")
	return _world_names.get(id, "")


func _is_bed(base: Dictionary) -> bool:
	if String(base.get("editor_id", "")).containsn("bed"):
		return true
	return _bed_keyword != 0 and _world.get_base_keywords(base["id"]).has(_bed_keyword)
