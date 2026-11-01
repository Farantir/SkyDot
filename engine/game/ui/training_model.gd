# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the training menu shows (ui/training_view.gd draws it, a restyled UI replaces the drawing; ADR 0010):
# plain strings and flags from GameSession.training_info, no drawing and no input. Built by `of`.
class_name TrainingModel
extends RefCounted

var trainer_name := ""
var skill_name := ""
var level := 0
var max_level := 0
var cost := 0
var gold := 0
var sessions_left := 0
## "" if a session can be bought, else the vanilla refusal text.
var refusal := ""
var can_train := false


## The model of `trainer`'s offer now.
static func of(session: GameSession, trainer: int) -> TrainingModel:
	var info := session.training_info(trainer)
	var model := TrainingModel.new()
	model.trainer_name = session.ref_name(trainer)
	model.skill_name = session.progression.skill_name(int(info["skill"]))
	model.level = int(info["level"])
	model.max_level = int(info["max_level"])
	model.cost = int(info["cost"])
	model.gold = int(info["gold"])
	model.sessions_left = int(info["sessions_left"])
	model.can_train = info["reason"] == ""
	model.refusal = session.training_refusal(String(info["reason"]))
	return model


## The lines of the menu, top to bottom.
func lines() -> Array[String]:
	var out: Array[String] = []
	out.append("%s: %s" % [trainer_name, skill_name] if trainer_name != "" else skill_name)
	out.append("Skill: %d (trains up to %d)" % [level, max_level])
	out.append("Cost: %d gold   Your gold: %d" % [cost, gold])
	out.append("Sessions left at this level: %d" % sessions_left)
	if refusal != "":
		out.append(refusal)
	return out
