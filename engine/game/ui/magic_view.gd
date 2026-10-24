# SPDX-License-Identifier: GPL-3.0-or-later
#
# The magic menu: categories on the left (Powers, the five schools of spells, Other, Scrolls the
# player carries, Active Effects), the entries of the chosen one on the right. A spell row has
# its Magicka cost ("/s" for concentration) and L, R or LR for the hand(s) it is in; one the
# player cannot pay now is dimmed but listed. Up and down choose an entry, left and right a
# category, accept (Enter, E) puts it in the right hand, `menu_equip_left` (Q, the gamepad's Y) in
# the left; a power goes to the power slot whichever is used. Cancel, P or the `magic` action
# closes. Abilities are not spells and show only as Active Effects. This is the view, a Control
# that fills its host (the viewer's CanvasLayer, a VR shell's panel); it takes its input from the
# InputMap's ui_* actions, `magic` and `menu_equip_left`, and knows the game only through
# GameSession. While it is open it emits `engaged(true)` so the shell keeps the player still.
class_name MagicView
extends Control

signal engaged(open: bool)

const NORMAL := Color(0.92, 0.92, 0.92)
const DIM := Color(0.6, 0.6, 0.6)
const SELECTED := Color(1.0, 0.9, 0.5)
const EQUIPPED := Color(0.6, 0.9, 0.6)
const STICK_THRESHOLD := 0.6
const REFRESH_SECONDS := 0.5
const HOURS_PER_DAY := 24.0

## The magic skills' actor value index (Alteration 18 ... Restoration 22) -> school.
const SCHOOLS := {18: "Alteration", 19: "Conjuration", 20: "Destruction", 21: "Illusion", 22: "Restoration"}
const POWERS := "Powers"
const OTHER := "Other"
const SCROLLS := "Scrolls"
const EFFECTS := "Active Effects"
const NAMES: Array[String] = [POWERS, "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", OTHER, SCROLLS, EFFECTS]

var _session: GameSession
var _open := false
var _category := 0
var _selected := 0
var _categories: Array[Dictionary] = []  # {name, rows} of the categories shown
var _stick_ready := true
var _since_refresh := 0.0
## Whether the menu listens at all (false in runs that drive themselves).
var enabled: Callable = func() -> bool: return true
## What the key labels say, by action (the viewer's gamepad or keyboard).
var key_label: Callable = func(_action: StringName) -> String: return ""


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	session.items_changed.connect(_refresh)
	session.magic_effect_changed.connect(func(_t: int, _m: int, _a: int, _s: bool, _i: int) -> void: _refresh())


func is_open() -> bool:
	return _open


func open() -> void:
	if _open:
		return
	_open = true
	engaged.emit(true)
	_refresh()


func close() -> void:
	if not _open:
		return
	_open = false
	engaged.emit(false)
	queue_redraw()


## Names of the categories shown, in order.
func category_names() -> Array[String]:
	var out: Array[String] = []
	for category in _categories:
		out.append(category["name"])
	return out


## The rows of the category called `name`, [] if there is none.
func rows_of(name: String) -> Array:
	for category in _categories:
		if category["name"] == name:
			return category["rows"]
	return []


## Show the category called `name`.
func select_category(name: String) -> void:
	for i in _categories.size():
		if _categories[i]["name"] == name:
			_category = i
			_selected = 0
			queue_redraw()


## The row under the cursor, {} if the list is empty.
func current() -> Dictionary:
	var rows := _rows()
	return rows[_selected] if _selected < rows.size() else {}


func _rows() -> Array:
	return _categories[_category]["rows"] if _category < _categories.size() else []


## Rebuild the lists: the spells by school, the scrolls carried, the effects acting on the player.
func _refresh() -> void:
	if not _open:
		return
	var shown := _category_name()
	var by_name := {}
	for name in NAMES:
		by_name[name] = []
	var left := _session.spell_in_hand(Magic.LEFT)
	var right := _session.spell_in_hand(Magic.RIGHT)
	var power := _session.spell_in_hand(Magic.POWER)
	var magicka := _session.actor_value(SkydotPapyrus.PLAYER_REF, "Magicka")
	for info: Dictionary in _session.known_spells():
		var form := int(info["form"])
		var row := {"form": form, "name": info["name"], "kind": "spell", "cost": int(info["cost"]),
			"concentration": int(info["casting_type"]) == Magic.CAST_CONCENTRATION,
			"left": form == left, "right": form == right, "affordable": float(info["cost"]) <= magicka}
		if int(info["type"]) >= 2:
			row["kind"] = "power"
			row["right"] = false
			row["left"] = false
			row["power"] = form == power
			row["cooldown_hours"] = float(info["cooldown"]) * HOURS_PER_DAY
			by_name[POWERS].append(row)
		else:
			by_name[SCHOOLS.get(int(info["school"]), OTHER)].append(row)
	for item: Dictionary in _session.inventory():
		if item["category"] == "scroll" and item["playable"]:
			var form := int(item["form"])
			by_name[SCROLLS].append({"form": form, "name": item["name"], "kind": "scroll", "count": int(item["count"]),
				"left": form == left, "right": form == right, "affordable": true})
	for effect: Dictionary in _session.player_effects():
		if bool(effect["active"]) and String(effect["name"]) != "":
			by_name[EFFECTS].append({"form": int(effect["mgef"]), "name": effect["name"], "kind": "effect",
				"magnitude": float(effect["magnitude"]), "remaining": float(effect["remaining"]), "affordable": true})
	for name in NAMES:
		(by_name[name] as Array).sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return String(a["name"]) < String(b["name"]))
	_categories.clear()
	for name in NAMES:
		if name == OTHER and by_name[name].is_empty():
			continue
		_categories.append({"name": name, "rows": by_name[name]})
	_category = 0
	for i in _categories.size():
		if _categories[i]["name"] == shown:
			_category = i
	_selected = clampi(_selected, 0, maxi(_rows().size() - 1, 0))
	queue_redraw()


func _category_name() -> String:
	return _categories[_category]["name"] if _category < _categories.size() else ""


func _process(delta: float) -> void:
	if not _open:
		return
	_since_refresh += delta
	if _since_refresh >= REFRESH_SECONDS:
		_since_refresh = 0.0
		_refresh()


func _input(event: InputEvent) -> void:
	if not _open or not enabled.call():
		return
	var handled := true
	if event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"magic"):
		close()
	elif event.is_action_pressed(&"ui_down"):
		_move(1)
	elif event.is_action_pressed(&"ui_up"):
		_move(-1)
	elif event.is_action_pressed(&"ui_right"):
		_change_category(1)
	elif event.is_action_pressed(&"ui_left"):
		_change_category(-1)
	elif event is InputEventJoypadMotion and event.axis == JOY_AXIS_LEFT_Y:
		_stick(event.axis_value)
	elif event.is_action_pressed(&"ui_accept"):
		equip_current(Magic.RIGHT)
	elif event.is_action_pressed(&"menu_equip_left"):
		equip_current(Magic.LEFT)
	else:
		handled = event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	if handled:
		get_viewport().set_input_as_handled()


func _stick(value: float) -> void:
	if absf(value) < STICK_THRESHOLD:
		_stick_ready = true
	elif _stick_ready:
		_stick_ready = false
		_move(1 if value > 0.0 else -1)


func _move(step: int) -> void:
	var rows := _rows()
	if rows.is_empty():
		return
	_selected = (_selected + step + rows.size()) % rows.size()
	queue_redraw()


func _change_category(step: int) -> void:
	if _categories.is_empty():
		return
	_category = (_category + step + _categories.size()) % _categories.size()
	_selected = 0
	queue_redraw()


## Put the entry under the cursor in `hand` (Magic.LEFT, RIGHT); a power goes to the power slot.
## Active effects cannot be equipped.
func equip_current(hand: int) -> bool:
	var row := current()
	if row.is_empty() or row["kind"] == "effect":
		return false
	var done := _session.equip_spell(int(row["form"]), Magic.POWER if row["kind"] == "power" else hand)
	_refresh()
	return done


func _draw() -> void:
	if not _open:
		return
	var area := size
	var font := ThemeDB.fallback_font
	var font_size := int(maxf(14.0, area.y / 38.0))
	var line := font_size * 1.45
	var panel := Rect2(area * Vector2(0.1, 0.08), area * Vector2(0.8, 0.84))
	draw_rect(panel, Color(0.04, 0.04, 0.05, 0.88))
	draw_rect(panel, Color(0.5, 0.45, 0.35), false, 2.0)
	var left := panel.position.x + font_size
	var top := panel.position.y + font_size * 2.0
	_text(font, "Magic", font_size, Vector2(left, top - font_size * 0.4), SELECTED)
	var header := "Magicka %d / %d" % [roundi(_session.actor_value(SkydotPapyrus.PLAYER_REF, "Magicka")),
		roundi(_session.actor_value_max(SkydotPapyrus.PLAYER_REF, "Magicka"))]
	_text(font, header, font_size, Vector2(panel.end.x - font.get_string_size(header, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x - font_size, top - font_size * 0.4), NORMAL)
	var list_top := top + line
	for i in _categories.size():
		var current_category := i == _category
		_text(font, ("> " if current_category else "  ") + String(_categories[i]["name"]), font_size,
			Vector2(left, list_top + line * i), SELECTED if current_category else NORMAL)
	var columns_x := panel.position.x + panel.size.x * 0.24
	var cost_x := panel.end.x - panel.size.x * 0.24
	var hand_x := panel.end.x - panel.size.x * 0.08
	var rows := _rows()
	var shown := _category_name()
	if shown == EFFECTS:
		_text(font, "Magnitude", font_size, Vector2(cost_x, list_top - line * 0.1), DIM)
		_text(font, "Time left", font_size, Vector2(hand_x - panel.size.x * 0.06, list_top - line * 0.1), DIM)
	elif shown != SCROLLS:
		_text(font, "Cost", font_size, Vector2(cost_x, list_top - line * 0.1), DIM)
	var rows_fit := int((panel.end.y - list_top - line * 3.0) / line)
	var first := clampi(_selected - rows_fit / 2, 0, maxi(rows.size() - rows_fit, 0))
	for i in range(first, mini(first + rows_fit, rows.size())):
		var row: Dictionary = rows[i]
		var y := list_top + line * (i - first + 1)
		var in_hand: bool = row.get("left", false) or row.get("right", false) or row.get("power", false)
		var colour := EQUIPPED if in_hand else NORMAL
		if not row["affordable"]:
			colour = DIM
		if i == _selected:
			colour = SELECTED
		_text(font, row_label(row), font_size, Vector2(columns_x, y), colour)
		match row["kind"]:
			"effect":
				_text(font, magnitude_text(row["magnitude"]), font_size, Vector2(cost_x, y), colour)
				_text(font, time_text(row["remaining"]), font_size, Vector2(hand_x - panel.size.x * 0.06, y), colour)
			"scroll":
				_text(font, hands_text(row), font_size, Vector2(hand_x, y), colour)
			"power":
				_text(font, power_text(row), font_size, Vector2(cost_x, y), colour)
			_:
				_text(font, cost_text(row), font_size, Vector2(cost_x, y), colour)
				_text(font, hands_text(row), font_size, Vector2(hand_x, y), colour)
	if rows.is_empty():
		_text(font, "Nothing here", font_size, Vector2(columns_x, list_top + line), DIM)
	var hint := "%s right hand   %s left hand   %s close" % [_key(&"ui_accept", "Enter"), _key(&"menu_equip_left", "Q"), _key(&"magic", "P")]
	_text(font, hint, font_size, Vector2(left, panel.end.y - font_size * 0.8), DIM)


## "Flames", "Oakflesh Scroll (2)", with "E " before the power in the power slot.
static func row_label(row: Dictionary) -> String:
	var text: String = row["name"] if row["name"] != "" else "0x%08X" % int(row["form"])
	if row.get("count", 1) > 1:
		text += " (%d)" % int(row["count"])
	return ("E " + text) if row.get("power", false) else text


## "14", or "8/s" for a concentration spell.
static func cost_text(row: Dictionary) -> String:
	return "%d%s" % [int(row["cost"]), "/s" if row["concentration"] else ""]


## "L", "R", "LR" or "": the hands that hold it.
static func hands_text(row: Dictionary) -> String:
	return ("L" if row["left"] else "") + ("R" if row["right"] else "")


## A power's cost, with the game hours to wait if it was used lately.
static func power_text(row: Dictionary) -> String:
	var cost := cost_text(row) if int(row["cost"]) > 0 else "-"
	return cost if row["cooldown_hours"] <= 0.0 else "%s (%d h)" % [cost, ceili(row["cooldown_hours"])]


## "40", or "0.7" for a small one.
static func magnitude_text(magnitude: float) -> String:
	return "%d" % roundi(magnitude) if absf(magnitude) >= 10.0 or is_equal_approx(magnitude, roundf(magnitude)) else "%.1f" % magnitude


## Seconds as "45 s" or "2:05", "" for a constant effect (remaining < 0).
static func time_text(seconds: float) -> String:
	if seconds < 0.0:
		return ""
	var s := ceili(seconds)
	return "%d s" % s if s < 60 else "%d:%02d" % [s / 60, s % 60]


func _key(action: StringName, fallback: String) -> String:
	var label: String = key_label.call(action)
	return label if label != "" else fallback


func _text(font: Font, text: String, font_size: int, at: Vector2, colour: Color) -> void:
	draw_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, 3, Color(0.0, 0.0, 0.0, 0.8))
	draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)
