# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Convert tab: which game, which mods, where the pack goes, how it is
# converted; then `bethconv convert --json` with progress and its log.
#
# Inputs are prefilled from `bethconv detect` and remembered between runs. The
# output folder is checked with `bethconv target` as it is typed, so a folder
# bethconv would refuse is reported before anything starts.
class_name PackToolConvertPage
extends ScrollContainer

const MODE_DATA := 0
const MODE_MO2 := 1

## Readable names for convert's phases (converter/docs/cli-json.md).
const PHASES := {
	"mount": "Reading archives and mod folders",
	"merge": "Merging plugins",
	"records": "Writing records.fb",
	"world": "Writing world.fb",
	"assets": "Converting meshes, textures and scripts",
	"finish": "Hashing plugins and writing the index",
}

var _tool  # pack_tool.gd
var _install_option: OptionButton
var _data_edit: LineEdit
var _game_note: Label
var _mode_option: OptionButton
var _mo2_rows: Array[Control] = []
var _mo2_edit: LineEdit
var _profile_option: OptionButton
var _mo2_note: Label
var _list_row: Control
var _list_edit: LineEdit
var _out_edit: LineEdit
var _target_note: Label
var _store_option: OptionButton
var _prune_check: CheckBox
var _advanced_check: CheckButton
var _advanced_rows: Array[Control] = []
var _filter_edit: LineEdit
var _limit_spin: SpinBox
var _convert_button: Button
var _cancel_button: Button
var _phase_label: Label
var _progress: ProgressBar
var _summary: Label
var _result_buttons: HBoxContainer
var _log: TextEdit
var _confirm: ConfirmationDialog
var _target_timer: Timer

var _mo2: Dictionary = {}       # last `bethconv mo2 --json`
var _mo2_request := 0           # answers to older requests are dropped
var _target: Dictionary = {}    # last `bethconv target --json`
var _out_touched := false       # the user typed an output folder: stop suggesting
var _running_out := ""
var _saw_result := false
var _cancelled := false
var _phase := ""
var _phase_started := 0.0
var _log_lines := 0


func setup(tool) -> void:
	_tool = tool
	horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	var page := VBoxContainer.new()
	page.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	page.add_theme_constant_override("separation", 4)
	add_child(page)

	# ---- game ----
	var game := PackToolUi.section(page, "Game")
	_install_option = OptionButton.new()
	_install_option.item_selected.connect(_on_install_selected)
	PackToolUi.row(game, "Install", _install_option)
	_data_edit = PackToolUi.line_edit("the game's Data folder")
	_data_edit.text_submitted.connect(func(_t: String) -> void: _on_data_changed())
	_data_edit.focus_exited.connect(_on_data_changed)
	PackToolUi.row(game, "Data folder", _data_edit, [
		PackToolUi.button("Browse…", func() -> void:
			_tool.browse_dir(_data_edit.text, func(path: String) -> void:
				_data_edit.text = path
				_on_data_changed())),
	])
	_game_note = PackToolUi.note(game)

	# ---- mods ----
	var mods := PackToolUi.section(page, "Mods")
	_mode_option = OptionButton.new()
	_mode_option.add_item("None, or installed into Data (manual, Vortex)", MODE_DATA)
	_mode_option.add_item("Mod Organizer 2 (also Wabbajack lists)", MODE_MO2)
	_mode_option.item_selected.connect(func(_i: int) -> void: _on_mode_changed())
	PackToolUi.row(mods, "Mod manager", _mode_option)
	_list_edit = PackToolUi.line_edit("plugins.txt; without one only the base game and DLC load")
	_list_row = PackToolUi.row(mods, "Load order", _list_edit, [
		PackToolUi.button("Browse…", func() -> void:
			_tool.browse_file(_list_edit.text, PackedStringArray(["*.txt ; Plugin lists"]),
				func(path: String) -> void: _list_edit.text = path)),
	])
	_mo2_edit = PackToolUi.line_edit("the folder holding ModOrganizer.ini")
	_mo2_edit.text_submitted.connect(func(_t: String) -> void: _read_mo2(""))
	_mo2_edit.focus_exited.connect(func() -> void:
		if _mo2_edit.text != _mo2.get("dir", ""):
			_read_mo2(""))
	_mo2_rows.append(PackToolUi.row(mods, "MO2 instance", _mo2_edit, [
		PackToolUi.button("Browse…", func() -> void:
			_tool.browse_dir(_mo2_edit.text, func(path: String) -> void:
				_mo2_edit.text = path
				_read_mo2(""))),
	]))
	_profile_option = OptionButton.new()
	_profile_option.item_selected.connect(func(i: int) -> void:
		_read_mo2(_profile_option.get_item_text(i)))
	_mo2_rows.append(PackToolUi.row(mods, "Profile", _profile_option))
	_mo2_note = PackToolUi.note(mods)

	# ---- output ----
	var output := PackToolUi.section(page, "Output")
	_out_edit = PackToolUi.line_edit("folder for the pack")
	_out_edit.text_changed.connect(func(_t: String) -> void:
		_out_touched = true
		_target_timer.start())
	PackToolUi.row(output, "Pack folder", _out_edit, [
		PackToolUi.button("Browse…", func() -> void:
			_tool.browse_dir(_out_edit.text, func(path: String) -> void:
				_out_edit.text = path
				_out_touched = true
				_check_target())),
	])
	_target_note = PackToolUi.note(output)
	_target_timer = Timer.new()
	_target_timer.one_shot = true
	_target_timer.wait_time = 0.4
	_target_timer.timeout.connect(_check_target)
	add_child(_target_timer)

	# ---- settings ----
	var conversion := PackToolUi.section(page, "Conversion")
	var textures := OptionButton.new()
	textures.add_item("Desktop: textures passed through (DDS, mip chains completed)")
	textures.disabled = true
	textures.tooltip_text = "Smaller and mobile profiles are planned (TOOLS-REQUIREMENTS.md, section 2)."
	PackToolUi.row(conversion, "Texture profile", textures)
	_store_option = OptionButton.new()
	_store_option.add_item("One blob file (recommended)")
	_store_option.add_item("A file per asset (debugging; slow disks refuse it)")
	_store_option.item_selected.connect(func(_i: int) -> void: _check_target())
	PackToolUi.row(conversion, "Asset storage", _store_option)
	_prune_check = CheckBox.new()
	_prune_check.text = "Remove assets this conversion no longer uses (compacts an updated pack)"
	PackToolUi.row(conversion, "", _prune_check)
	_advanced_check = CheckButton.new()
	_advanced_check.text = "Show options for quick test runs"
	_advanced_check.toggled.connect(func(on: bool) -> void:
		for r in _advanced_rows:
			r.visible = on)
	PackToolUi.row(conversion, "", _advanced_check)
	_filter_edit = PackToolUi.line_edit("e.g. meshes/weapons/ (records are always converted)")
	_advanced_rows.append(PackToolUi.row(conversion, "Only paths with", _filter_edit))
	_limit_spin = SpinBox.new()
	_limit_spin.max_value = 10000000
	_limit_spin.step = 100
	_limit_spin.tooltip_text = "Stop after this many files (0: all)"
	_advanced_rows.append(PackToolUi.row(conversion, "At most (files)", _limit_spin))
	for r in _advanced_rows:
		r.visible = false

	# ---- run ----
	var run := PackToolUi.section(page, "Convert")
	var buttons := HBoxContainer.new()
	_convert_button = PackToolUi.button("Convert", _on_convert)
	_convert_button.custom_minimum_size.x = 160
	buttons.add_child(_convert_button)
	_cancel_button = PackToolUi.button("Cancel", func() -> void:
		_cancelled = true
		_tool.cli.cancel())
	_cancel_button.disabled = true
	buttons.add_child(_cancel_button)
	run.add_child(buttons)
	_phase_label = Label.new()
	run.add_child(_phase_label)
	_progress = ProgressBar.new()
	_progress.custom_minimum_size.y = 22
	_progress.visible = false
	run.add_child(_progress)
	_summary = Label.new()
	_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	run.add_child(_summary)
	_result_buttons = HBoxContainer.new()
	_result_buttons.visible = false
	_result_buttons.add_child(PackToolUi.button("Show in Packs", func() -> void:
		_tool.show_pack(_running_out)))
	_result_buttons.add_child(PackToolUi.button("Open report.json", func() -> void:
		OS.shell_open(_running_out.path_join("report.json"))))
	run.add_child(_result_buttons)
	_log = TextEdit.new()
	_log.editable = false
	_log.custom_minimum_size.y = 220
	_log.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
	_log.placeholder_text = "The converter's output appears here."
	run.add_child(_log)

	_confirm = ConfirmationDialog.new()
	_confirm.confirmed.connect(_start)
	add_child(_confirm)

	_tool.cli.event.connect(_on_event)
	_tool.cli.log_line.connect(_append_log)
	_tool.cli.finished.connect(_on_finished)

	_restore()
	_update_buttons()


# ---- inputs ---------------------------------------------------------------------

func _restore() -> void:
	var s: ConfigFile = _tool.settings
	_data_edit.text = s.get_value("form", "data", "")
	_list_edit.text = s.get_value("form", "list", "")
	_mo2_edit.text = s.get_value("form", "mo2", "")
	_mode_option.select(s.get_value("form", "mode", MODE_DATA))
	_store_option.select(s.get_value("form", "store", 0))
	# A folder the user chose is kept; a suggestion is made again.
	if s.get_value("form", "out_manual", false):
		_out_edit.text = s.get_value("form", "out", "")
		_out_touched = not _out_edit.text.is_empty()
	_on_mode_changed()
	if _mode_option.selected == MODE_MO2 and not _mo2_edit.text.is_empty():
		_read_mo2(s.get_value("form", "profile", ""))
	_on_data_changed()


func _save() -> void:
	var s: ConfigFile = _tool.settings
	s.set_value("form", "data", _data_edit.text)
	s.set_value("form", "list", _list_edit.text)
	s.set_value("form", "mo2", _mo2_edit.text)
	s.set_value("form", "profile", _selected_profile())
	s.set_value("form", "mode", _mode_option.selected)
	s.set_value("form", "store", _store_option.selected)
	s.set_value("form", "out", _out_edit.text)
	s.set_value("form", "out_manual", _out_touched)
	s.save(_tool.settings_path)


## Called when `bethconv detect` has answered.
func installs_changed() -> void:
	_install_option.clear()
	var current := -1
	for i in _tool.installs.size():
		var install: Dictionary = _tool.installs[i]
		var label: String = install["edition_name"]
		if install.get("source", "") == "steam":
			label += "  (Steam, build %s)" % install.get("build_id", "?")
		else:
			label += "  (%s)" % install.get("root", "")
		_install_option.add_item(label)
		_install_option.set_item_metadata(i, i)
		if install["data"] == _data_edit.text:
			current = i
	# Metadata, not ids: an id of -1 means "use the index".
	_install_option.add_item("Another folder…")
	_install_option.set_item_metadata(_install_option.item_count - 1, -1)
	if _data_edit.text.is_empty() and not _tool.installs.is_empty():
		current = _preferred_install()
		_install_option.select(current)
		_on_install_selected(current)
	else:
		_on_data_changed()


## Special Edition if present: it is what the corpus tests cover most.
func _preferred_install() -> int:
	for i in _tool.installs.size():
		if _tool.installs[i]["edition"] == "se":
			return i
	return 0


func _on_install_selected(index: int) -> void:
	var id: int = _install_option.get_item_metadata(index)
	if id < 0:
		_tool.browse_dir(_data_edit.text, func(path: String) -> void:
			_data_edit.text = path
			_on_data_changed())
		return
	var install: Dictionary = _tool.installs[id]
	_data_edit.text = install["data"]
	if install.get("plugins_txt") != null:
		_list_edit.text = install["plugins_txt"]
	elif _mode_option.selected == MODE_DATA:
		_list_edit.text = ""
	_on_data_changed()


## The detected install whose Data folder is the one in the field, or {}.
func _current_install() -> Dictionary:
	for install in _tool.installs:
		if install["data"] == _data_edit.text:
			return install
	return {}


func _on_data_changed() -> void:
	var install := _current_install()
	_sync_install_option()
	if _data_edit.text.is_empty():
		PackToolUi.set_note(_game_note, "Choose the game's Data folder.", "warn")
	elif not DirAccess.dir_exists_absolute(_data_edit.text):
		PackToolUi.set_note(_game_note, "This folder does not exist.", "error")
	elif install.is_empty():
		PackToolUi.set_note(_game_note, "A folder that is not a detected install. The pack "
			+ "will hold whatever plugins and archives are in it.", "info")
	else:
		var lines := PackedStringArray()
		var level := "ok"
		if install.get("build_tested") == false:
			lines.append("Steam build %s has not been tested (the converter's tests ran against %s); it will probably work." % [
				install["build_id"], install["tested_build_id"]])
			level = "warn"
		elif install.get("build_tested") == true:
			lines.append("Tested build.")
		if install.get("plugins_txt") == null:
			lines.append("No plugins.txt: start the game's launcher once to write one; without it "
				+ "only the base game, DLC and Creation Club plugins load.")
			if level == "ok":
				level = "info"
		PackToolUi.set_note(_game_note, " ".join(lines), level)
	# The instance note compares its edition with this folder's.
	if _mode_option.selected == MODE_MO2 and not _mo2.is_empty():
		_show_mo2()
	_suggest_out()
	_update_buttons()


## Select the detected install matching the Data folder, or "Another folder…".
func _sync_install_option() -> void:
	for i in _install_option.item_count:
		var id: int = _install_option.get_item_metadata(i)
		if id >= 0 and _tool.installs[id]["data"] == _data_edit.text:
			_install_option.select(i)
			return
	if _install_option.item_count > 0:
		_install_option.select(_install_option.item_count - 1)


func _on_mode_changed() -> void:
	var mo2 := _mode_option.selected == MODE_MO2
	for r in _mo2_rows:
		r.visible = mo2
	_list_row.visible = not mo2
	PackToolUi.set_note(_mo2_note, "", "info")
	if mo2 and not _mo2_edit.text.is_empty() and _mo2.is_empty():
		_read_mo2("")
	elif mo2 and not _mo2.is_empty():
		_show_mo2()
	_suggest_out()


func _selected_profile() -> String:
	if _profile_option.selected < 0:
		return ""
	return _profile_option.get_item_text(_profile_option.selected)


func _read_mo2(profile: String) -> void:
	if _mo2_edit.text.is_empty():
		return
	var args := PackedStringArray(["mo2", _mo2_edit.text, "--json"])
	if not profile.is_empty():
		args.append_array(["--profile", profile])
	PackToolUi.set_note(_mo2_note, "Reading the instance…", "info")
	_mo2_request += 1
	_tool.cli.query(args, _on_mo2.bind(_mo2_request))


func _on_mo2(doc: Dictionary, request: int) -> void:
	if request != _mo2_request:
		return
	if doc.has("error"):
		_mo2 = {}
		_profile_option.clear()
		PackToolUi.set_note(_mo2_note, doc["error"], "error")
		_suggest_out()
		return
	_mo2 = doc
	_profile_option.clear()
	var chosen: String = doc["profile"]["name"] if doc.get("profile") != null else ""
	for name in doc.get("profiles", []):
		_profile_option.add_item(name)
		if name == chosen:
			_profile_option.select(_profile_option.item_count - 1)
	# The instance's own game folder, if it is on this machine; else a
	# detected install of the same edition.
	if doc.get("game_data") != null:
		_data_edit.text = doc["game_data"]
	elif _current_install().get("edition", "") != doc.get("edition", ""):
		for i in _tool.installs.size():
			if _tool.installs[i]["edition"] == doc.get("edition", ""):
				_data_edit.text = _tool.installs[i]["data"]
				break
	installs_changed()  # also shows the instance (_on_data_changed)


func _show_mo2() -> void:
	var doc := _mo2
	var lines := PackedStringArray()
	var level := "ok"
	lines.append("%s instance." % doc.get("game_name", "?"))
	var profile = doc.get("profile")
	if profile == null:
		lines.append(doc.get("profile_error", "No profile selected."))
		level = "error"
	else:
		lines.append("%d mods enabled (%d disabled), %d of %d plugins active." % [
			profile["mods_enabled"], profile["mods_disabled"], profile["plugins_active"],
			profile["plugins_listed"]])
		if not profile["missing"].is_empty():
			lines.append("%d enabled mods have no folder: %s." % [profile["missing"].size(),
				", ".join(PackedStringArray(profile["missing"]).slice(0, 5))])
			level = "warn"
	if doc.get("game_data") == null:
		lines.append("Its game folder (%s) is not on this machine: the Data folder above is used instead; it must be the same edition (%s)." % [
			doc.get("game_path_written", "?"), doc.get("edition", "?")])
		if level == "ok":
			level = "info"
	var install := _current_install()
	if not install.is_empty() and install["edition"] != doc.get("edition", ""):
		lines.append("The Data folder above is %s, but the instance is for %s." % [
			install["edition_name"], doc.get("game_name", "?")])
		level = "error"
	PackToolUi.set_note(_mo2_note, " ".join(lines), level)
	_suggest_out()


## Propose `<packs root>/<edition>[-<profile>]` until the user types a folder.
func _suggest_out() -> void:
	if _out_touched:
		return
	var edition: String = _current_install().get("edition", "pack")
	var profile := ""
	if _mode_option.selected == MODE_MO2 and _mo2.get("profile") != null:
		profile = _mo2["profile"]["name"]
		edition = _mo2.get("edition", edition)
	_out_edit.text = _tool.packs_root().path_join(PackToolUi.pack_name(edition, profile))
	_out_touched = false
	_check_target()


func _check_target() -> void:
	if _out_edit.text.is_empty():
		PackToolUi.set_note(_target_note, "Choose where the pack goes.", "warn")
		return
	_tool.cli.query(PackedStringArray(["target", _out_edit.text, "--json"]), _on_target)


func _on_target(doc: Dictionary) -> void:
	if doc.has("error"):
		_target = {}
		PackToolUi.set_note(_target_note, doc["error"], "error")
		return
	if doc.get("path", "") != _out_edit.text:
		return  # an answer for an older value of the field
	_target = doc
	var verdict: Dictionary = doc["loose" if _store_option.selected == 1 else "blob"]
	var lines := PackedStringArray()
	if doc["is_pack"]:
		lines.append("An existing pack: it will be updated; unchanged files are not converted again.")
	elif doc["exists"] and not doc["empty"]:
		lines.append("This folder is not empty and holds no pack.")
	if doc.get("free_bytes") != null:
		lines.append("%s free." % BethconvCli.format_bytes(doc["free_bytes"]))
	var fs = doc.get("filesystem")
	if fs is Dictionary:
		lines.append("On %s%s." % [fs["fs_type"], ", solid state" if fs.get("rotational") == false else ""])
	var level := "ok"
	if verdict["verdict"] == "refuse":
		level = "error"
		lines.append(verdict["reason"])
	elif verdict["verdict"] == "warn":
		level = "warn"
		lines.append(verdict["reason"])
	elif doc["exists"] and not doc["empty"] and not doc["is_pack"]:
		level = "warn"
	PackToolUi.set_note(_target_note, " ".join(lines), level)
	_update_buttons()


func _update_buttons() -> void:
	var running: bool = _tool.cli.is_running()
	var refused: bool = not _target.is_empty() and _target[
		"loose" if _store_option.selected == 1 else "blob"]["verdict"] == "refuse"
	_convert_button.disabled = running or refused or _data_edit.text.is_empty() \
		or _out_edit.text.is_empty() or _tool.cli.path.is_empty()
	_cancel_button.disabled = not running


## Fill the form from a pack's manifest `input` (Packs page, "Update").
func load_input(path: String, input: Dictionary) -> void:
	_out_edit.text = path
	_out_touched = true
	_data_edit.text = input.get("data", "")
	if input.get("kind", "data") == "mo2":
		_mode_option.select(MODE_MO2)
		_mo2_edit.text = input.get("mo2_instance", "")
		_on_mode_changed()
		_read_mo2(input.get("mo2_profile", ""))
	else:
		_mode_option.select(MODE_DATA)
		_list_edit.text = input.get("plugin_list", "")
		_on_mode_changed()
	installs_changed()
	_check_target()


# ---- running --------------------------------------------------------------------

func form() -> Dictionary:
	var f := {
		"data": _data_edit.text,
		"out": _out_edit.text,
		"store": "loose" if _store_option.selected == 1 else "blob",
		"prune": _prune_check.button_pressed,
	}
	if _mode_option.selected == MODE_MO2:
		f["mo2"] = _mo2_edit.text
		f["profile"] = _selected_profile()
	else:
		f["list"] = _list_edit.text
	if _advanced_check.button_pressed:
		f["filter"] = _filter_edit.text
		f["limit"] = int(_limit_spin.value)
	return f


func _on_convert() -> void:
	if _mode_option.selected == MODE_MO2 and _mo2.get("profile") == null:
		PackToolUi.set_note(_mo2_note, "Choose a readable MO2 instance and profile first.", "error")
		return
	if not _target.is_empty() and _target["exists"] and not _target["empty"] and not _target["is_pack"]:
		_confirm.dialog_text = "%s is not empty and holds no pack.\nWrite the pack into it anyway?" % _out_edit.text
		_confirm.popup_centered()
		return
	_start()


func _start() -> void:
	_save()
	var args := BethconvCli.convert_args(form())
	_log.text = ""
	_log_lines = 0
	_append_log("bethconv " + " ".join(args))
	_summary.text = ""
	_result_buttons.visible = false
	_saw_result = false
	_cancelled = false
	_running_out = _out_edit.text
	_phase = ""
	if not _tool.cli.start(args):
		_summary.text = "Could not start bethconv."
		return
	_progress.visible = true
	_progress.indeterminate = true
	_phase_label.text = "Starting…"
	_update_buttons()


func _on_event(data: Dictionary) -> void:
	match data.get("event", ""):
		"progress":
			_on_progress(data)
		"start":
			var input: Dictionary = data["input"]
			var lines := PackedStringArray()
			lines.append("%d plugins, %d sources, %d files in the mount" % [
				data["plugins"], data["sources"], data["unique_paths"]])
			if input["kind"] == "mo2":
				lines.append("profile %s with %d mods" % [input["mo2_profile"], input["mods"]])
			for problem in data["load_order_problems"]:
				lines.append("load order: " + problem)
			for failure in data["mount_failures"]:
				lines.append("not mounted: " + failure)
			if not data["unloaded_archives"].is_empty():
				lines.append("%d mod archives not loaded (no plugin of that name): %s" % [
					data["unloaded_archives"].size(),
					", ".join(PackedStringArray(data["unloaded_archives"]).slice(0, 3))])
			for line in lines:
				_append_log(line)
		"done":
			_saw_result = true
			var a: Dictionary = data["assets"]
			_summary.text = ("Done in %s: %d forms, %d cells; %d assets written, %d unchanged "
				+ "(%s in the store). %d failed, %d warnings.") % [
				_duration(data["elapsed"]), data["forms"], data["cells"], a["written"],
				a["deduped"], BethconvCli.format_bytes(a["store_bytes"]), data["failed"],
				data["warnings"]]
			_summary.add_theme_color_override("font_color",
				PackToolUi.COLORS["ok" if data["failed"] == 0 else "warn"])
			for failure in data["first_failures"]:
				_append_log("failed: %s (%s): %s" % [failure["vpath"], failure["stage"], failure["detail"]])
			_result_buttons.visible = true
			_tool.pack_converted(_running_out)
		"error":
			_saw_result = true
			_summary.text = data["message"]
			_summary.add_theme_color_override("font_color", PackToolUi.COLORS["error"])


func _on_progress(data: Dictionary) -> void:
	var phase: String = data["phase"]
	var done: int = data["done"]
	var total: int = data["total"]
	if phase != _phase:
		_phase = phase
		_phase_started = data["elapsed"]
	var text: String = PHASES.get(phase, phase)
	_progress.indeterminate = total <= 0 or (phase == "merge" and done < total)
	if total > 0:
		_progress.max_value = total
		_progress.value = done
		text += "  %d / %d" % [done, total]
		var spent: float = data["elapsed"] - _phase_started
		if done > 0 and done < total and spent > 3.0:
			text += ", about %s left" % _duration(spent / done * (total - done))
	_phase_label.text = text + "   (%s)" % _duration(data["elapsed"])


func _on_finished(code: int) -> void:
	_progress.visible = false
	_phase_label.text = ""
	if _cancelled:
		_summary.text = "Cancelled. The pack may be incomplete until it is converted again " \
			+ "(unchanged files are not converted twice)."
		_summary.add_theme_color_override("font_color", PackToolUi.COLORS["warn"])
	elif not _saw_result:
		_summary.text = "bethconv stopped unexpectedly (exit code %d); see the log." % code
		_summary.add_theme_color_override("font_color", PackToolUi.COLORS["error"])
	_update_buttons()
	_check_target()


func _append_log(text: String) -> void:
	# Keep the widget responsive on chatty runs.
	if _log_lines > 5000:
		return
	_log_lines += 1
	_log.text += text + "\n"
	_log.scroll_vertical = _log.get_line_count()


static func _duration(seconds: float) -> String:
	var s := int(round(seconds))
	if s < 60:
		return "%d s" % s
	if s < 3600:
		return "%d min %d s" % [s / 60, s % 60]
	return "%d h %d min" % [s / 3600, (s % 3600) / 60]
