# SPDX-License-Identifier: GPL-3.0-or-later
#
# Layout helpers for the pack tool's pages, which build their controls in code
# (the project keeps `.tscn` files minimal; see project.godot).
class_name PackToolUi
extends RefCounted

const LABEL_WIDTH := 150

const COLORS := {
	"ok": Color(0.55, 0.85, 0.55),
	"warn": Color(1.0, 0.78, 0.3),
	"error": Color(1.0, 0.45, 0.4),
	"info": Color(0.72, 0.72, 0.72),
}


## A titled block; returns the box its rows go into.
static func section(parent: Control, title: String) -> VBoxContainer:
	var heading := Label.new()
	heading.text = title
	heading.add_theme_font_size_override("font_size", 18)
	parent.add_child(heading)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	parent.add_child(box)
	var gap := Control.new()
	gap.custom_minimum_size.y = 8
	parent.add_child(gap)
	return box


## A label column, then `control` expanding, then `extra` controls.
static func row(parent: Control, label: String, control: Control, extra: Array = []) -> HBoxContainer:
	var box := HBoxContainer.new()
	var caption := Label.new()
	caption.text = label
	caption.custom_minimum_size.x = LABEL_WIDTH
	box.add_child(caption)
	control.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(control)
	for item in extra:
		box.add_child(item)
	parent.add_child(box)
	return box


## A wrapping line of explanation under a row, indented to the controls.
static func note(parent: Control) -> Label:
	var box := HBoxContainer.new()
	var indent := Control.new()
	indent.custom_minimum_size.x = LABEL_WIDTH
	box.add_child(indent)
	var label := Label.new()
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(label)
	parent.add_child(box)
	set_note(label, "", "info")
	return label


static func set_note(label: Label, text: String, level: String = "info") -> void:
	label.text = text
	label.add_theme_color_override("font_color", COLORS.get(level, COLORS["info"]))
	label.get_parent().visible = not text.is_empty()


static func button(text: String, callback: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.pressed.connect(callback)
	return b


static func line_edit(placeholder: String = "") -> LineEdit:
	var edit := LineEdit.new()
	edit.placeholder_text = placeholder
	return edit


## "se", or "se-fus-ro-dah" for a profile: a folder name for a pack.
static func pack_name(edition: String, profile: String = "") -> String:
	var name := edition if not edition.is_empty() and edition != "unknown" else "pack"
	if profile.is_empty():
		return name
	var slug := ""
	for c in profile.to_lower():
		if (c >= "a" and c <= "z") or (c >= "0" and c <= "9"):
			slug += c
		elif not slug.ends_with("-") and not slug.is_empty():
			slug += "-"
	slug = slug.trim_suffix("-")
	return name + "-" + slug if not slug.is_empty() else name
