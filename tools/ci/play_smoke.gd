# SPDX-License-Identifier: GPL-3.0-or-later
#
# packtool.sh --smoke: in the project packtool.sh runs, every script compiles
# against that project's class cache, and the pack tool finds bethconv.
extends SceneTree

var _failed := 0


func _init() -> void:
	_walk("res://")
	var found := ""
	for path in BethconvCli.candidates():
		if FileAccess.file_exists(path):
			found = path
			break
	if found.is_empty():
		printerr("play smoke: the pack tool finds no bethconv")
		_failed += 1
	print("play smoke: %s" % ("clean" if _failed == 0 else "%d failures" % _failed))
	quit(1 if _failed > 0 else 0)


func _walk(dir: String) -> void:
	for sub in DirAccess.get_directories_at(dir):
		if not sub.begins_with("."):
			_walk(dir.path_join(sub))
	for file in DirAccess.get_files_at(dir):
		if file.get_extension() != "gd":
			continue
		var path := dir.path_join(file)
		var script := ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE) as Script
		if script == null or not script.can_instantiate():
			printerr("play smoke: %s does not compile" % path)
			_failed += 1
