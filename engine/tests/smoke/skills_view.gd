# SPDX-License-Identifier: GPL-3.0-or-later
#
# The skills screen on the real SE pack (SKYDOT_SSE_PACK): the model's ring and header, the One-Handed tree
# (11 stars, Armsman available with a perk point), taking Armsman through the view's keys (accept asks, accept
# confirms) and the status becoming taken, and the level-up choice when a level is banked. Headless; skips
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
	expect(m.skills.size() == 18, "18 skills")
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
	expect(m.current_node().get("name", "") == "Armsman", "the cursor starts on it")
	_press(view, &"ui_accept")
	expect(m.pending.get("kind", "") == "perk", "accept asks")
	_press(view, &"ui_accept")
	armsman = m.node(armsman["index"])
	expect(armsman["rank"] == 1, "Armsman rank 1 is taken: %s" % [armsman["rank"]])
	expect(m.header["perk_points"] == 0, "the point is spent")
	expect(armsman["status"] == "locked" and armsman["blocker_text"] != "", "the next rank is locked: %s" % [armsman["blocker_text"]])
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
