# SPDX-License-Identifier: GPL-3.0-or-later
#
# The skills screen (the `skills` action: L, the gamepad's right stick click, VR: the right stick click) over
# SkillsModel. With a level banked it first asks for Health, Magicka or Stamina (up and down choose, accept
# spends the level). Then: the ring of skills along the top (Page Up / Page Down or the shoulder buttons change
# the skill), the selected skill's perk tree as stars joined by lines (taken filled, available ringed bright,
# locked dim; the cursor is a box), the star under the cursor described on the right, and the level, XP bar
# and perk points along the bottom. The arrows move the cursor, accept asks to take the perk and again to
# confirm, `menu_drop` (R, X) asks to make a skill at 100 legendary, cancel declines or closes. A Control that
# fills its host (see InventoryView for the contract); the numbers and the actions are SkillsModel's.
class_name SkillsView
extends Control

signal engaged(open: bool)

const NORMAL := Color(0.92, 0.92, 0.92)
const DIM := Color(0.5, 0.5, 0.5)
const SELECTED := Color(1.0, 0.9, 0.5)
const AVAILABLE := Color(0.6, 0.9, 1.0)
const TAKEN := Color(1.0, 0.85, 0.4)
const SHORT := Color(0.9, 0.5, 0.45)
const STICK_THRESHOLD := 0.6

var model: SkillsModel
var _session: GameSession
var _open := false
var _choice := 0   # the attribute under the level-up choice
var _stick_ready := true
var enabled: Callable = func() -> bool: return true
var key_label: Callable = func(_action: StringName) -> String: return ""


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE


func is_open() -> bool:
	return _open


func open() -> void:
	model = SkillsModel.new(_session)
	_choice = 0
	var was_open := _open
	_open = true
	queue_redraw()
	print("skills: opens (level %d, %d perk points%s)" % [model.header["level"], model.header["perk_points"],
		", level-up ready" if model.choosing_attribute() else ""])
	if not was_open:
		engaged.emit(true)


## The level-up attribute choice cannot be skipped: while it is asked the screen stays open unless `force`
## (a death, a load or the session ending).
func close(force := false) -> void:
	if not _open:
		return
	if not force and model != null and model.choosing_attribute():
		return
	_open = false
	engaged.emit(false)
	queue_redraw()


## Select a skill by AV or name (the debug flag); no-op for unknown ones.
func select_skill(skill: String) -> void:
	if model == null:
		return
	for i in model.skills.size():
		var s: Dictionary = model.skills[i]
		if String(s["name"]).to_lower().replace("-", "") == skill.to_lower().replace("-", "") or str(s["av"]) == skill:
			model.select_skill(i - model.selected)
	queue_redraw()


func _input(event: InputEvent) -> void:
	if not _open or not enabled.call():
		return
	var handled := true
	if model.choosing_attribute():
		handled = _level_up_input(event)
	elif event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"skills"):
		if model.pending.is_empty():
			close()
		else:
			model.cancel()
	elif event.is_action_pressed(&"ui_accept"):
		if not model.pending.is_empty():
			model.confirm()
		else:
			model.ask_take()
	elif event.is_action_pressed(&"menu_drop"):
		model.ask_legendary()
	elif event.is_action_pressed(&"ui_page_down") or _button(event, JOY_BUTTON_RIGHT_SHOULDER):
		model.select_skill(1)
	elif event.is_action_pressed(&"ui_page_up") or _button(event, JOY_BUTTON_LEFT_SHOULDER):
		model.select_skill(-1)
	elif event.is_action_pressed(&"ui_down"):
		model.move_cursor(Vector2.DOWN)
	elif event.is_action_pressed(&"ui_up"):
		model.move_cursor(Vector2.UP)
	elif event.is_action_pressed(&"ui_left"):
		model.move_cursor(Vector2.LEFT)
	elif event.is_action_pressed(&"ui_right"):
		model.move_cursor(Vector2.RIGHT)
	elif event is InputEventJoypadMotion and (event.axis == JOY_AXIS_LEFT_X or event.axis == JOY_AXIS_LEFT_Y):
		_stick(event)
	else:
		handled = event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	queue_redraw()
	if handled:
		get_viewport().set_input_as_handled()


func _level_up_input(event: InputEvent) -> bool:
	if event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"skills"):
		pass   # forced: no way out before an attribute is chosen
	elif event.is_action_pressed(&"ui_down"):
		_choice = (_choice + 1) % SkillsModel.ATTRIBUTES.size()
	elif event.is_action_pressed(&"ui_up"):
		_choice = (_choice + SkillsModel.ATTRIBUTES.size() - 1) % SkillsModel.ATTRIBUTES.size()
	elif event.is_action_pressed(&"ui_accept"):
		model.choose_attribute(SkillsModel.ATTRIBUTES[_choice])
	else:
		return event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	return true


func _button(event: InputEvent, index: int) -> bool:
	return event is InputEventJoypadButton and event.pressed and event.button_index == index


func _stick(event: InputEventJoypadMotion) -> void:
	var value: float = event.axis_value
	if absf(value) < STICK_THRESHOLD:
		_stick_ready = true
		return
	if not _stick_ready:
		return
	_stick_ready = false
	if event.axis == JOY_AXIS_LEFT_Y:
		model.move_cursor(Vector2.DOWN if value > 0.0 else Vector2.UP)
	else:
		model.move_cursor(Vector2.RIGHT if value > 0.0 else Vector2.LEFT)


# ---- drawing

func _draw() -> void:
	if not _open or model == null:
		return
	var area := size
	var font := ThemeDB.fallback_font
	var font_size := int(maxf(14.0, area.y / 38.0))
	var line := font_size * 1.45
	var panel := Rect2(area * Vector2(0.05, 0.05), area * Vector2(0.9, 0.9))
	draw_rect(panel, Color(0.03, 0.03, 0.05, 0.92))
	draw_rect(panel, Color(0.5, 0.45, 0.35), false, 2.0)
	_draw_bottom(font, font_size, line, panel)
	if model.choosing_attribute():
		_draw_level_up(font, font_size, line, panel)
		return
	_draw_ring(font, font_size, line, panel)
	var tree := Rect2(panel.position + Vector2(font_size, line * 4.0), Vector2(panel.size.x * 0.6, panel.size.y - line * 7.0))
	_draw_tree(font_size, tree)
	_draw_info(font, font_size, line, Rect2(Vector2(tree.end.x + font_size, tree.position.y),
		Vector2(panel.end.x - tree.end.x - font_size * 2.0, tree.size.y)))


func _draw_bottom(font: Font, font_size: int, line: float, panel: Rect2) -> void:
	var h: Dictionary = model.header
	var y := panel.end.y - font_size * 0.8
	var x := panel.position.x + font_size
	_text(font, "Level %d" % h["level"], font_size, Vector2(x, y), SELECTED, 200.0)
	var bar := Rect2(Vector2(x + font_size * 7.0, y - font_size), Vector2(panel.size.x * 0.35, font_size * 0.8))
	draw_rect(bar, Color(0.15, 0.15, 0.18))
	draw_rect(Rect2(bar.position, Vector2(bar.size.x * float(h["xp_fraction"]), bar.size.y)), Color(0.8, 0.7, 0.35))
	draw_rect(bar, DIM, false, 1.0)
	_text(font, "Perk points: %d" % h["perk_points"], font_size, Vector2(bar.end.x + font_size, y), NORMAL, 300.0)
	var hint := "%s take  %s legendary  PgUp/PgDn skill  %s close" % [_key(&"ui_accept", "Enter"), _key(&"menu_drop", "R"),
		_key(&"skills", "L")]
	if model.choosing_attribute():
		hint = "%s choose" % _key(&"ui_accept", "Enter")
	_text(font, hint, int(font_size * 0.85), Vector2(x, y - line), DIM, panel.size.x - font_size * 2.0)


func _draw_ring(font: Font, font_size: int, line: float, panel: Rect2) -> void:
	var count := model.skills.size()
	var width := (panel.size.x - font_size * 2.0) / 7.0
	for offset in range(-3, 4):
		var s: Dictionary = model.skills[posmod(model.selected + offset, count)]
		var x := panel.position.x + font_size + (offset + 3) * width
		var colour := SELECTED if offset == 0 else (NORMAL if absi(offset) == 1 else DIM)
		_text(font, "%s %d" % [s["name"], s["level"]], font_size, Vector2(x, panel.position.y + line), colour, width - 6.0)
		var bar := Rect2(Vector2(x, panel.position.y + line * 1.3), Vector2(width - 10.0, font_size * 0.4))
		draw_rect(bar, Color(0.15, 0.15, 0.18))
		draw_rect(Rect2(bar.position, Vector2(bar.size.x * float(s["fraction"]), bar.size.y)), colour)
	var s: Dictionary = model.current_skill()
	var legend := "   Legendary x%d" % s["legendary"] if int(s["legendary"]) > 0 else ""
	_text(font, "%s arc: %s %d (base %d)%s" % [s["arc"], s["name"], s["level"], s["base"], legend], font_size,
		Vector2(panel.position.x + font_size, panel.position.y + line * 2.8), SELECTED, panel.size.x - font_size * 2.0)


func _star_at(area: Rect2, pos: Vector2) -> Vector2:
	var inner := area.grow(-20.0)
	return inner.position + pos * inner.size


func _draw_tree(font_size: int, area: Rect2) -> void:
	var radius := font_size * 0.6
	for n: Dictionary in model.nodes:
		for l: int in n["links"]:
			var other := model.node(l)
			if other.is_empty():
				continue
			var colour := TAKEN if n["status"] == "taken" and other["status"] == "taken" else DIM
			draw_line(_star_at(area, n["pos"]), _star_at(area, other["pos"]), colour, 2.0)
	for n: Dictionary in model.nodes:
		var at := _star_at(area, n["pos"])
		match n["status"]:
			"taken":
				draw_circle(at, radius, TAKEN)
			"available":
				draw_circle(at, radius, Color(0.04, 0.04, 0.06))
				draw_arc(at, radius, 0.0, TAU, 24, AVAILABLE, 3.0)
			_:
				draw_arc(at, radius, 0.0, TAU, 24, DIM, 1.5)
		if n["index"] == model.cursor:
			draw_rect(Rect2(at - Vector2.ONE * (radius + 5.0), Vector2.ONE * (radius + 5.0) * 2.0), SELECTED, false, 2.0)


func _draw_info(font: Font, font_size: int, line: float, area: Rect2) -> void:
	var n: Dictionary = model.current_node()
	var y := area.position.y + line
	if n.is_empty():
		_text(font, "No perks", font_size, Vector2(area.position.x, y), DIM, area.size.x)
		return
	_text(font, String(n["name"]), font_size, Vector2(area.position.x, y), SELECTED, area.size.x)
	y += line
	if int(n["ranks"]) > 1:
		_text(font, "Rank %d of %d" % [n["rank"], n["ranks"]], font_size, Vector2(area.position.x, y), NORMAL, area.size.x)
		y += line
	if int(n["required_level"]) > 0:
		_text(font, "Requires skill %d" % n["required_level"], font_size, Vector2(area.position.x, y), NORMAL, area.size.x)
		y += line
	for part in _wrap(font, String(n["description"]), font_size, area.size.x):
		_text(font, part, font_size, Vector2(area.position.x, y), NORMAL, area.size.x)
		y += line
	var status: String = n["status"]
	var text := "Taken" if status == "taken" else ("Available" if status == "available" else String(n["blocker_text"]))
	_text(font, text, font_size, Vector2(area.position.x, y), TAKEN if status == "taken" else (AVAILABLE if status == "available" else SHORT), area.size.x)
	y += line * 1.5
	if not model.pending.is_empty():
		_text(font, "%s  %s yes   %s no" % [model.pending["text"], _key(&"ui_accept", "Enter"), _key(&"ui_cancel", "Esc")],
			font_size, Vector2(area.position.x, y), SELECTED, area.size.x)
	elif model.note != "":
		_text(font, model.note, font_size, Vector2(area.position.x, y), SHORT, area.size.x)


func _draw_level_up(font: Font, font_size: int, line: float, panel: Rect2) -> void:
	var x := panel.position.x + panel.size.x * 0.3
	var y := panel.position.y + panel.size.y * 0.25
	_text(font, "Level Up: choose an attribute (level %d, %d waiting)" % [model.header["level"], model.header["banked"]],
		font_size, Vector2(x, y), SELECTED, panel.size.x * 0.6)
	y += line * 2.0
	for i in SkillsModel.ATTRIBUTES.size():
		var av: int = SkillsModel.ATTRIBUTES[i]
		var label := "%s  +%d" % [model.attribute_name(av), model.attribute_gain()]
		if av == Progression.STAMINA:
			label += "  (and carry weight)"
		_text(font, ("> " if i == _choice else "  ") + label, font_size, Vector2(x, y), SELECTED if i == _choice else NORMAL, panel.size.x * 0.6)
		y += line


func _wrap(font: Font, text: String, font_size: int, width: float) -> Array[String]:
	var lines: Array[String] = []
	var current := ""
	for word in text.split(" "):
		var trial := word if current == "" else current + " " + word
		if current != "" and font.get_string_size(trial, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x > width:
			lines.append(current)
			current = word
		else:
			current = trial
	if current != "":
		lines.append(current)
	return lines


func _key(action: StringName, fallback: String) -> String:
	var label: String = key_label.call(action)
	return label if label != "" else fallback


func _text(font: Font, text: String, font_size: int, at: Vector2, colour: Color, width: float) -> void:
	while text.length() > 3 and font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x > width:
		text = text.substr(0, text.length() - 3) + ".."
	draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 3, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)
