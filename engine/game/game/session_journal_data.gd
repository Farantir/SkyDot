# SPDX-License-Identifier: GPL-3.0-or-later
#
# The journal's Data in the game (JournalLog.Data): quests, their state and the names of what their aliases hold,
# from the session.
class_name SessionJournalData
extends JournalLog.Data

var _session: GameSession


func _init(session: GameSession) -> void:
	_session = session


func quest_info(quest: int) -> Dictionary:
	return _session.world.get_quest(quest)


func quest_state(quest: int) -> Dictionary:
	return _session.papyrus.get_quest_state(quest)


func running_quests() -> Array:
	return Array(_session.papyrus.get_running_quests())


func ref_name(ref: int) -> String:
	var name := String(_session.world.get_actor(ref).get("name", ""))
	if name == "":
		var info := _session.world.get_ref_info(_session.world.get_ref_cell(ref), ref)
		name = String(_session.world.get_base(int(info.get("base", 0))).get("name", "")) if info.has("base") else ""
	return name


func days_passed() -> int:
	return int(floor(_session.clock.get_game_days())) if _session.clock != null else 0
