# SPDX-License-Identifier: GPL-3.0-or-later
#
# The pack tool: converts a Skyrim install (optionally with a Mod Organizer 2
# profile) into a pack and opens packs in the viewer. It is the project's main
# scene; the viewer stays its own scene and runs as a separate process.
#
#   Convert  game, mods, output folder and settings; progress and the log
#   Packs    packs found on this machine, their size and problems; update,
#            open in the viewer, delete
#   Settings where bethconv is and where packs go by default
#
# Everything about installs, mods and packs comes from `bethconv` as JSON
# (BethconvCli); nothing here parses game files.
extends Control

const SETTINGS_PATH := "user://packtool.cfg"
const VIEWER_SCENE := "res://viewer/cell_viewer.tscn"

var cli := BethconvCli.new()
var settings := ConfigFile.new()
## Set before the node enters the tree to keep settings elsewhere (tests).
var settings_path := SETTINGS_PATH
## `bethconv detect --json`'s installs, once known.
var installs: Array = []

var _tabs: TabContainer
var _convert: PackToolConvertPage
var _packs: PackToolPacksPage
var _cli_edit: LineEdit
var _cli_note: Label
var _root_edit: LineEdit
var _dialog: FileDialog
var _dialog_callback := Callable()


func _ready() -> void:
	get_window().title = "SkyDot pack tool"
	get_window().min_size = Vector2i(900, 600)
	settings.load(settings_path)

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 12)
	add_child(margin)
	_tabs = TabContainer.new()
	margin.add_child(_tabs)

	_convert = PackToolConvertPage.new()
	_convert.name = "Convert"
	_tabs.add_child(_convert)
	_packs = PackToolPacksPage.new()
	_packs.name = "Packs"
	_tabs.add_child(_packs)
	_tabs.add_child(_build_settings_page())

	_dialog = FileDialog.new()
	_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_dialog.use_native_dialog = true
	_dialog.dir_selected.connect(_on_dialog_path)
	_dialog.file_selected.connect(_on_dialog_path)
	add_child(_dialog)

	_convert.setup(self)
	_packs.setup(self)
	locate_cli()


func _exit_tree() -> void:
	cli.shutdown()


# ---- shared state -----------------------------------------------------------

func setting(key: String, default: Variant = "") -> Variant:
	return settings.get_value("tool", key, default)


func set_setting(key: String, value: Variant) -> void:
	settings.set_value("tool", key, value)
	settings.save(settings_path)


## Default parent folder for new packs.
func packs_root() -> String:
	var root: String = setting("packs_root", "")
	if root.is_empty():
		root = OS.get_system_dir(OS.SYSTEM_DIR_DOCUMENTS).path_join("SkyDot Packs")
	return root


## One spelling per folder, so a folder typed as ".../sse/" and found as
## ".../sse" is one pack.
static func pack_key(path: String) -> String:
	var out := path.simplify_path()
	while out.length() > 1 and out.ends_with("/") and not out.ends_with(":/"):
		out = out.substr(0, out.length() - 1)
	return out


## Pack folders the tool has written or opened, most recent first.
func known_packs() -> PackedStringArray:
	var out := PackedStringArray()
	for path in PackedStringArray(setting("known_packs", PackedStringArray())):
		if not out.has(pack_key(path)):
			out.append(pack_key(path))
	return out


func remember_pack(path: String) -> void:
	path = pack_key(path)
	var list := known_packs()
	var at := list.find(path)
	if at >= 0:
		list.remove_at(at)
	list.insert(0, path)
	set_setting("known_packs", list)


func forget_pack(path: String) -> void:
	var list := known_packs()
	var at := list.find(pack_key(path))
	if at >= 0:
		list.remove_at(at)
	set_setting("known_packs", list)


func locate_cli() -> void:
	var found := cli.locate(setting("bethconv", ""))
	if found.is_empty():
		PackToolUi.set_note(_cli_note, "bethconv was not found. Build the converter "
			+ "(converter/README.md) or choose the binary above. Looked in:\n"
			+ "\n".join(BethconvCli.candidates(setting("bethconv", ""))), "error")
		_tabs.current_tab = 2
		return
	PackToolUi.set_note(_cli_note, "Using " + found, "ok")
	cli.query(PackedStringArray(["detect", "--json"]), _on_detected)


func _on_detected(doc: Dictionary) -> void:
	installs = doc.get("installs", [])
	_convert.installs_changed()


## A pack was written: list it and show it.
func pack_converted(path: String) -> void:
	path = pack_key(path)
	remember_pack(path)
	_packs.refresh()


## Fill the convert page from a pack's manifest `input` and switch to it.
func edit_pack(path: String, input: Dictionary) -> void:
	_convert.load_input(path, input)
	_tabs.current_tab = 0


## Open the Packs page on `path`.
func show_pack(path: String) -> void:
	_tabs.current_tab = 1
	_packs.select(path)


# ---- file dialogs -------------------------------------------------------------

func browse_dir(current: String, callback: Callable) -> void:
	_open_dialog(FileDialog.FILE_MODE_OPEN_DIR, current, PackedStringArray(), callback)


func browse_file(current: String, filters: PackedStringArray, callback: Callable) -> void:
	_open_dialog(FileDialog.FILE_MODE_OPEN_FILE, current, filters, callback)


func _open_dialog(mode: FileDialog.FileMode, current: String, filters: PackedStringArray,
		callback: Callable) -> void:
	_dialog.file_mode = mode
	_dialog.filters = filters
	if not current.is_empty():
		if DirAccess.dir_exists_absolute(current):
			_dialog.current_dir = current
		elif DirAccess.dir_exists_absolute(current.get_base_dir()):
			_dialog.current_dir = current.get_base_dir()
	_dialog_callback = callback
	_dialog.popup_centered_ratio(0.6)


func _on_dialog_path(path: String) -> void:
	if _dialog_callback.is_valid():
		_dialog_callback.call(path)


# ---- the viewer ---------------------------------------------------------------

## Start the viewer on a pack as its own process. `args` are the viewer's own
## (cell_viewer.gd), without --pack.
func launch_viewer(pack: String, args: PackedStringArray) -> int:
	var command := PackedStringArray()
	# An exported build is the game itself; the editor binary needs the project.
	if not OS.has_feature("template"):
		command.append_array(["--path", ProjectSettings.globalize_path("res://")])
	command.append(VIEWER_SCENE)
	command.append("--")
	command.append_array(["--pack", pack])
	command.append_array(args)
	return OS.create_process(OS.get_executable_path(), command)


# ---- settings page ------------------------------------------------------------

func _build_settings_page() -> Control:
	var page := VBoxContainer.new()
	page.name = "Settings"
	var tool := PackToolUi.section(page, "Converter")
	_cli_edit = PackToolUi.line_edit("found automatically")
	_cli_edit.text = setting("bethconv", "")
	_cli_edit.text_submitted.connect(func(text: String) -> void:
		set_setting("bethconv", text)
		locate_cli())
	PackToolUi.row(tool, "bethconv binary", _cli_edit, [
		PackToolUi.button("Browse…", func() -> void:
			browse_file(_cli_edit.text, PackedStringArray(), func(path: String) -> void:
				_cli_edit.text = path
				set_setting("bethconv", path)
				locate_cli())),
		PackToolUi.button("Find again", locate_cli),
	])
	_cli_note = PackToolUi.note(tool)

	var packs := PackToolUi.section(page, "Packs")
	_root_edit = PackToolUi.line_edit(packs_root())
	_root_edit.text = setting("packs_root", "")
	_root_edit.text_submitted.connect(func(text: String) -> void:
		set_setting("packs_root", text)
		_packs.refresh())
	PackToolUi.row(packs, "New packs go to", _root_edit, [
		PackToolUi.button("Browse…", func() -> void:
			browse_dir(packs_root(), func(path: String) -> void:
				_root_edit.text = path
				set_setting("packs_root", path)
				_packs.refresh())),
	])
	var hint := PackToolUi.note(packs)
	PackToolUi.set_note(hint, "Use a local SSD. Packs are one large blob (vanilla SE: about "
		+ "20 GB); bethconv refuses many small files on FUSE (NTFS via ntfs-3g) and spinning "
		+ "disks, and warns about large files there.", "info")
	return page
