# SPDX-License-Identifier: GPL-3.0-or-later
#
# Training with a trainer, the Training menu's rules on top of Progression's (ui/training_view.gd draws the
# menu): what the trainer offers, the vanilla refusal texts, and buying a session, which counts Training Sessions
# and says the raised skill as any skill increase. GameSession.training_opened opens the menu.
class_name SessionTraining
extends RefCounted

var _session: GameSession


func _init(session: GameSession) -> void:
	_session = session


## What `trainer` offers now: Progression.training_info.
func info(trainer: int) -> Dictionary:
	return _session.progression.training_info(trainer)


## The refusal text of an `info` reason ("" for none), the vanilla texts.
func refusal(reason: String) -> String:
	var world := _session.world
	match reason:
		"sessions":
			return world.get_gmst_string("sCanNotTrainAnymore", "You must level up to train more.")
		"level":
			return world.get_gmst_string("sCanNotTrainHigher", "This NPC cannot help you train any higher.")
		"gold":
			return world.get_gmst_string("sInsufficientGoldToTrain", "You have insufficient gold to train.")
	return ""


## Buy one training session from `trainer` (Progression.train); a refusal says its text, a raised skill its usual
## "<skill> Increased to <n>". Returns Progression.train's result.
func train(trainer: int) -> Dictionary:
	var result := _session.progression.train(trainer)
	if result["ok"]:
		_session.journal.count("Training Sessions")
		_session.announce_progress(result)
		_session.items_changed.emit()
	else:
		var text := refusal(String(result["reason"]))
		if text != "":
			_session.message.emit(text, false)
	return result
