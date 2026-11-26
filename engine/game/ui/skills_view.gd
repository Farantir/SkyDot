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
# A Control that fills its host (GameMenu); the numbers and the actions are SkillsModel's.
class_name SkillsView
extends GameMenu

const STICK_THRESHOLD := 0.6
const PLAYER_NPC := 7

var model: SkillsModel
var sky: StatsSky
var _session: GameSession
var _chrome := Control.new()
var _in_tree := false
var _choice := 0   # the attribute under the level-up choice
var _stick_ready := true


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
func _may_close(force: bool) -> bool:
	return force or model == null or not model.choosing_attribute()


func _closed() -> void:
	sky.visible = false
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


func _menu_input(event: InputEvent) -> void:
	var handled := true
	if model.choosing_attribute():
		handled = _level_up_input(event)
	elif event is InputEventMouse:
		handled = _mouse_input(event)
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


## The mouse. The ring: a click on a skill's label or its constellation turns to it, on the faced one zooms in.
## A tree: the star under the mouse is the cursor's (the camera stays, so the stars stay under the mouse); a click
## on it asks to take its perk and a second click takes it; a right click goes back as cancel does.
func _mouse_input(event: InputEventMouse) -> bool:
	var at := (sky.get_global_transform_with_canvas().affine_inverse() * event.position) / sky.size
	var button := event as InputEventMouseButton
	var click := button != null and button.pressed and button.button_index == MOUSE_BUTTON_LEFT
	if button != null and button.pressed and button.button_index == MOUSE_BUTTON_RIGHT:
		var cancel := InputEventAction.new()
		cancel.action = &"ui_cancel"
		cancel.pressed = true
		return _tree_input(cancel) if _in_tree else _ring_input(cancel)
	if not _in_tree:
		if click:
			var skill := skill_at(event.position)
			if skill >= 0 and skill == sky.facing() and not sky.is_turning():
				_zoom_in()
			elif skill >= 0:
				sky.turn_to(skill)
		return button != null
	var star := sky.star_at(model.selected, at)
	if star < 0:
		return button != null
	if star != model.cursor:
		model.cursor = star
		model.cancel()
		model.note = ""
	if click:
		if model.pending.is_empty():
			model.ask_take()
		else:
			model.confirm()
			sky.set_status(model.selected, model.nodes)
	return button != null


## The skill a click at `position` (the viewport's pixels) means on the ring: its label, else the constellation
## nearest across; -1 if none.
func skill_at(position: Vector2) -> int:
	var local := _chrome.get_global_transform_with_canvas().affine_inverse() * position
	var skill := StatsMenuDraw.ring_skill_at(_chrome.size, model.skills.size(), rad_to_deg(sky.yaw), local)
	if skill >= 0:
		return skill
	return sky.tree_at((sky.get_global_transform_with_canvas().affine_inverse() * position) / sky.size)


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
