# SPDX-License-Identifier: GPL-3.0-or-later
#
# Saves and loads: the scripts' state (SkydotPapyrus) and where the player is,
# as one sectioned file. F5 and F9 use the quicksave; --save-to and --load name a file.
class_name SaveService
extends RefCounted

const QUICKSAVE := "user://quicksave.skydot"
const PLYR_VERSION := 1
const FAVS_VERSION := 1
const JRNL_VERSION := 1
const INFO_VERSION := 1
const WMAP_VERSION := 1
## A save the journal's System tab makes: save001.skydot, save002.skydot, ... (the game numbers its saves).
const SAVE_PREFIX := "save"

## A save's script state is in place, the place is not entered yet: the parts that keep state of the
## game before it drop it now (GameSession._reset_game_state).
signal committed

## Where newest() looks; a test points it at a directory of its own so that tests running in
## parallel do not see or delete each other's saves.
var directory := "user://"
var _papyrus: SkydotPapyrus
var _place: Place
var _streamer: SkydotStreamer
var _player: PlayerBody
var _bridge: ScriptBridge
var _clock: SkydotClock
## The favorites to save and load (FAVS); a save without the section has none.
var favorites: Favorites
## The journal's log, stats and play time (JRNL); a save without the section starts it empty.
var journal: JournalLog
## The world map's custom marker and last exterior position (WMAP); a save without it has neither.
var world_map: WorldMap
## What the load list shows of a save (INFO): name, race, level, location ({} when nobody says).
var describe: Callable


func _init(papyrus: SkydotPapyrus, place: Place, streamer: SkydotStreamer, player: PlayerBody,
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
func save(path: String, extra := {}) -> bool:
	_bridge.record_graphs()
	var writer := SkydotSaveWriter.new()
	_papyrus.write_sections(writer)
	_clock.write_section(writer)
	writer.add_section("PLYR", PLYR_VERSION, _player_section())
	if favorites != null:
		writer.add_section("FAVS", FAVS_VERSION, favorites.save_bytes())
	if journal != null:
		writer.add_section("JRNL", JRNL_VERSION, journal.save_bytes())
	if world_map != null:
		writer.add_section("WMAP", WMAP_VERSION, world_map.save_bytes())
	var info: Dictionary = describe.call() if describe.is_valid() else {}
	info.merge(extra, true)
	info["saved_at"] = int(Time.get_unix_time_from_system())
	info["play_seconds"] = journal.play_seconds if journal != null else 0.0
	writer.add_section("INFO", INFO_VERSION, var_to_bytes(info))
	if writer.write(ProjectSettings.globalize_path(path)) != OK:
		printerr("cannot save to ", path, ": ", writer.get_error())
		return false
	print("saved ", path)
	return true


## Load a save in two phases: every section is checked and decoded first, and
## only then does the game change. A file that is missing, damaged or from
## another pack returns false and leaves the running game as it is.
## The newest save in `directory` (the quicksave and any other .skydot), "" if there is none.
func newest() -> String:
	var best := ""
	var best_time := 0
	for file in DirAccess.get_files_at(directory):
		if not file.ends_with(".skydot"):
			continue
		var path := directory + file
		var time := FileAccess.get_modified_time(path)
		if best == "" or time > best_time:
			best = path
			best_time = time
	return best


## The player's saves in `directory` (the quicksave and the numbered saves; other .skydot files there, a test's or a
## --save-to's, are not listed), newest first, as the journal's load list shows them: {path, number ("QUICK" or the
## save's number), location, name, race, level, play_time ("001.49.36"), date ("10/4/26"), shot (PNG bytes)}.
func list_saves() -> Array:
	var out := []
	for file in DirAccess.get_files_at(directory):
		var name := file.get_basename()
		var numbered := name.begins_with(SAVE_PREFIX) and name.substr(SAVE_PREFIX.length()).is_valid_int()
		if file.ends_with(".skydot") and (numbered or name == QUICKSAVE.get_file().get_basename()):
			var info := read_info(directory + file)
			info["path"] = directory + file
			info["modified"] = FileAccess.get_modified_time(directory + file)
			out.append(info)
	out.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return int(a["modified"]) > int(b["modified"]))
	return out


## A save's INFO section with the load list's texts, {} for a file that is no save.
func read_info(path: String) -> Dictionary:
	var reader := SkydotSaveReader.new()
	if reader.open(ProjectSettings.globalize_path(path)) != OK:
		return {}
	var info := {}
	if reader.has_section("INFO") and reader.get_section_version("INFO") == INFO_VERSION:
		var state: Variant = bytes_to_var(reader.get_section("INFO"))
		if typeof(state) == TYPE_DICTIONARY:
			info = state
	var file := path.get_file().get_basename()
	if path == QUICKSAVE or file == QUICKSAVE.get_file().get_basename():
		info["number"] = "QUICK"
	elif file.begins_with(SAVE_PREFIX) and file.substr(SAVE_PREFIX.length()).is_valid_int():
		info["number"] = "%03d" % file.substr(SAVE_PREFIX.length()).to_int()
	else:
		info["number"] = ""
	var seconds := int(info.get("play_seconds", 0.0))
	info["play_time"] = "%03d.%02d.%02d" % [seconds / 3600, (seconds / 60) % 60, seconds % 60]
	var when := Time.get_datetime_dict_from_unix_time(int(info.get("saved_at", FileAccess.get_modified_time(path))))
	info["date"] = "%d/%d/%02d" % [when["month"], when["day"], int(when["year"]) % 100]
	return info


## The path a new numbered save takes: one past the highest number in `directory`.
func next_save_path() -> String:
	var highest := 0
	for file in DirAccess.get_files_at(directory):
		var name := file.get_basename()
		if file.ends_with(".skydot") and name.begins_with(SAVE_PREFIX) and name.substr(SAVE_PREFIX.length()).is_valid_int():
			highest = maxi(highest, name.substr(SAVE_PREFIX.length()).to_int())
	return directory + "%s%03d.skydot" % [SAVE_PREFIX, highest + 1]


func delete(path: String) -> bool:
	return DirAccess.remove_absolute(ProjectSettings.globalize_path(path)) == OK


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
	var journal_state := {}
	if journal != null and reader.has_section("JRNL") and reader.get_section_version("JRNL") == JRNL_VERSION:
		journal_state = JournalLog.decode(reader.get_section("JRNL"))
	var favorite_state := {}
	if favorites != null and reader.has_section("FAVS") and reader.get_section_version("FAVS") == FAVS_VERSION:
		favorite_state = Favorites.decode(reader.get_section("FAVS"))
	var map_state := {}
	if world_map != null and reader.has_section("WMAP") and reader.get_section_version("WMAP") == WMAP_VERSION:
		map_state = WorldMap.decode(reader.get_section("WMAP"))
	_bridge.retire_graphs()
	_papyrus.commit_load()
	_clock.commit_load()
	committed.emit()
	if favorites != null:
		favorites.adopt(favorite_state)  # an older save has none
	if journal != null:
		journal.adopt(journal_state)
	if world_map != null:
		world_map.adopt(map_state)
	var at: Vector3 = _player.loaded_position
	if int(player["world"]) != 0:
		if not _place.enter_exterior(int(player["world"]), at, null):
			printerr("cannot enter the saved worldspace of ", path)
			return false
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
