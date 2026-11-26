# SPDX-License-Identifier: GPL-3.0-or-later
#
# The skills screen on the real SE pack (SKYDOT_SSE_PACK): the model's ring and header, the sky with every tree, the
# One-Handed tree (11 stars, Armsman available with a perk point), taking Armsman through the view's keys (accept
# zooms in, accept asks, accept confirms) and the status becoming taken, and the level-up choice when a level is banked. Headless; skips
# without the pack.
extends SceneTree

var failures := 0


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_skills_view: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _press(view: SkillsView, action: StringName) -> void:
	var event := InputEventAction.new()
	event.action = action
	event.pressed = true
	view._input(event)


func _mouse_event(view: SkillsView, at: Vector2, button := MOUSE_BUTTON_LEFT) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = at
	view._input(motion)
	if button == MOUSE_BUTTON_NONE:
		return
	var click := InputEventMouseButton.new()
	click.position = at
	click.button_index = button
	click.pressed = true
	view._input(click)


func _settle(view: SkillsView) -> void:
	var steps := 0
	while view.sky.is_turning() and steps < 600:
		view.sky._step_turn(1.0 / 60.0)
		steps += 1
	view.sky._move_t = 1.0
	view.sky._intro_t = -1.0
	view.sky._place_camera()
	view._process(0.0)


## The mouse: a click on a ring label or on a constellation turns to that skill, on the faced one zooms in; in the
## tree the star under the mouse is the cursor's, a click asks, a second takes, a right click goes back.
func _mouse(view: SkillsView, session: GameSession) -> void:
	var m := view.model
	var size := view.sky.size
	expect(size.x > 0.0 and size.y > 0.0, "the sky has a size: %s" % size)
	_mouse_event(view, size * 0.5, MOUSE_BUTTON_RIGHT)
	expect(not view._in_tree, "a right click leaves the tree")
	_settle(view)
	var start := view.sky.facing()
	# A ring label two skills to the right.
	var target := (start + 2) % 18
	var label := Vector2.ZERO
	for place: Array in StatsMenuDraw.ring_places(18, rad_to_deg(view.sky.yaw)):
		if place[0] == target:
			label = UiDraw.stage_fill(size, StatsMenuDraw.STAGE) * (StatsMenuDraw.RING_ORIGIN + Vector2(place[1], 5.0))
	expect(view.skill_at(label) == target, "the label at %s is skill %d: %d" % [label, target, view.skill_at(label)])
	_mouse_event(view, label)
	_settle(view)
	expect(view.sky.facing() == target and m.selected == target, "a click on its label turns to skill %d: %d, %d" % [target, view.sky.facing(), m.selected])
	# The constellation to the left.
	var left := (target + 17) % 18
	var at: Variant = view.sky.tree_screen(left)
	expect(at != null, "the left constellation is in front")
	if at == null:
		return
	var px: Vector2 = at * size
	expect(view.skill_at(px) == left, "the constellation at %s is skill %d: %d" % [px, left, view.skill_at(px)])
	_mouse_event(view, px)
	_settle(view)
	expect(view.sky.facing() == left and m.selected == left, "a click on it turns to skill %d: %d" % [left, view.sky.facing()])
	# The faced one: into its tree.
	view.select_skill("OneHanded")
	_settle(view)
	_mouse_event(view, (view.sky.tree_screen(view.sky.facing()) as Vector2) * size)
	expect(view._in_tree and m.current_skill()["av"] == 6, "a click on the faced constellation zooms into One-handed")
	_settle(view)
	session.papyrus.set_perk_points(1)
	m.refresh()
	var pick := {}
	for n: Dictionary in m.nodes:
		if n["status"] == "available" and n["index"] != m.cursor:
			pick = n
	expect(not pick.is_empty(), "another perk to take")
	if pick.is_empty():
		return
	var star: Vector2 = view.sky.star_screen(m.selected, int(pick["index"])) * size
	expect(view.sky.star_at(m.selected, star / size) == pick["index"], "the star under %s is %s" % [star, pick["name"]])
	_mouse_event(view, star, MOUSE_BUTTON_NONE)
	expect(m.cursor == pick["index"], "the mouse over %s puts the cursor there: %s" % [pick["name"], m.current_node().get("name")])
	_mouse_event(view, star)
	expect(m.pending.get("kind", "") == "perk", "a click asks to take %s" % pick["name"])
	_mouse_event(view, star)
	expect(int(m.node(pick["index"])["rank"]) == 1, "a second click takes %s" % pick["name"])


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	var session := GameSession.new()
	session.interactive = false
	session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(session)
	expect(session.open(config, viewpoint, PlayerBody.new()), "the session opens")
	if session.world == null:
		quit(1)
		return
	expect(session.start_game(), "the game starts")
	session.papyrus.set_perk_points(1)
	var view := SkillsView.new(session)
	root.add_child(view)
	view.open()
	view.select_skill("OneHanded")
	var m := view.model
	# The ring turns while a direction is held and slows into the next skill when let go.
	var start := view.sky.facing()
	view.sky.turn(1.0)
	for i in 6:  # a tenth of a second held
		view.sky._step_turn(1.0 / 60.0)
	view.sky.turn(0.0)
	var steps := 0
	while view.sky.is_turning() and steps < 600:
		view.sky._step_turn(1.0 / 60.0)
		steps += 1
	expect(view.sky.facing() == (start + 1) % 18, "a tap turns one skill to the right: %d -> %d" % [start, view.sky.facing()])
	view.select_skill("OneHanded")
	expect(m.skills.size() == 18, "18 skills")
	expect(view.sky._stars.size() == 18, "every tree's stars in the sky: %d" % view.sky._stars.size())
	expect(m.current_skill()["av"] == 6, "One-handed is selected: %s" % [m.current_skill()["name"]])
	expect(m.nodes.size() == 10, "11 tree nodes, the root not a star: %d stars" % m.nodes.size())
	expect(m.header["perk_points"] == 1, "one perk point")
	var armsman := {}
	for n: Dictionary in m.nodes:
		if n["name"] == "Armsman":
			armsman = n
	expect(not armsman.is_empty() and armsman["status"] == "available", "Armsman is available: %s" % [armsman.get("status")])
	expect(armsman.get("ranks", 0) == 5, "Armsman has five ranks: %s" % [armsman.get("ranks")])
	expect(String(armsman.get("description", "")).contains("20%"), "with its description: %s" % [armsman.get("description")])
	_press(view, &"ui_accept")
	expect(view._in_tree, "accept zooms into the tree")
	expect(m.current_node().get("name", "") == "Armsman", "the cursor starts on its lowest star")
	expect(view.sky.zoomed(), "the camera flies to it")
	_press(view, &"ui_accept")
	expect(m.pending.get("kind", "") == "perk", "accept asks")
	_press(view, &"ui_accept")
	armsman = m.node(armsman["index"])
	expect(armsman["rank"] == 1, "Armsman rank 1 is taken: %s" % [armsman["rank"]])
	expect(m.header["perk_points"] == 0, "the point is spent")
	expect(armsman["status"] == "locked" and armsman["blocker_text"] != "", "the next rank is locked: %s" % [armsman["blocker_text"]])
	_mouse(view, session)
	# A banked level asks for an attribute first.
	session.papyrus.set_player_xp(100000.0)
	view.open()
	expect(view.model.choosing_attribute(), "a banked level asks for the attribute")
	var level := int(view.model.header["level"])
	_press(view, &"ui_cancel")
	expect(view.is_open(), "cancel does not leave the level-up choice")
	_press(view, &"skills")
	expect(view.is_open(), "the skills key does not leave the level-up choice")
	view.close()
	expect(view.is_open(), "close() without force refuses during the choice")
	var guard := 0
	while view.model.choosing_attribute() and guard < 100:
		guard += 1
		_press(view, &"ui_accept")
		expect(int(view.model.header["level"]) == level + guard, "accept spends a level")
		if view.model.choosing_attribute():
			_press(view, &"ui_cancel")
			expect(view.is_open(), "each banked level asks in turn")
	expect(guard >= 1, "a level was chosen")
	expect(not view.model.choosing_attribute(), "all banked levels are spent")
	_press(view, &"ui_cancel")
	expect(not view.is_open(), "with no level banked cancel closes")
	# Forced close works even while choosing.
	session.papyrus.set_player_xp(100000.0)
	view.open()
	expect(view.model.choosing_attribute(), "another banked level asks again")
	view.close(true)
	expect(not view.is_open(), "close(true) closes even while choosing")
	print("smoke_skills_view: failures=%d" % failures)
	quit(failures)
