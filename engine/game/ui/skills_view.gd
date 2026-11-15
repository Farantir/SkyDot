# SPDX-License-Identifier: GPL-3.0-or-later
#
# The skills screen (the `skills` action: L, the gamepad's right stick click, VR: the right stick click) as the
# game's StatsMenu: the 3D sky with every skill's constellation (StatsSky) under the movie's chrome
# (StatsMenuDraw), over SkillsModel. With a level banked it first asks for Magicka, Health or Stamina
# (LevelUpDraw over the sky; the arrows choose, accept spends the level, nothing leaves it).
#   The ring: left / right (Page Up / Page Down, the shoulders, the left stick) turn the sky while held and it
#   slows into a skill when let go; up or accept zooms into the facing skill's tree; down, cancel or the skills
#   key closes.
#   A tree: the camera flies to the star under the cursor and the arrows (left stick) move it to the nearest star
#   that way, down from the lowest star or cancel goes back to the ring; accept asks to take the perk
#   (sStatsPerkConfirm) and accept again takes it; `menu_drop` (R, X) asks to make a skill at 100 legendary.
# A Control that fills its host (see InventoryView for the contract); the numbers and the actions are SkillsModel's.
class_name SkillsView
extends Control

signal engaged(open: bool)

const STICK_THRESHOLD := 0.6
const PLAYER_NPC := 7

var model: SkillsModel
var sky: StatsSky
var _session: GameSession
var _chrome := Control.new()
var _open := false
var _in_tree := false
var _choice := 0   # the attribute under the level-up choice
var _stick_ready := true
var enabled: Callable = func() -> bool: return true
var key_label: Callable = func(_action: StringName) -> String: return ""


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	sky = StatsSky.new(session)
	sky.visible = false
	add_child(sky)
	_chrome.set_anchors_preset(Control.PRESET_FULL_RECT)
	_chrome.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_chrome.draw.connect(_draw_chrome)
	add_child(_chrome)


func is_open() -> bool:
	return _open


func open() -> void:
	model = SkillsModel.new(_session)
	_choice = 0
	_in_tree = false
	var was_open := _open
	_open = true
	sky.visible = true
	sky.build(model.trees())
	sky.open(model.selected)
	_redraw()
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
	sky.visible = false
	engaged.emit(false)
	_redraw()


## Select a skill by AV or name (the debug flag --open-skills); no-op for unknown ones. Steps after colons follow:
## "tree" zooms into it, then "up" / "down" / "left" / "right" move the cursor ("Destruction:tree:up").
func select_skill(skill: String) -> void:
	if model == null:
		return
	var steps := skill.split(":")
	var name := steps[0].to_lower().replace("-", "")
	for i in model.skills.size():
		var s: Dictionary = model.skills[i]
		if String(s["name"]).to_lower().replace("-", "") == name or str(s["av"]) == name:
			model.select_skill(i - model.selected)
			sky.open(model.selected)
	var dirs := {"up": Vector2.UP, "down": Vector2.DOWN, "left": Vector2.LEFT, "right": Vector2.RIGHT}
	for step in steps.slice(1):
		if step == "tree":
			_zoom_in()
		elif dirs.has(step) and _in_tree:
			_move(dirs[step])
	_redraw()


func _redraw() -> void:
	_chrome.queue_redraw()


func _process(_delta: float) -> void:
	if not _open or model == null:
		return
	# The ring follows the sky: the skill the camera faces is the selected one once it rests.
	if not _in_tree and not sky.is_turning() and sky.facing() != model.selected:
		model.select_skill(sky.facing() - model.selected)
	_redraw()


func _input(event: InputEvent) -> void:
	if not _open or not enabled.call():
		return
	var handled := true
	if model.choosing_attribute():
		handled = _level_up_input(event)
	elif _in_tree:
		handled = _tree_input(event)
	else:
		handled = _ring_input(event)
	_redraw()
	if handled:
		get_viewport().set_input_as_handled()


func _ring_input(event: InputEvent) -> bool:
	if event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"skills") or event.is_action_pressed(&"ui_down"):
		close()
	elif event.is_action_pressed(&"ui_accept") or event.is_action_pressed(&"ui_up"):
		_zoom_in()
	elif event.is_action(&"ui_right") or event.is_action(&"ui_page_down") or _button(event, JOY_BUTTON_RIGHT_SHOULDER):
		sky.turn(1.0 if event.is_pressed() else 0.0)
	elif event.is_action(&"ui_left") or event.is_action(&"ui_page_up") or _button(event, JOY_BUTTON_LEFT_SHOULDER):
		sky.turn(-1.0 if event.is_pressed() else 0.0)
	elif event is InputEventJoypadMotion and event.axis == JOY_AXIS_LEFT_X:
		var value: float = event.axis_value
		sky.turn(signf(value) if absf(value) >= STICK_THRESHOLD else 0.0)
	else:
		return event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	return true


func _tree_input(event: InputEvent) -> bool:
	if event.is_action_pressed(&"skills"):
		close()
	elif event.is_action_pressed(&"ui_cancel"):
		if model.pending.is_empty():
			_zoom_out()
		else:
			model.cancel()
	elif event.is_action_pressed(&"ui_accept"):
		if not model.pending.is_empty():
			model.confirm()
			sky.set_status(model.selected, model.nodes)
		else:
			model.ask_take()
	elif event.is_action_pressed(&"menu_drop"):
		model.ask_legendary()
	elif event.is_action_pressed(&"ui_down"):
		_move(Vector2.DOWN)
	elif event.is_action_pressed(&"ui_up"):
		_move(Vector2.UP)
	elif event.is_action_pressed(&"ui_left"):
		_move(Vector2.LEFT)
	elif event.is_action_pressed(&"ui_right"):
		_move(Vector2.RIGHT)
	elif event is InputEventJoypadMotion and (event.axis == JOY_AXIS_LEFT_X or event.axis == JOY_AXIS_LEFT_Y):
		_stick(event)
	else:
		return event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	return true


func _zoom_in() -> void:
	if model.nodes.is_empty():
		return
	sky.turn(0.0)
	_in_tree = true
	model.cursor = model.start_node()
	sky.zoom(model.selected, model.cursor)


func _zoom_out() -> void:
	_in_tree = false
	model.cancel()
	sky.zoom(-1, -1)


## Move the cursor; down from the lowest star leaves the tree.
func _move(dir: Vector2) -> void:
	var before := model.cursor
	model.move_cursor(dir)
	if model.cursor == before:
		if dir == Vector2.DOWN:
			_zoom_out()
		return
	sky.zoom(model.selected, model.cursor)


## The attribute under the level-up choice (the game's buttons stand Magicka, Health, Stamina left to right).
func _choice_av() -> int:
	return LevelUpDraw.ORDER[_choice]


func _level_up_input(event: InputEvent) -> bool:
	var count: int = LevelUpDraw.ORDER.size()
	if event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"skills"):
		pass   # forced: no way out before an attribute is chosen
	elif event.is_action_pressed(&"ui_down") or event.is_action_pressed(&"ui_right"):
		_choice = (_choice + 1) % count
	elif event.is_action_pressed(&"ui_up") or event.is_action_pressed(&"ui_left"):
		_choice = (_choice + count - 1) % count
	elif event.is_action_pressed(&"ui_accept"):
		model.choose_attribute(_choice_av())
	else:
		return event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	return true


func _button(event: InputEvent, index: int) -> bool:
	return event is InputEventJoypadButton and event.button_index == index


func _stick(event: InputEventJoypadMotion) -> void:
	var value: float = event.axis_value
	if absf(value) < STICK_THRESHOLD:
		_stick_ready = true
		return
	if not _stick_ready:
		return
	_stick_ready = false
	if event.axis == JOY_AXIS_LEFT_Y:
		_move(Vector2.DOWN if value > 0.0 else Vector2.UP)
	else:
		_move(Vector2.RIGHT if value > 0.0 else Vector2.LEFT)


# ---- drawing

func _draw_chrome() -> void:
	if not _open or model == null:
		return
	if model.choosing_attribute():
		if LevelUpDraw.available():
			LevelUpDraw.draw(_chrome, _choice_av())
		return
	if not StatsMenuDraw.available():
		return
	var perk := model.current_node() if _in_tree else {}
	var labels: Array = []
	if _in_tree:
		for n: Dictionary in model.nodes:
			var at: Variant = sky.star_screen(model.selected, int(n["index"]))
			if at == null:
				continue
			var text := String(n["name"])
			if int(n["ranks"]) > 1:
				text += " (%d/%d)" % [n["rank"], n["ranks"]]
			labels.append({"text": text, "at": at})
		if not model.pending.is_empty():
			perk = perk.duplicate()
			perk["description"] = _session.world.get_gmst_string("sStatsPerkConfirm", String(model.pending["text"])) \
				if model.pending["kind"] == "perk" and _session.world != null else String(model.pending["text"])
		elif model.note != "":
			perk = perk.duplicate()
			perk["description"] = model.note
	StatsMenuDraw.draw(_chrome, _player_info(), model.skills, model.selected, rad_to_deg(sky.yaw), perk, labels)


## The header's and the bottom bar's values: the player's name and race, level and its progress, perk points,
## Health, Magicka and Stamina.
func _player_info() -> Dictionary:
	var info := {"level": model.header["level"], "xp_fraction": model.header["xp_fraction"],
		"perk_points": model.header["perk_points"]}
	if _session.world != null:
		var plan: Dictionary = _session.world.get_npc_plan(PLAYER_NPC, SkydotPapyrus.PLAYER_REF)
		info["name"] = String(_session.world.get_npc(PLAYER_NPC).get("name", ""))
		info["race"] = String(_session.world.get_race(int(plan.get("race", 0))).get("name", ""))
	for av: String in ["Health", "Magicka", "Stamina"]:
		info[av.to_lower()] = Vector2(_session.actor_value(SkydotPapyrus.PLAYER_REF, av),
			_session.actor_value_max(SkydotPapyrus.PLAYER_REF, av))
	return info
