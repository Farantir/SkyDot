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
var _player: Object  # GameSession's player contract: save_section, load_section, place_at
var _bridge: ScriptBridge
var _clock: SkydotClock


func _init(papyrus: SkydotPapyrus, place: Place, streamer: SkydotStreamer, player: Object,
		bridge: ScriptBridge, clock: SkydotClock) -> void:
	_papyrus = papyrus
	_place = place
	_streamer = streamer
	_player = player
	_bridge = bridge
	_clock = clock


## Every part of a save writes its own section (save/save_file.hpp,
## docs/adr/0002-game-state-saves-and-session.md): the scripts' (HEAD, VMST,
## RFST, ...) from SkydotPapyrus, PLYR here. The file is written to a
## temporary and renamed over the old one.
func save(path: String) -> bool:
	_bridge.record_graphs()
	var writer := SkydotSaveWriter.new()
	_papyrus.write_sections(writer)
	_clock.write_section(writer)
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
	if _clock.prepare_load(reader) != OK:
		_papyrus.cancel_load()
		printerr("cannot load ", path, ": ", _clock.get_last_error())
		return false
	_bridge.retire_graphs()
	_papyrus.commit_load()
	_clock.commit_load()
	var at: Vector3 = _player.loaded_position
	if int(player["world"]) != 0:
		_place.enter_exterior(int(player["world"]), at, null)
	else:
		_place.enter_interior(int(player["cell"]), at, null)
	_player.place_at(_player.loaded_position, _player.loaded_yaw, _player.loaded_pitch)
	print("loaded ", path)
	return true


## PLYR: where the player is. The place is the session's, the rest (position,
## look) the player object's own section.
func _player_section() -> PackedByteArray:
	return var_to_bytes({
		"cell": _place.cell_id,
		"world": _streamer.world_id,
		"player": _player.save_section(),
	})


## The PLYR section as a Dictionary, empty if it is missing or wrong (the
## player object reads its part, and keeps it for place_at).
func _parse_player(reader: SkydotSaveReader) -> Dictionary:
	if not reader.has_section("PLYR") or reader.get_section_version("PLYR") != PLYR_VERSION:
		return {}
	var state = bytes_to_var(reader.get_section("PLYR"))
	if typeof(state) != TYPE_DICTIONARY:
		return {}
	if typeof(state.get("cell")) != TYPE_INT or typeof(state.get("world")) != TYPE_INT \
			or typeof(state.get("player")) != TYPE_PACKED_BYTE_ARRAY:
		return {}
	if not _player.load_section(state["player"]):
		return {}
	return state
