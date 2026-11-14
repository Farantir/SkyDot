# SPDX-License-Identifier: GPL-3.0-or-later
#
# The magic menu: the shared item list (ItemListView) with the player's magic in the Favorites, the five schools'
# (Alteration ... Restoration), Other, Scrolls (carried), Shouts, Powers and Active Effects tabs. A spell row
# has its Magicka cost ("/s" for concentration) and L, R or LR for the hand(s) it is in; one the player cannot
# pay now is dimmed but listed. Up and down choose an entry, left and right a tab, accept (Enter, E) puts it in
# the right hand, `menu_equip_left` (Q, the gamepad's Y) in the left; a power goes to the power slot whichever
# is used; `menu_favorite` (F) marks it a favorite. Cancel, P or the `magic` action closes. Shouts: a row is a
# shout the player has, marked E in the voice slot; accept or `menu_equip_left` puts it there
# (GameSession.equip_shout), and under the list the selected shout shows its three words with their
# translations, each unlocked, known (locked) or unknown (shown as ?????). `menu_drop` (R, gamepad X) unlocks
# the next known, locked word of the shout with a dragon soul after the question sConfirmSpendSoul (accept yes,
# cancel no); with no soul it says sFailSpendSoul. The header shows Magicka and the dragon souls. Abilities are
# not spells and show only as Active Effects. A Control that fills its host (the viewer's CanvasLayer, a VR
# shell's panel); it takes its input from the InputMap, and knows the game only through GameSession (via
# ItemListSources). While it is open it emits `engaged(true)` so the shell keeps the player still.
class_name MagicView
extends Control

signal engaged(open: bool)

const REFRESH_SECONDS := 0.5
const NOTICE_SECONDS := 4.0

## The magic tabs by name (also the ids of the rows' `tab` capitalised).
const SHOUTS := "Shouts"
const POWERS := "Powers"
const OTHER := "Other"
const SCROLLS := "Scrolls"
const EFFECTS := "Active Effects"

var model: ItemListModel
var _session: GameSession
var _open := false
var _list: ItemListView
var _since_refresh := 0.0
var _confirm := {}  # the word to unlock once the player says yes: {word, name}
var _notice := ""  # the last refusal, shown in the menu
var _notice_left := 0.0
## Whether the menu listens at all (false in runs that drive themselves).
var enabled: Callable = func() -> bool: return true
## What the key labels say, by action (the viewer's gamepad or keyboard).
var key_label: Callable = func(_action: StringName) -> String: return ""


func _init(session: GameSession) -> void:
	_session = session
	model = ItemListModel.new("magic", session.favorites)
	_list = ItemListView.new(model)
	_list.title = "Magic"
	_list.visible = false
	_list.key_label = func(action: StringName) -> String: return key_label.call(action)
	_list.action_requested.connect(_on_action)
	model.changed.connect(_update_detail)
	add_child(_list)
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
	_list.visible = true
	engaged.emit(true)
	_refresh()


func close() -> void:
	if not _open:
		return
	_open = false
	_confirm = {}
	_list.visible = false
	_list.stop_search()
	_update_detail()
	engaged.emit(false)


## Names of the tabs shown, in order.
func category_names() -> Array[String]:
	return model.tab_names()


## The rows of the tab called `name`, [] if there is none.
func rows_of(name: String) -> Array:
	return model.rows_of_tab(name)


## Show the tab called `name`.
func select_category(name: String) -> void:
	model.select_tab(name)


## The row under the cursor, {} if the list is empty.
func current() -> Dictionary:
	return model.current()


## Rebuild the lists: the spells by school, the scrolls carried, the effects acting on the player.
func _refresh() -> void:
	if not _open:
		return
	model.set_rows(ItemListSources.magic(_session))
	_list.status = ["Magicka %d / %d" % [roundi(_session.actor_value(SkydotPapyrus.PLAYER_REF, "Magicka")),
		roundi(_session.actor_value_max(SkydotPapyrus.PLAYER_REF, "Magicka"))], "Dragon Souls: %d" % _session.dragon_souls()]


func _process(delta: float) -> void:
	if not _open:
		return
	if _notice_left > 0.0:
		_notice_left = maxf(_notice_left - delta, 0.0)
		if _notice_left == 0.0:
			_update_detail()
	_since_refresh += delta
	if _since_refresh >= REFRESH_SECONDS:
		_since_refresh = 0.0
		_refresh()


func _input(event: InputEvent) -> void:
	if not _open or not enabled.call():
		return
	var handled := true
	if not _confirm.is_empty():
		# A question is open: yes or no, nothing else.
		if event.is_action_pressed(&"ui_accept"):
			answer_confirmation(true)
		elif event.is_action_pressed(&"ui_cancel"):
			answer_confirmation(false)
		if event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton:
			get_viewport().set_input_as_handled()
		return
	if _list.is_searching():
		_list.handle_input(event)
	elif event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"magic"):
		close()
	elif not _list.handle_input(event):
		handled = event is InputEventKey or event is InputEventJoypadButton or event is InputEventMouseButton
	if handled:
		get_viewport().set_input_as_handled()


func _on_action(action: StringName, row: Dictionary) -> void:
	match action:
		&"equip_right":
			equip_current(Magic.RIGHT)
		&"equip_left":
			equip_current(Magic.LEFT)
		&"unlock":
			unlock_current()
		&"favorite":
			_session.toggle_favorite(int(row["form"]))


## Put the entry under the cursor in `hand` (Magic.LEFT, RIGHT); a power goes to the power slot.
## Active effects cannot be equipped.
func equip_current(hand: int) -> bool:
	var row := current()
	if row.is_empty() or row["kind"] == "effect":
		return false
	if row["kind"] == "shout":
		var equipped := _session.equip_shout(int(row["form"]))
		_refresh()
		return equipped
	var done := _session.equip_spell(int(row["form"]), Magic.POWER if row["kind"] == "power" else hand)
	_refresh()
	return done


## The unlock action on a shout: the question "Spend Soul?" for its next known, locked word, or the
## refusal (sFailSpendSoul with no dragon soul). False if nothing was asked.
func unlock_current() -> bool:
	var row := current()
	if row.is_empty() or row["kind"] != "shout":
		return false
	for word: Dictionary in row["words"]:
		if word["known"] and not word["unlocked"]:
			if _session.dragon_souls() < 1:
				_say(_session.world.get_gmst_string("sFailSpendSoul", "You do not have a dragon soul to spend."))
				return false
			_confirm = {"word": int(word["word"]), "name": word["name"]}
			_update_detail()
			return true
	_say("No known word to unlock")
	return false


## The question's answer: yes spends the soul and unlocks the word. Returns whether it was unlocked.
func answer_confirmation(yes: bool) -> bool:
	if _confirm.is_empty():
		return false
	var word := int(_confirm["word"])
	_confirm = {}
	var done := false
	if yes:
		var result := _session.unlock_word(word)
		done = result["ok"]
		if not done:
			_say(String(result["reason"]))
	_refresh()
	return done


## The question being asked, "" if none.
func confirmation() -> String:
	return "" if _confirm.is_empty() else "%s %s" % [_session.world.get_gmst_string("sConfirmSpendSoul", "Spend Soul?"), _confirm["name"]]


## The last refusal the menu shows ("" after a few seconds).
func notice() -> String:
	return _notice if _notice_left > 0.0 else ""


func _say(text: String) -> void:
	_notice = text
	_notice_left = NOTICE_SECONDS
	_update_detail()


## The word's line in a shout's detail: "Fus  Force", "Ro  Balance (locked)", "?????".
static func word_text(word: Dictionary) -> String:
	if not word["known"]:
		return "?????"
	var text := "%s  %s" % [word["name"], word["translation"]]
	return text if word["unlocked"] else text + " (locked)"


## What sits under the list: the selected shout's description and words, or the question or refusal.
func _update_detail() -> void:
	_list.detail_lines.clear()
	var row := current() if _open else {}
	if not row.is_empty() and row["kind"] == "shout":
		var text: String = row["description"]
		_list.detail_lines.append({"text": text.left(60), "tone": "dim"})
		for word: Dictionary in row["words"]:
			_list.detail_lines.append({"text": word_text(word), "tone": "equipped" if word["unlocked"] else ("normal" if word["known"] else "dim")})
	_list.notice = ""
	if not _confirm.is_empty():
		_list.notice = "%s   %s yes   %s no" % [confirmation(), _key(&"ui_accept", "Enter"), _key(&"ui_cancel", "Esc")]
	elif notice() != "":
		_list.notice = notice()
	_list.queue_redraw()


func _key(action: StringName, fallback: String) -> String:
	var label: String = key_label.call(action)
	return label if label != "" else fallback


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
