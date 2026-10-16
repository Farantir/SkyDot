# SPDX-License-Identifier: GPL-3.0-or-later
#
# Saves and loads: the scripts' state (SkydotPapyrus) and where the player is,
# as one sectioned file. F5 and F9 use the quicksave; --save-to and --load name a file.
class_name SaveService
extends RefCounted

const QUICKSAVE := "user://quicksave.skydot"
const PLYR_VERSION := 1

var _papyrus: SkydotPapyrus
var _place: Place
var _streamer: SkydotStreamer
var _rig: PlayerRig
var _bridge: ScriptBridge


func _init(papyrus: SkydotPapyrus, place: Place, streamer: SkydotStreamer, rig: PlayerRig,
		bridge: ScriptBridge) -> void:
	_papyrus = papyrus
	_place = place
	_streamer = streamer
	_rig = rig
	_bridge = bridge


## Every part of a save writes its own section (save/save_file.hpp,
## docs/adr/0002-game-state-saves-and-session.md): the scripts' (HEAD, VMST,
## RFST, ...) from SkydotPapyrus, PLYR here. The file is written to a
## temporary and renamed over the old one.
func save(path: String) -> bool:
	_bridge.record_graphs()
	var writer := SkydotSaveWriter.new()
	_papyrus.write_sections(writer)
	writer.add_section("PLYR", PLYR_VERSION, _player_section())
	if writer.write(ProjectSettings.globalize_path(path)) != OK:
		printerr("cannot save to ", path, ": ", writer.get_error())
		return false
	print("saved ", path)
	return true


## Load a save in two phases: every section is checked and decoded first, and
## only then does the game change. A file that is missing, damaged or from
## another pack returns false and leaves the running game as it is.
func load_game(path: String) -> bool:
	var reader := SkydotSaveReader.new()
	if reader.open(ProjectSettings.globalize_path(path)) != OK:
		printerr("cannot read ", path, ": ", reader.get_error())
		return false
	var player := _parse_player(reader)
	if player.is_empty():
		printerr("not a usable save: ", path)
		return false
	if _papyrus.prepare_load(reader) != OK:
		printerr("cannot load ", path, ": ", _papyrus.get_last_error())
		return false
	_bridge.retire_graphs()
	_papyrus.commit_load()
	if int(player["world"]) != 0:
		_place.enter_exterior(int(player["world"]), player["position"], null)
	else:
		_place.enter_interior(int(player["cell"]), player["position"], null)
	_rig.apply_look(float(player["yaw"]), float(player["pitch"]))
	print("loaded ", path)
	return true


## PLYR: where the player is.
func _player_section() -> PackedByteArray:
	return var_to_bytes({
		"cell": _place.cell_id,
		"world": _streamer.world_id,
		"position": _rig.camera.position,
		"yaw": _rig.yaw,
		"pitch": _rig.pitch,
	})


## The PLYR section as a Dictionary, empty if it is missing or wrong.
func _parse_player(reader: SkydotSaveReader) -> Dictionary:
	if not reader.has_section("PLYR") or reader.get_section_version("PLYR") != PLYR_VERSION:
		return {}
	var state = bytes_to_var(reader.get_section("PLYR"))
	if typeof(state) != TYPE_DICTIONARY:
		return {}
	if typeof(state.get("cell")) != TYPE_INT or typeof(state.get("world")) != TYPE_INT \
			or typeof(state.get("position")) != TYPE_VECTOR3 or typeof(state.get("yaw")) != TYPE_FLOAT \
			or typeof(state.get("pitch")) != TYPE_FLOAT:
		return {}
	return state
