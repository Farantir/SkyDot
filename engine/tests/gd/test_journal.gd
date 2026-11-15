# SPDX-License-Identifier: GPL-3.0-or-later
#
# JournalLog (game/journal.gd) over fake quest data: the description is the newest log entry, MISCELLANEOUS gathers
# the misc quests' objectives, completed quests follow a divider, alias names are put into texts, Toggle Active, the
# stats table and counts (Game.IncrementStat, completions by quest type), and the JRNL round trip.
extends "gd_test.gd"

const CLAW := 0x39645  # a side quest (type 8)
const MAIN := 0x3372B  # a main quest (type 1)
const FAVOR := 0xD45C0  # a misc quest (type 6)


class FakeData extends JournalLog.Data:
	var info := {
		CLAW: {"name": "The Golden Claw", "type": 8, "aliases": [{"id": 0, "name": "Lucan"}],
			"objectives": [{"index": 10, "text": "Retrieve the Golden Claw"}, {"index": 40, "text": "Bring the claw to <Alias=Lucan>"}]},
		MAIN: {"name": "Unbound", "type": 1, "aliases": [], "objectives": [{"index": 5, "text": "Escape Helgen"}]},
		FAVOR: {"name": "Delivery", "type": 6, "aliases": [{"id": 3, "name": "Target"}],
			"objectives": [{"index": 10, "text": "Deliver it to <Alias.ShortName=Target>"}]},
	}
	var states := {}
	var days := 3

	func quest_info(quest: int) -> Dictionary:
		return info.get(quest, {})

	func quest_state(quest: int) -> Dictionary:
		return states.get(quest, {})

	func running_quests() -> Array:
		return states.keys().filter(func(q: int) -> bool: return states[q].get("running", false))

	func ref_name(ref: int) -> String:
		return {0x1A68D: "Lucan Valerius", 0x1A67D: "Faendal"}.get(ref, "")

	func days_passed() -> int:
		return days


var data: FakeData
var journal: JournalLog


func before_each() -> void:
	data = FakeData.new()
	journal = JournalLog.new(data)


func _show(quest: int, index: int, running := true) -> void:
	var state: Dictionary = data.states.get(quest, {"running": running, "completed": false, "objectives": {}, "aliases": {}})
	state["objectives"][index] = {"displayed": true, "completed": false, "failed": false}
	data.states[quest] = state
	journal.on_objective(quest, index, "displayed", "")


func test_description_is_the_newest_entry() -> void:
	journal.on_quest_stage(CLAW, 10, "Lucan is offering a reward.")
	journal.on_quest_stage(CLAW, 12, "I've overheard a group of bandits.")
	journal.on_quest_stage(CLAW, 13, "")  # a stage without a log entry writes nothing
	expect_eq(journal.description(CLAW), "I've overheard a group of bandits.", "newest entry")
	expect_eq((journal.quests[CLAW]["log"] as Array).size(), 2, "two entries")


func test_title_list_order_misc_and_divider() -> void:
	journal.on_quest_stage(CLAW, 10, "Lucan is offering a reward.")
	_show(CLAW, 10)
	_show(FAVOR, 10)
	journal.on_quest_stage(MAIN, 5, "I escaped.")
	data.states[MAIN] = {"running": false, "completed": true, "objectives": {}, "aliases": {}}
	var titles := journal.title_list()
	var texts := titles.map(func(t: Dictionary) -> String: return t["text"])
	expect_eq(texts, ["THE GOLDEN CLAW", "MISCELLANEOUS", "", "UNBOUND"], "active, misc, divider, completed")
	expect(titles[2]["divider"], "divider row")
	expect(titles[3]["completed"], "completed row")
	expect(not titles[1]["active"], "MISCELLANEOUS starts inactive")
	expect(titles[0]["active"], "a new quest is active")


func test_objectives_and_aliases() -> void:
	_show(CLAW, 10)
	data.states[CLAW]["aliases"] = {0: 0x1A68D}
	_show(CLAW, 40)
	data.states[CLAW]["objectives"][10]["completed"] = true
	var rows := journal.objectives(CLAW)
	expect_eq(rows.map(func(o: Dictionary) -> String: return o["text"]),
		["Bring the claw to Lucan Valerius", "Retrieve the Golden Claw"], "newest first, alias named")
	expect_eq(rows.map(func(o: Dictionary) -> String: return o["state"]), ["Normal", "Completed"], "states")
	_show(FAVOR, 10)
	data.states[FAVOR]["aliases"] = {3: 0x1A67D}
	expect_eq(journal.objectives(0).map(func(o: Dictionary) -> String: return o["text"]), ["Deliver it to Faendal"],
		"MISCELLANEOUS lists the misc quests' objectives")
	expect_eq(journal.substitute(CLAW, "Ask <Alias=Nobody>"), "Ask <Alias=Nobody>", "unknown alias kept")


func test_toggle_active() -> void:
	journal.on_quest_stage(CLAW, 10, "x")
	expect(not journal.toggle_active(CLAW), "off")
	expect(journal.toggle_active(CLAW), "on again")
	expect(journal.toggle_active(0), "MISCELLANEOUS on")


func test_stats() -> void:
	expect_eq(journal.stat_rows(0).size(), 22, "GENERAL lists the 22 shown stats")
	expect_eq(journal.stat_rows(0)[0]["text"], "Locations Discovered", "first general stat")
	expect_eq(journal.stat_rows(4)[0]["text"], "Soul Gems Used", "crafting starts with Soul Gems Used (the exe's category)")
	expect_eq(journal.value("Days Passed"), 3, "days from the clock")
	expect_eq(journal.value("Favorite Weapon"), "None", "a named stat")
	journal.count("Persuasions", 2)
	expect_eq(journal.value("Persuasions"), 2, "IncrementStat")
	journal.at_least("Most Gold Carried", 140)
	journal.at_least("Most Gold Carried", 90)
	expect_eq(journal.value("Most Gold Carried"), 140, "most gold keeps the highest")
	journal.on_quest_completed(MAIN)
	expect_eq(journal.value("Quests Completed"), 1, "quests completed")
	expect_eq(journal.value("Main Quests Completed"), 1, "main quests completed")
	_show(FAVOR, 10)
	journal.on_objective(FAVOR, 10, "completed", "")
	expect_eq(journal.value("Misc Objectives Completed"), 1, "misc objective completed")


func test_save_round_trip() -> void:
	journal.on_quest_stage(CLAW, 10, "Lucan is offering a reward.")
	journal.toggle_active(CLAW)
	journal.count("Bribes")
	journal.play_seconds = 125.0
	var other := JournalLog.new(data)
	other.adopt(JournalLog.decode(journal.save_bytes()))
	expect_eq(other.description(CLAW), "Lucan is offering a reward.", "log kept")
	expect(not other.quests[CLAW]["active"], "active flag kept")
	expect_eq(other.value("Bribes"), 1, "stats kept")
	expect_eq(other.play_seconds, 125.0, "play time kept")
	expect_eq(JournalLog.decode(var_to_bytes(42)), {}, "not a JRNL section")
