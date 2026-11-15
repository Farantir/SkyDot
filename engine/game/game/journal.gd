# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the journal (quest_journal.swf) shows and the game keeps for it: each quest's log entries in the order the
# stages wrote them, the order its objectives were displayed, which quests are active (Toggle Active), the misc
# statistics (General Stats) and the time played. Saved in the JRNL section; a save without it starts empty.
#
# The quests' state itself (running, completed, objectives displayed/completed/failed, alias fills) is the scripts'
# (SkydotPapyrus.get_quest_state); this only adds what the runtime does not keep in order. Data comes through a
# JournalLog.Data, so the rules run on fakes in tests.
#
# Rules from the game:
#   - A quest's description is its newest log entry (game captures: after MS13 stages 10 and 12 the journal shows only
#     stage 12's text; a completed quest shows the completing entry).
#   - Quests of type 6 (Miscellaneous) are not listed by name: their displayed objectives are gathered under
#     "MISCELLANEOUS", the last of the active quests (QuestsPage.onQuestsDataComplete pushes it with formID 0).
#   - Completed quests come after the active ones, below a divider (completedQuestSort).
#   - The title text is upper case (onQuestsDataComplete); "<Alias=Name>", "<Alias.ShortName=Name>" in log and
#     objective texts are the alias's fill's name.
#   - The stats are the exe's misc stat table (STATS: name, category, shown), read from SkyrimSE.exe 1.6.1170 /
#     1.7.104 (the registration calls behind the "Locations Discovered" ... "StalhrimItemsCrafted" strings); hidden
#     ones and category 6 are never listed. Values: Game.IncrementStat and the events the session counts.
class_name JournalLog
extends RefCounted

const VERSION := 1
## QUST DNAM type of a miscellaneous quest: listed under MISCELLANEOUS.
const TYPE_MISC := 6
## QuestsPage.onQuestHighlight: the title's endpieces frame by quest type.
const ENDPIECES := ["Misc", "Main", "MagesGuild", "ThievesGuild", "DarkBrotherhood", "Companion", "Favor", "Daedric",
	"Misc", "CivilWar", "DLC01", "DLC02"]
## The stats page's categories (StatsPage.onLoad), by the exe's category index.
const CATEGORIES := ["GENERAL", "QUEST", "COMBAT", "MAGIC", "CRAFTING", "CRIME"]
## The exe's misc stats in registration order: [name, category, shown]. Category 6 is not a page.
const STATS := [
	["Locations Discovered", 0, true], ["Dungeons Cleared", 0, true], ["Days Passed", 0, true],
	["Hours Slept", 0, true], ["Hours Waiting", 0, true], ["Standing Stones Found", 0, true], ["Gold Found", 0, true],
	["Most Gold Carried", 0, true], ["Chests Looted", 0, true], ["Skill Increases", 0, true],
	["Skill Books Read", 0, true], ["Food Eaten", 0, true], ["Training Sessions", 0, true], ["Books Read", 0, true],
	["Horses Owned", 0, true], ["Houses Owned", 0, true], ["Stores Invested In", 0, true], ["Barters", 0, true],
	["Persuasions", 0, true], ["Bribes", 0, true], ["Intimidations", 0, true], ["Diseases Contracted", 0, true],
	["Days as a Vampire", 0, false], ["Days as a Werewolf", 0, false], ["Necks Bitten", 0, false],
	["Vampirism Cures", 0, false], ["Werewolf Transformations", 0, false], ["Mauls", 0, false],
	["Quests Completed", 1, true], ["Misc Objectives Completed", 1, true], ["Main Quests Completed", 1, true],
	["Side Quests Completed", 1, true], ["The Companions Quests Completed", 1, true],
	["College of Winterhold Quests Completed", 1, true], ["Thieves' Guild Quests Completed", 1, true],
	["The Dark Brotherhood Quests Completed", 1, true], ["Civil War Quests Completed", 1, true],
	["Daedric Quests Completed", 1, true], ["Dawnguard Quests Completed", 1, false],
	["Dragonborn Quests Completed", 1, false], ["Questlines Completed", 1, true],
	["People Killed", 2, true], ["Animals Killed", 2, true], ["Creatures Killed", 2, true], ["Undead Killed", 2, true],
	["Daedra Killed", 2, true], ["Automatons Killed", 2, true], ["Favorite Weapon", 2, true],
	["Critical Strikes", 2, true], ["Sneak Attacks", 2, true], ["Backstabs", 2, true], ["Weapons Disarmed", 2, true],
	["Brawls Won", 2, true], ["Bunnies Slaughtered", 2, true],
	["Spells Learned", 3, true], ["Favorite Spell", 3, true], ["Favorite School", 3, true],
	["Dragon Souls Collected", 3, true], ["Words Of Power Learned", 3, true], ["Words Of Power Unlocked", 3, true],
	["Shouts Learned", 3, true], ["Shouts Unlocked", 3, true], ["Shouts Mastered", 3, true], ["Times Shouted", 3, true],
	["Favorite Shout", 3, true],
	["Soul Gems Used", 4, true], ["Souls Trapped", 4, true], ["Magic Items Made", 4, true],
	["Weapons Improved", 4, true], ["Weapons Made", 4, true], ["Armor Improved", 4, true], ["Armor Made", 4, true],
	["Potions Mixed", 4, true], ["Potions Used", 4, true], ["Poisons Mixed", 4, true], ["Poisons Used", 4, true],
	["Ingredients Harvested", 4, true], ["Ingredients Eaten", 4, true], ["Nirnroots Found", 4, true],
	["Wings Plucked", 4, true],
	["Total Lifetime Bounty", 5, true], ["Largest Bounty", 5, true], ["Locks Picked", 5, true],
	["Pockets Picked", 5, true], ["Items Pickpocketed", 5, true], ["Times Jailed", 5, true], ["Days Jailed", 5, true],
	["Fines Paid", 5, true], ["Jail Escapes", 5, true], ["Items Stolen", 5, true], ["Assaults", 5, true],
	["Murders", 5, true], ["Horses Stolen", 5, true], ["Trespasses", 5, true], ["Eastmarch Bounty", 5, false],
	["Falkreath Bounty", 5, false], ["Haafingar Bounty", 5, false], ["Hjaalmarch Bounty", 5, false],
	["The Pale Bounty", 5, false], ["The Reach Bounty", 5, false], ["The Rift Bounty", 5, false],
	["Tribal Orcs Bounty", 5, false], ["Whiterun Bounty", 5, false], ["Winterhold Bounty", 5, false],
	["NumVampirePerks", 6, true], ["NumWerewolfPerks", 6, true], ["Solstheim Locations Discovered", 6, true],
	["StalhrimItemsCrafted", 6, true],
]
## Stats whose value is a name ("None" until set).
const NAMED_STATS := ["Favorite Weapon", "Favorite Spell", "Favorite School", "Favorite Shout"]
## A completed quest of this QUST type also counts in this stat (besides "Quests Completed"); the stat names say it.
const COMPLETED_BY_TYPE := {1: "Main Quests Completed", 2: "College of Winterhold Quests Completed",
	3: "Thieves' Guild Quests Completed", 4: "The Dark Brotherhood Quests Completed",
	5: "The Companions Quests Completed", 7: "Daedric Quests Completed", 8: "Side Quests Completed",
	9: "Civil War Quests Completed", 10: "Dawnguard Quests Completed", 11: "Dragonborn Quests Completed"}


## Where the journal reads the game: the session's in the game, a fake in tests.
class Data:
	## QUST record as SkydotWorld.get_quest has it (name, type, aliases, objectives).
	func quest_info(_quest: int) -> Dictionary:
		return {}

	## SkydotPapyrus.get_quest_state: running, completed, failed, objectives {index: {displayed, completed, failed}},
	## aliases {alias id: ref}.
	func quest_state(_quest: int) -> Dictionary:
		return {}

	## The quests that run now.
	func running_quests() -> Array:
		return []

	## The name a reference (or a location) shows.
	func ref_name(_ref: int) -> String:
		return ""

	## Whole days since the game began (Days Passed).
	func days_passed() -> int:
		return 0


## quest -> {"seq": order of its first entry, "touched": order of its newest change, "log": [{stage, text}],
## "objectives": {index: order displayed}, "active": bool}
var quests := {}
## Stat name -> int, or a name for NAMED_STATS.
var stats := {}
## Seconds played (the save list's play time).
var play_seconds := 0.0
## MISCELLANEOUS's Toggle Active (ToggleShowMiscObjectives): off in a new game (the game's capture shows no marker).
var misc_active := false
var data: Data
var _seq := 0


func _init(source: Data = null) -> void:
	data = source if source != null else Data.new()


## A stage was set and wrote `text` to the journal (empty: no entry).
func on_quest_stage(quest: int, stage: int, text: String) -> void:
	if text == "":
		return
	var entry := _quest(quest)
	(entry["log"] as Array).append({"stage": stage, "text": text})
	entry["touched"] = _next()


## An objective's state changed ("displayed", "completed", "failed", or "not ...").
func on_objective(quest: int, index: int, state: String, _text: String) -> void:
	var entry := _quest(quest)
	if state == "displayed":
		(entry["objectives"] as Dictionary)[index] = _next()
	entry["touched"] = _next()
	if state == "completed" and int(data.quest_info(quest).get("type", 0)) == TYPE_MISC:
		count("Misc Objectives Completed")


## A quest's log entry completed it: the stats count it.
func on_quest_completed(quest: int) -> void:
	count("Quests Completed")
	var by_type: String = COMPLETED_BY_TYPE.get(int(data.quest_info(quest).get("type", 0)), "")
	if by_type != "":
		count(by_type)


## Game.IncrementStat, and the session's own counts.
func count(stat: String, by := 1) -> void:
	stats[stat] = int(stats.get(stat, 0)) + by


## A stat that keeps its highest value ("Most Gold Carried").
func at_least(stat: String, value: int) -> void:
	if value > int(stats.get(stat, 0)):
		stats[stat] = value


## Game.QueryStat and the stats page: the value of `stat` (an int, or a name for the favorites).
func value(stat: String) -> Variant:
	if stat == "Days Passed":
		return data.days_passed()
	if stat in NAMED_STATS:
		return stats.get(stat, "None")
	return int(stats.get(stat, 0))


## The stats page's list of `category` (0 GENERAL ... 5 CRIME): [{text, value}] of the shown stats, in the exe's order.
func stat_rows(category: int) -> Array:
	var out := []
	for row: Array in STATS:
		if int(row[1]) == category and bool(row[2]):
			out.append({"text": row[0], "value": value(row[0])})
	return out


## The quest list as QuestsPage builds it: active quests, then MISCELLANEOUS (when a misc quest shows an objective),
## then a divider and the completed ones. Entries: {quest (0 for MISCELLANEOUS), text, type, completed, active,
## divider}.
func title_list() -> Array:
	var active := []
	var completed := []
	var misc := false
	for quest: int in _listed_quests():
		var info := data.quest_info(quest)
		var state := data.quest_state(quest)
		var type := int(info.get("type", 0))
		if type == TYPE_MISC:
			if state.get("running", false) and not state.get("completed", false) and not _shown_objectives(quest, state).is_empty():
				misc = true
			continue
		var entry: Dictionary = quests.get(quest, {})
		if (entry.get("log", []) as Array).is_empty() and _shown_objectives(quest, state).is_empty():
			continue
		var row := {"quest": quest, "text": quest_title(quest).to_upper(), "type": type, "divider": false,
			"completed": bool(state.get("completed", false)) or bool(state.get("failed", false)),
			"active": bool(entry.get("active", true)), "order": int(entry.get("seq", 0))}
		(completed if row["completed"] else active).append(row)
	# Newest first within each group.
	var newest := func(a: Dictionary, b: Dictionary) -> bool: return int(a["order"]) > int(b["order"])
	active.sort_custom(newest)
	completed.sort_custom(newest)
	if misc:
		active.append({"quest": 0, "text": "MISCELLANEOUS", "type": TYPE_MISC, "divider": false, "completed": false,
			"active": misc_active, "order": 0})
	if not completed.is_empty() and not active.is_empty():
		active.append({"quest": -1, "text": "", "type": 0, "divider": true, "completed": true, "active": false,
			"order": 0})
	return active + completed


## The quest's name as the journal shows it (FULL, else the editor id).
func quest_title(quest: int) -> String:
	var info := data.quest_info(quest)
	var title: String = info.get("name", "")
	return title if title != "" else String(info.get("editor_id", ""))


## The description: the newest log entry, with its aliases' names put in.
func description(quest: int) -> String:
	var log: Array = (quests.get(quest, {}) as Dictionary).get("log", [])
	return substitute(quest, String(log[-1]["text"])) if not log.is_empty() else ""


## The objectives shown for `quest` (0: MISCELLANEOUS, every running misc quest's): [{quest, index, text, state}],
## state "Completed", "Failed" or "Normal" (ObjectiveScrollingList.SetEntry's frames; the game's captures show a
## displayed objective of an active quest, and of MISCELLANEOUS, at "Normal", the diamond), newest displayed first.
func objectives(quest: int) -> Array:
	var sources := []
	if quest == 0:
		for q: int in _listed_quests():
			if int(data.quest_info(q).get("type", 0)) == TYPE_MISC:
				sources.append(q)
	else:
		sources.append(quest)
	var out := []
	for q: int in sources:
		var state := data.quest_state(q)
		if quest == 0 and (not state.get("running", false) or state.get("completed", false)):
			continue
		var order: Dictionary = (quests.get(q, {}) as Dictionary).get("objectives", {})
		for index: int in _shown_objectives(q, state):
			var bits: Dictionary = (state.get("objectives", {}) as Dictionary).get(index, {})
			if quest == 0 and bits.get("completed", false):
				continue
			var shown := "Normal"
			if bits.get("failed", false):
				shown = "Failed"
			elif bits.get("completed", false):
				shown = "Completed"
			out.append({"quest": q, "index": index, "text": substitute(q, _objective_text(q, index)), "state": shown,
				"order": int(order.get(index, 0))})
	out.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return int(a["order"]) > int(b["order"]))
	return out


## Toggle Active on a quest (0: MISCELLANEOUS); returns the new state.
func toggle_active(quest: int) -> bool:
	if quest == 0:
		misc_active = not misc_active
		return misc_active
	var entry := _quest(quest)
	entry["active"] = not bool(entry.get("active", true))
	return entry["active"]


## "<Alias=Name>" and "<Alias.ShortName=Name>" become the name of the alias's fill; an unfilled alias stays as written.
func substitute(quest: int, text: String) -> String:
	if not text.contains("<"):
		return text
	var info := data.quest_info(quest)
	var fills: Dictionary = data.quest_state(quest).get("aliases", {})
	var re := RegEx.new()
	re.compile("<Alias(?:\\.ShortName)?=([^>]+)>")
	var out := text
	for m in re.search_all(text):
		var wanted := m.get_string(1).to_lower()
		for alias: Dictionary in info.get("aliases", []):
			if String(alias.get("name", "")).to_lower() != wanted:
				continue
			var ref := int(fills.get(int(alias["id"]), 0))
			var name := data.ref_name(ref) if ref != 0 else ""
			if name != "":
				out = out.replace(m.get_string(0), name)
			break
	return out


func save_bytes() -> PackedByteArray:
	return var_to_bytes({"quests": quests, "stats": stats, "play_seconds": play_seconds, "misc_active": misc_active,
		"seq": _seq})


## Take a saved state (decode's result); an empty one (an older save) starts the journal empty.
func adopt(state: Dictionary) -> void:
	quests = state.get("quests", {})
	stats = state.get("stats", {})
	play_seconds = float(state.get("play_seconds", 0.0))
	misc_active = bool(state.get("misc_active", false))
	_seq = int(state.get("seq", 0))


## A JRNL section's bytes as a state for adopt; empty when it is not one.
static func decode(bytes: PackedByteArray) -> Dictionary:
	var state: Variant = bytes_to_var(bytes)
	if typeof(state) != TYPE_DICTIONARY or typeof(state.get("quests")) != TYPE_DICTIONARY:
		return {}
	return state


func _quest(quest: int) -> Dictionary:
	if not quests.has(quest):
		quests[quest] = {"seq": _next(), "touched": 0, "log": [], "objectives": {}, "active": true}
	return quests[quest]


func _next() -> int:
	_seq += 1
	return _seq


## The quests the journal knows of: those with an entry here and those the scripts run (a save from before the JRNL
## section still lists its quests with displayed objectives).
func _listed_quests() -> Array:
	var out := quests.keys()
	for quest: int in data.running_quests():
		if not quests.has(quest):
			out.append(quest)
	return out


## The displayed objectives of `quest`, by index.
func _shown_objectives(_quest_id: int, state: Dictionary) -> Array:
	var out := []
	var bits: Dictionary = state.get("objectives", {})
	for index: int in bits:
		if (bits[index] as Dictionary).get("displayed", false):
			out.append(index)
	return out


func _objective_text(quest: int, index: int) -> String:
	for o: Dictionary in data.quest_info(quest).get("objectives", []):
		if int(o["index"]) == index:
			return o["text"]
	return ""
