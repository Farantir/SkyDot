# SPDX-License-Identifier: GPL-3.0-or-later
#
# The magic menu (ui/magic_view.gd) on the real SE pack (SKYDOT_SSE_PACK), headless, 7a-3: the
# player's start spells Flames and Healing under Destruction and Restoration with their costs,
# the race power under Powers, a Scroll of Firebolt under Scrolls, Fortify Health under Active
# Effects after a potion; accept puts a spell in the right hand, `menu_equip_left` in the left (the
# hand marks follow), a power and a scroll can be equipped; the view's actions are fed as
# InputEventActions. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const HEALING := 0x12FCC
const FLAMES := 0x12FCD
const SCROLL_FIREBOLT := 0x96598
const POTION_FORTIFY := 0x3EAF4

var failures := 0
var _session: GameSession
var _view: MagicView


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false
	var loaded_position := Vector3.ZERO
	var loaded_yaw := 0.0
	var loaded_pitch := 0.0

	func place_at(_at: Vector3, new_yaw: float, new_pitch: float) -> void:
		yaw = new_yaw
		pitch = new_pitch

	func set_view_distance(_metres: float) -> void:
		pass

	func save_section() -> PackedByteArray:
		return PackedByteArray()

	func load_section(_bytes: PackedByteArray) -> bool:
		return true


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_magic_menu: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _press(action: StringName) -> void:
	var event := InputEventAction.new()
	event.action = action
	event.pressed = true
	_view._input(event)


func _row(category: String, form: int) -> Dictionary:
	for row: Dictionary in _view.rows_of(category):
		if int(row["form"]) == form:
			return row
	return {}


func _select(category: String, form: int) -> void:
	_view.select_category(category)
	for i in _view.rows_of(category).size():
		if int(_view.current().get("form", 0)) == form:
			return
		_press(&"ui_down")
	expect(false, "0x%X is not listed under %s" % [form, category])


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	config.wander = false
	var camera := Camera3D.new()
	root.add_child(camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, camera, FakePlayer.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	var papyrus := _session.papyrus
	for spell in [FLAMES, HEALING]:
		if not papyrus.has_spell(PLAYER, spell):
			papyrus.add_spell(PLAYER, spell)
	_session.give([Vector2i(SCROLL_FIREBOLT, 2)])
	_view = MagicView.new(_session)
	root.add_child(_view)
	var engaged: Array[bool] = []
	_view.engaged.connect(func(open: bool) -> void: engaged.append(open))
	expect(not _view.is_open(), "the view starts closed")
	_view.open()
	expect(_view.is_open() and engaged == [true], "it opens and says so")
	print("categories: ", _view.category_names())
	for category in ["Powers", "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Scrolls", "Active Effects"]:
		expect(category in _view.category_names(), "category %s is shown" % category)

	# ---- the rows
	var flames := _row("Destruction", FLAMES)
	var healing := _row("Restoration", HEALING)
	print("flames: ", flames, " healing: ", healing)
	expect(not flames.is_empty() and flames["name"] == "Flames", "Destruction has Flames")
	expect(not healing.is_empty() and healing["name"] == "Healing", "Restoration has Healing")
	expect(int(flames.get("cost", 0)) == roundi(papyrus.spell_cost(PLAYER, FLAMES)) and int(flames["cost"]) > 0, "Flames has its cost: %s" % [flames])
	expect(flames["concentration"] == true and MagicView.cost_text(flames) == "%d/s" % flames["cost"], "Flames is concentration: cost per second")
	expect(_row("Alteration", FLAMES).is_empty() and _row("Destruction", HEALING).is_empty(), "spells sit only under their school")
	var powers := _view.rows_of("Powers")
	print("powers: ", powers)
	expect(not powers.is_empty() and powers[0]["kind"] == "power", "the race power is under Powers")
	for row: Dictionary in powers:
		expect(papyrus.has_spell(PLAYER, int(row["form"])), "a power listed is one the player has")
	for category in ["Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Other"]:
		for row: Dictionary in _view.rows_of(category):
			expect(row["kind"] == "spell", "%s lists only spells: %s" % [category, row])
	var scroll := _row("Scrolls", SCROLL_FIREBOLT)
	expect(not scroll.is_empty() and scroll["count"] == 2 and scroll["kind"] == "scroll", "Scrolls has the Scroll of Firebolt x2: %s" % [scroll])
	expect(MagicView.row_label(scroll).ends_with("(2)"), "its label has the count")

	# ---- equipping through the view's actions
	_select("Destruction", FLAMES)
	_press(&"menu_equip_left")
	expect(_session.spell_in_hand(Magic.LEFT) == FLAMES and papyrus.equipped_spell(PLAYER, Magic.LEFT) == FLAMES, "Flames is in the left hand")
	expect(MagicView.hands_text(_row("Destruction", FLAMES)) == "L", "and marked L")
	_select("Restoration", HEALING)
	_press(&"ui_accept")
	expect(papyrus.equipped_spell(PLAYER, Magic.RIGHT) == HEALING, "Healing is in the right hand")
	expect(MagicView.hands_text(_row("Restoration", HEALING)) == "R", "and marked R")
	_select("Destruction", FLAMES)
	_press(&"ui_accept")
	expect(MagicView.hands_text(_row("Destruction", FLAMES)) == "LR", "Flames in both hands is marked LR")
	var power_form := int(powers[0]["form"])
	_select("Powers", power_form)
	_press(&"ui_accept")
	expect(papyrus.equipped_spell(PLAYER, Magic.POWER) == power_form, "the power goes to the power slot")
	_select("Scrolls", SCROLL_FIREBOLT)
	_press(&"menu_equip_left")
	expect(papyrus.equipped_spell(PLAYER, Magic.LEFT) == SCROLL_FIREBOLT, "the scroll is in the left hand")
	_press(&"ui_cancel")
	expect(not _view.is_open() and engaged == [true, false], "cancel closes it")

	# ---- active effects after a potion
	_view.open()
	expect(not _view.rows_of("Active Effects").any(func(row: Dictionary) -> bool: return row["name"] == "Fortify Health"), "no Fortify Health yet")
	_session.give([Vector2i(POTION_FORTIFY, 1)])
	expect(_session.use_item(POTION_FORTIFY), "the potion is drunk")
	var effects := _view.rows_of("Active Effects")
	print("effects: ", effects)
	expect(effects.any(func(row: Dictionary) -> bool: return row["name"] == "Fortify Health" and row["magnitude"] > 0.0 and row["remaining"] > 0.0),
		"Active Effects lists Fortify Health with magnitude and time left")
	_view.select_category("Active Effects")
	_press(&"ui_accept")
	expect(not _view.equip_current(Magic.RIGHT), "an active effect cannot be equipped")
	_view.close()

	print("smoke_magic_menu: ", "FAIL" if failures > 0 else "OK")
	quit(1 if failures > 0 else 0)
