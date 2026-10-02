# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Packs tab: packs the tool knows (written or opened by it, or found in the
# packs folder), each summarized by `bethconv info --json`; actions for the
# selected one; and the viewer's start options (`bethconv cell --json`).
class_name PackToolPacksPage
extends VBoxContainer

const RIVERWOOD_AT := "19458,-47900,300"
const RIVERWOOD_TARGET := "19458,-47252,-120"

const START_RIVERWOOD := 0
const START_WORLD := 1
const START_INTERIOR := 2

var _tool  # pack_tool.gd
var _tree: Tree
var _note: Label
var _actions: HBoxContainer
var _viewer_box: VBoxContainer
var _start_option: OptionButton
var _world_row: Control
var _world_option: OptionButton
var _at_edit: LineEdit
var _interior_row: Control
var _interior_filter: LineEdit
var _interior_list: ItemList
var _fly_check: CheckBox
var _quests_check: CheckBox
var _viewer_note: Label
var _delete_dialog: ConfirmationDialog

var _infos := {}      # path -> `bethconv info --json`
var _generation := 0  # answers for a list that was since refreshed are dropped
var _selected := ""
var _worlds: Array = []
var _interiors := PackedStringArray()


func setup(tool) -> void:
	_tool = tool
	add_theme_constant_override("separation", 6)

	var top := HBoxContainer.new()
	top.add_child(PackToolUi.button("Refresh", refresh))
	top.add_child(PackToolUi.button("Add a pack folder…", func() -> void:
		_tool.browse_dir(_tool.packs_root(), func(path: String) -> void:
			_tool.remember_pack(path)
			refresh())))
	add_child(top)

	_tree = Tree.new()
	_tree.columns = 6
	_tree.column_titles_visible = true
	_tree.hide_root = true
	_tree.select_mode = Tree.SELECT_ROW
	_tree.custom_minimum_size.y = 180
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	for column in 6:
		_tree.set_column_title(column, ["Pack", "Game", "Mods", "Size", "Stale", "Problems"][column])
		_tree.set_column_expand(column, column == 0)
		if column > 0:
			_tree.set_column_custom_minimum_width(column, 120)
	_tree.item_selected.connect(_on_selected)
	add_child(_tree)

	_note = Label.new()
	_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_note)

	_actions = HBoxContainer.new()
	_actions.add_child(PackToolUi.button("Update…", func() -> void:
		var info: Dictionary = _infos.get(_selected, {})
		var manifest: Dictionary = info.get("manifest", {})
		var input: Dictionary = manifest.get("input", {}).duplicate()
		input["max_texture"] = int(manifest.get("textures", {}).get("max_size", 0))
		input["encode"] = manifest.get("textures", {}).get("uncompressed", "keep")
		_tool.edit_pack(_selected, input)))
	_actions.add_child(PackToolUi.button("Open folder", func() -> void:
		OS.shell_open(_selected)))
	_actions.add_child(PackToolUi.button("Remove from list", func() -> void:
		_tool.forget_pack(_selected)
		refresh()))
	_actions.add_child(PackToolUi.button("Delete…", _ask_delete))
	add_child(_actions)

	_viewer_box = PackToolUi.section(self, "View")
	_start_option = OptionButton.new()
	_start_option.add_item("Riverwood (Tamriel)", START_RIVERWOOD)
	_start_option.add_item("A worldspace", START_WORLD)
	_start_option.add_item("An interior", START_INTERIOR)
	_start_option.item_selected.connect(func(_i: int) -> void: _show_start())
	PackToolUi.row(_viewer_box, "Start in", _start_option)
	_world_option = OptionButton.new()
	_world_option.item_selected.connect(_on_world_selected)
	_at_edit = PackToolUi.line_edit("X,Y,Z in game units")
	_at_edit.tooltip_text = "A spot below the ground is lifted onto it."
	_world_row = PackToolUi.row(_viewer_box, "Worldspace", _world_option, [_at_edit])
	_interior_filter = PackToolUi.line_edit("filter, e.g. whiterun")
	_interior_filter.text_changed.connect(func(_t: String) -> void: _fill_interiors())
	_interior_list = ItemList.new()
	_interior_list.custom_minimum_size.y = 140
	var interior_box := VBoxContainer.new()
	interior_box.add_child(_interior_filter)
	interior_box.add_child(_interior_list)
	_interior_row = PackToolUi.row(_viewer_box, "Interior", interior_box)
	_fly_check = CheckBox.new()
	_fly_check.text = "Start flying (V switches)"
	_quests_check = CheckBox.new()
	_quests_check.text = "Start the game's quests"
	_quests_check.button_pressed = true
	var options := HBoxContainer.new()
	options.add_child(_fly_check)
	options.add_child(_quests_check)
	PackToolUi.row(_viewer_box, "", options)
	PackToolUi.row(_viewer_box, "", PackToolUi.button("Open in the viewer", _launch))
	_viewer_note = PackToolUi.note(_viewer_box)
	_set_viewer_enabled(false)

	_delete_dialog = ConfirmationDialog.new()
	_delete_dialog.confirmed.connect(_delete)
	add_child(_delete_dialog)

	_show_start()
	refresh()


## Pack folders to show: remembered ones, then packs directly in the packs folder.
func _pack_paths() -> PackedStringArray:
	var paths: PackedStringArray = _tool.known_packs()
	var root: String = _tool.packs_root()
	var dir := DirAccess.open(root)
	if dir != null:
		for name in dir.get_directories():
			var path: String = _tool.pack_key(root.path_join(name))
			if FileAccess.file_exists(path.path_join("manifest.json")) and not paths.has(path):
				paths.append(path)
	return paths


func refresh() -> void:
	_tree.clear()
	_tree.create_item()
	_infos.clear()
	_generation += 1
	for path in _pack_paths():
		var item := _tree.create_item()
		item.set_text(0, path)
		item.set_metadata(0, path)
		item.set_text(1, "reading…")
		_tool.cli.query(PackedStringArray(["info", path, "--json"]),
			_on_info.bind(path, item, _generation))
	if _tree.get_root().get_child_count() == 0:
		_note.text = "No packs yet. Convert one on the Convert tab, or add a folder holding one."
	else:
		_note.text = ""
	_actions.visible = false
	_set_viewer_enabled(false)
	if not _selected.is_empty():
		select(_selected)


func _on_info(doc: Dictionary, path: String, item: TreeItem, generation: int) -> void:
	if generation != _generation:
		return  # the item belonged to a tree that was cleared since
	_infos[path] = doc
	if doc.has("error"):
		item.set_text(1, "not readable")
		item.set_tooltip_text(1, doc["error"])
		item.set_custom_color(1, PackToolUi.COLORS["error"])
		return
	var manifest: Dictionary = doc["manifest"]
	var input: Dictionary = manifest.get("input", {})
	# Packs from before the manifest's `input` key do not say what they are.
	var edition: String = input.get("edition", "")
	item.set_text(1, (edition.to_upper() + "  " if not edition.is_empty() else "")
		+ "%d plugins" % doc.get("plugins", 0))
	if input.get("kind", "") == "mo2":
		item.set_text(2, "%s (%d mods)" % [input.get("mo2_profile", "?"), input.get("mods", 0)])
	else:
		item.set_text(2, "none" if not input.is_empty() else "not recorded")
	item.set_text(3, BethconvCli.format_bytes(doc["disk"]["bytes"]))
	var blob = doc.get("blob")
	if blob is Dictionary:
		item.set_text(4, BethconvCli.format_bytes(blob["stale_bytes"]))
		if blob["stale_bytes"] > 1024 * 1024 * 1024:
			item.set_custom_color(4, PackToolUi.COLORS["warn"])
			item.set_tooltip_text(4, "Update with \"Remove assets this conversion no longer uses\" to reclaim it.")
	var report: Dictionary = manifest.get("report", {})
	item.set_text(5, "%d failed, %d warnings" % [report.get("failed", 0), report.get("warnings", 0)])
	if report.get("failed", 0) > 0:
		item.set_custom_color(5, PackToolUi.COLORS["warn"])
	if manifest.get("pack_format_version", 0) != SkydotPack.PACK_FORMAT_VERSION:
		item.set_custom_color(1, PackToolUi.COLORS["error"])
		item.set_tooltip_text(1, "Pack format %d; this engine reads %d. Convert it again." % [
			manifest.get("pack_format_version", 0), SkydotPack.PACK_FORMAT_VERSION])
	if path == _selected:
		_on_selected()


func select(path: String) -> void:
	path = _tool.pack_key(path)
	_selected = path
	var item := _tree.get_root().get_first_child() if _tree.get_root() != null else null
	while item != null:
		if item.get_metadata(0) == path:
			item.select(0)
			return
		item = item.get_next()


func _on_selected() -> void:
	var item := _tree.get_selected()
	if item == null:
		return
	var path: String = item.get_metadata(0)
	var changed := path != _selected or _worlds.is_empty()
	_selected = path
	_actions.visible = true
	var info: Dictionary = _infos.get(path, {})
	if info.is_empty() or info.has("error"):
		_note.text = info.get("error", "")
		_set_viewer_enabled(false)
		return
	var manifest: Dictionary = info["manifest"]
	var input: Dictionary = manifest.get("input", {})
	var lines := PackedStringArray()
	lines.append("%s; %d forms, %d cells." % [manifest.get("converter", "?"),
		manifest.get("records", {}).get("forms", 0), manifest.get("world", {}).get("cells", 0)])
	if input.is_empty():
		lines.append("Converted before packs recorded their inputs; \"Update…\" needs the inputs chosen again.")
	else:
		lines.append("From %s%s." % [input.get("data", "?"),
			(" with MO2 profile " + input.get("mo2_profile", "")) if input.get("kind") == "mo2" else ""])
	var max_size := int(manifest.get("textures", {}).get("max_size", 0))
	if max_size > 0:
		lines.append("Textures at most %d px." % max_size)
	_note.text = " ".join(lines)
	if changed:
		_worlds = []
		_interiors = PackedStringArray()
		_tool.cli.query(PackedStringArray(["cell", path, "--worlds", "--json"]), _on_worlds.bind(path))
		_tool.cli.query(PackedStringArray(["cell", path, "--list", "--json"]), _on_cells.bind(path))
	_set_viewer_enabled(manifest.has("world"))


func _on_worlds(doc: Dictionary, path: String) -> void:
	if path != _selected:
		return
	_worlds = doc.get("worlds", [])
	_world_option.clear()
	for w in _worlds:
		_world_option.add_item(w["editor_id"])
	if not _worlds.is_empty():
		_world_option.select(0)
		_on_world_selected(0)


func _on_cells(doc: Dictionary, path: String) -> void:
	if path != _selected:
		return
	_interiors = PackedStringArray()
	for cell in doc.get("cells", []):
		if cell["interior"]:
			_interiors.append(cell["editor_id"])
	_interiors.sort()
	_fill_interiors()


func _fill_interiors() -> void:
	_interior_list.clear()
	var filter := _interior_filter.text.to_lower()
	for name in _interiors:
		if filter.is_empty() or name.to_lower().contains(filter):
			_interior_list.add_item(name)


## A world's default spot: the middle of its bounds, on the ground.
func _on_world_selected(index: int) -> void:
	if index < 0 or index >= _worlds.size():
		return
	var b: Array = _worlds[index]["bounds"]
	_at_edit.text = "%d,%d,0" % [int((b[0] + b[2]) / 2.0), int((b[1] + b[3]) / 2.0)]


func _show_start() -> void:
	var start := _start_option.get_selected_id()
	_world_row.visible = start == START_WORLD
	_interior_row.visible = start == START_INTERIOR


func _set_viewer_enabled(on: bool) -> void:
	_viewer_box.modulate.a = 1.0 if on else 0.5
	for child in _viewer_box.find_children("*", "BaseButton", true, false):
		(child as BaseButton).disabled = not on


func _launch() -> void:
	var args := PackedStringArray()
	match _start_option.get_selected_id():
		START_RIVERWOOD:
			args.append_array(["--world", "Tamriel", "--at", RIVERWOOD_AT, "--target", RIVERWOOD_TARGET])
		START_WORLD:
			if _world_option.selected < 0:
				PackToolUi.set_note(_viewer_note, "This pack has no worldspaces.", "error")
				return
			args.append_array(["--world", _world_option.get_item_text(_world_option.selected),
				"--at", _at_edit.text.replace(" ", "")])
		START_INTERIOR:
			var picked := _interior_list.get_selected_items()
			if picked.is_empty():
				PackToolUi.set_note(_viewer_note, "Pick an interior.", "error")
				return
			args.append_array(["--cell", _interior_list.get_item_text(picked[0])])
	if _fly_check.button_pressed:
		args.append_array(["--walk", "off"])
	if not _quests_check.button_pressed:
		args.append_array(["--quests", "off"])
	var pid: int = _tool.launch_viewer(_selected, args)
	if pid <= 0:
		PackToolUi.set_note(_viewer_note, "Could not start the viewer.", "error")
	else:
		PackToolUi.set_note(_viewer_note, "Viewer started (process %d). F12 saves a screenshot with its place and settings; Esc releases the mouse; all keys are listed at the top of viewer/cell_viewer.gd." % pid, "ok")
		_tool.remember_pack(_selected)


# ---- deleting ---------------------------------------------------------------

## Files a pack consists of (formats/pack-format.md). Nothing else in the
## folder is touched.
const PACK_FILES := ["manifest.json", "records.fb", "world.fb", "vpath.idx", "assets.idx", "report.json"]


func _ask_delete() -> void:
	var info: Dictionary = _infos.get(_selected, {})
	var size := BethconvCli.format_bytes(info.get("disk", {}).get("bytes", 0))
	_delete_dialog.dialog_text = "Delete the pack in\n%s\n(%s)?\n\nOnly the pack's own files are removed; the folder is removed if that leaves it empty." % [_selected, size]
	_delete_dialog.popup_centered()


func _delete() -> void:
	var path := _selected
	if not FileAccess.file_exists(path.path_join("manifest.json")):
		_note.text = "Not deleted: %s holds no manifest.json." % path
		return
	var dir := DirAccess.open(path)
	if dir == null:
		_note.text = "Not deleted: cannot open %s." % path
		return
	for name in dir.get_files():
		if PACK_FILES.has(name) or (name.begins_with("assets-") and name.ends_with(".blob")):
			dir.remove(name)
	# A loose store: assets/<two hex digits>/<hash>.<ext>.
	if dir.dir_exists("assets"):
		_remove_tree(path.path_join("assets"))
	if dir.get_files().is_empty() and dir.get_directories().is_empty():
		DirAccess.remove_absolute(path)
	_tool.forget_pack(path)
	_selected = ""
	refresh()


func _remove_tree(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	for sub in dir.get_directories():
		_remove_tree(path.path_join(sub))
	for name in dir.get_files():
		dir.remove(name)
	DirAccess.remove_absolute(path)
