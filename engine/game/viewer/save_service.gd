# SPDX-License-Identifier: GPL-3.0-or-later
#
# Saves and loads: the scripts' state (SkydotPapyrus) and where the player is,
# as one file. F5 and F9 use the quicksave; --save-to and --load name a file.
class_name SaveService
extends RefCounted

const QUICKSAVE := "user://quicksave.skydot"
const FORMAT := 1

var _papyrus: SkydotPapyrus
var _place: Place
var _streamer: SkydotStreamer
var _rig: PlayerRig


func _init(papyrus: SkydotPapyrus, place: Place, streamer: SkydotStreamer, rig: PlayerRig) -> void:
	_papyrus = papyrus
	_place = place
	_streamer = streamer
	_rig = rig


## The scripts' state and where the camera is.
func save(path: String) -> void:
	var state := {
		"format": FORMAT,
		"papyrus": _papyrus.save_state(),
		"cell": _place.cell_id,
		"world": _streamer.world_id,
		"position": _rig.camera.position,
		"yaw": _rig.yaw,
		"pitch": _rig.pitch,
	}
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		printerr("cannot save to ", path)
		return
	file.store_buffer(var_to_bytes(state))
	print("saved ", path)


## Restore the scripts' state and enter the saved place. False if the file is
## missing or not a save.
func load_game(path: String) -> bool:
	if not FileAccess.file_exists(path):
		printerr("no save at ", path)
		return false
	var state = bytes_to_var(FileAccess.get_file_as_bytes(path))
	if typeof(state) != TYPE_DICTIONARY or state.get("format", 0) != FORMAT \
			or typeof(state.get("papyrus")) != TYPE_PACKED_BYTE_ARRAY:
		printerr("not a save: ", path)
		return false
	if _papyrus.load_state(state["papyrus"]) != OK:
		printerr("cannot load the scripts' state: ", _papyrus.get_last_error())
		return false
	if int(state.get("world", 0)) != 0:
		_place.enter_exterior(int(state["world"]), state["position"], null)
	else:
		_place.enter_interior(int(state["cell"]), state["position"], null)
	_rig.apply_look(float(state["yaw"]), float(state["pitch"]))
	print("loaded ", path)
	return true
