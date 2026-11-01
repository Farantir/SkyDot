# SPDX-License-Identifier: GPL-3.0-or-later
#
# The rules of levelling for the player (ARCHITECTURE.md 8.2, ADR 0009; UESP Skyrim:Leveling):
# skill use turns into skill progress and, at the skill's threshold, into a skill increase that
# gives character XP; enough XP makes a level-up ready, which raises one of Health, Magicka and
# Stamina and gives a perk point; perks are taken with points through the skill's perk tree;
# a skill at 100 can be made legendary. A RefCounted over ProgressionData: it returns plain
# values and Dictionaries, emits nothing and keeps no state (the state is SkydotPapyrus's
# progression, saved in PRGS). A mod extends it and overrides a rule method (`skill_use_mult`,
# `skill_threshold`, `xp_for_skill_level`, `level_up_threshold`, `perk_blocker`,
# `on_perk_taken`), or replaces it whole through GameSession.progression_script.
class_name Progression
extends RefCounted

const HEALTH := 24
const MAGICKA := 25
const STAMINA := 26
const CARRY_WEIGHT := 32
const MAX_SKILL := 100.0
const LEGENDARY_BASE := 15.0

var _data: ProgressionData


func _init(data: ProgressionData = null) -> void:
	_data = data


func is_skill(av: int) -> bool:
	return av >= ProgressionData.FIRST_SKILL and av <= ProgressionData.LAST_SKILL


func skill_name(av: int) -> String:
	return String(_data.skill_info(av).get("name", ""))


## The skill's multiplier on what use gives. P6f feeds the Mod Skill Use perk entry point here.
func skill_use_mult(_av: int) -> float:
	return 1.0


## The skill progress needed to go from base level `level` to `level + 1`:
## improve_mult * level ^ fSkillUseCurve + improve_offset.
func skill_threshold(av: int, level: float) -> float:
	var info := _data.skill_info(av)
	var curve := _data.gmst_float("fSkillUseCurve", 1.95)
	return float(info.get("improve_mult", 1.0)) * pow(level, curve) + float(info.get("improve_offset", 0.0))


## The character XP a skill increase to `new_level` gives: the new level * fXPPerSkillRank
## (not in Skyrim.esm; 1.0).
func xp_for_skill_level(new_level: int) -> float:
	return new_level * _data.gmst_float("fXPPerSkillRank", 1.0)


## The XP the player needs to go from `level` to `level + 1`.
func level_up_threshold(level: int) -> float:
	return _data.gmst_float("fXPLevelUpBase", 75.0) + _data.gmst_float("fXPLevelUpMult", 25.0) * level


func level_up_ready() -> bool:
	var p := _data.progress()
	return float(p["xp"]) >= level_up_threshold(int(p["level"]))


## Use of skill `av` worth `amount` (the action's own skill-use value): progress =
## amount * use_mult * skill_use_mult + use_offset. Returns {gained, skill_increases:
## [{av, level}], level_up_ready (true when this call made a level-up ready)}.
func use_skill(av: int, amount: float) -> Dictionary:
	var out := {"gained": 0.0, "skill_increases": [], "level_up_ready": false}
	if not is_skill(av) or _data.skill_info(av).is_empty() or _data.base_av(av) >= MAX_SKILL:
		return out
	var info := _data.skill_info(av)
	var gained := amount * float(info.get("use_mult", 1.0)) * skill_use_mult(av) + float(info.get("use_offset", 0.0))
	out["gained"] = gained
	var was_ready := level_up_ready()
	var progress := float(_data.progress()["skill_xp"][av - ProgressionData.FIRST_SKILL]) + gained
	var level := int(_data.base_av(av))
	var xp := 0.0
	while level < MAX_SKILL:
		var need := skill_threshold(av, level)
		if progress < need:
			break
		progress -= need
		level += 1
		xp += xp_for_skill_level(level)
		out["skill_increases"].append({"av": av, "level": level})
	if level >= MAX_SKILL:
		progress = 0.0
	if level != int(_data.base_av(av)):
		_data.set_base_av(av, level)
	_data.set_skill_xp(av, progress)
	_add_xp(xp)
	out["level_up_ready"] = not was_ready and level_up_ready()
	return out


## Raise skill `av` by `count` levels at once (books, trainers, IncrementSkill), each giving
## character XP; the progress fraction toward the next level is kept (an estimate: the game's
## own handling is unknown). Returns like use_skill (without `gained`).
func increment_skill(av: int, count: int) -> Dictionary:
	var out := {"skill_increases": [], "level_up_ready": false}
	if not is_skill(av) or _data.skill_info(av).is_empty() or count <= 0:
		return out
	var was_ready := level_up_ready()
	var level := int(_data.base_av(av))
	var start := level
	var xp := 0.0
	var fraction := 0.0
	if level < MAX_SKILL:
		var need := skill_threshold(av, level)
		if need > 0.0:
			fraction = float(_data.progress()["skill_xp"][av - ProgressionData.FIRST_SKILL]) / need
	while count > 0 and level < MAX_SKILL:
		level += 1
		count -= 1
		xp += xp_for_skill_level(level)
		out["skill_increases"].append({"av": av, "level": level})
	if level != start:
		_data.set_base_av(av, level)
		_data.set_skill_xp(av, 0.0 if level >= MAX_SKILL else fraction * skill_threshold(av, level))
	_add_xp(xp)
	out["level_up_ready"] = not was_ready and level_up_ready()
	return out


## Spend the banked level on Health 24, Magicka 25 or Stamina 26: XP -= threshold, level + 1,
## that attribute + iAVDhmsLevelUp (Stamina also CarryWeight + fLevelUpCarryWeightMod), one
## perk point, the three attributes restored. Callable again while still ready. Returns
## {ok, level, reason}.
func level_up(attribute_av: int) -> Dictionary:
	if attribute_av != HEALTH and attribute_av != MAGICKA and attribute_av != STAMINA:
		return {"ok": false, "reason": "attribute"}
	var p := _data.progress()
	var level := int(p["level"])
	var need := level_up_threshold(level)
	if float(p["xp"]) < need:
		return {"ok": false, "reason": "xp"}
	_data.set_xp(float(p["xp"]) - need)
	_data.set_level(level + 1)
	_data.mod_base_av(attribute_av, _data.gmst_int("iAVDhmsLevelUp", 10))
	if attribute_av == STAMINA:
		_data.mod_base_av(CARRY_WEIGHT, _data.gmst_float("fLevelUpCarryWeightMod", 5.0))
	_data.set_perk_points(int(p["perk_points"]) + 1)
	for a in [HEALTH, MAGICKA, STAMINA]:
		_data.restore_av(a)
	return {"ok": true, "level": level + 1, "reason": ""}


## What stands between the player and `perk` (the id of any rank): "" if nothing, else
## "unknown", "has", "points", "playable", "tree" (in no skill's tree), "rank" (the rank before
## is missing), "link" (no node leading to it is taken), "conditions". A mod adds a condition by
## overriding this.
func perk_blocker(perk: int) -> String:
	var info := _data.perk(perk)
	if info.is_empty():
		return "unknown"
	if _data.perk_rank(perk) > 0:
		return "has"
	if int(_data.progress()["perk_points"]) <= 0:
		return "points"
	if not bool(info.get("playable", false)):
		return "playable"
	var placed := _data.perk_node(perk)
	if placed.is_empty():
		return "tree"
	var node: Dictionary = placed["node"]
	if first_missing(int(node["perk"])) != perk:
		return "rank"
	if not _linked(placed["tree"], node):
		return "link"
	if not _data.perk_conditions_met(perk):
		return "conditions"
	return ""


## {perk, name, blocker (see perk_blocker), can, av (the skill of its tree, 0 if none)}.
func perk_status(perk: int) -> Dictionary:
	var blocker := perk_blocker(perk)
	var placed := _data.perk_node(perk)
	return {"perk": perk, "name": String(_data.perk(perk).get("name", "")), "blocker": blocker,
			"can": blocker == "", "av": int(placed.get("av", 0))}


func can_take_perk(perk: int) -> bool:
	return perk_blocker(perk) == ""


## The first perk of the rank chain starting at `perk` that the player lacks (0 if all are taken).
func first_missing(perk: int) -> int:
	var p := perk
	var guard := 0
	while p != 0 and _data.perk_rank(p) > 0 and guard < 16:
		p = int(_data.perk(p).get("next_perk", 0))
		guard += 1
	return p


## Take `perk`: one perk point, the perk added. Returns {ok, perk, reason (the blocker)}.
func take_perk(perk: int) -> Dictionary:
	var blocker := perk_blocker(perk)
	if blocker != "":
		return {"ok": false, "perk": perk, "reason": blocker}
	_data.set_perk_points(int(_data.progress()["perk_points"]) - 1)
	_data.add_perk(perk)
	on_perk_taken(perk)
	return {"ok": true, "perk": perk, "reason": ""}


## Hook: a perk was taken (P6d applies its ability and quest effects here).
func on_perk_taken(_perk: int) -> void:
	pass


## Make skill `av` legendary: needs the base at 100; the base goes to 15, every perk of its tree
## (all ranks) is removed and refunded as perk points, the legendary count + 1, the progress 0.
## Returns {ok, refunded, legendary, reason}.
func make_legendary(av: int) -> Dictionary:
	if not is_skill(av):
		return {"ok": false, "reason": "skill"}
	if _data.base_av(av) < MAX_SKILL:
		return {"ok": false, "reason": "level"}
	var refunded := 0
	for node: Dictionary in _data.perk_tree(av):
		var p := int(node.get("perk", 0))
		var guard := 0
		while p != 0 and guard < 16:
			if _data.perk_rank(p) > 0:
				_data.remove_perk(p)
				refunded += 1
			p = int(_data.perk(p).get("next_perk", 0))
			guard += 1
	var progress := _data.progress()
	var legendary := int(progress["legendary"][av - ProgressionData.FIRST_SKILL]) + 1
	_data.set_base_av(av, LEGENDARY_BASE)
	_data.set_skill_xp(av, 0.0)
	_data.set_legendary(av, legendary)
	_data.set_perk_points(int(progress["perk_points"]) + refunded)
	return {"ok": true, "refunded": refunded, "legendary": legendary, "reason": ""}


func _add_xp(xp: float) -> void:
	if xp > 0.0:
		_data.set_xp(float(_data.progress()["xp"]) + xp)


## Whether a node of `tree` leading to `node` is taken (its first perk owned) or is the root.
func _linked(tree: Array, node: Dictionary) -> bool:
	var index := int(node["index"])
	for other: Dictionary in tree:
		if not (index in other.get("links", [])):
			continue
		var p := int(other.get("perk", 0))
		if p == 0 or _data.perk_rank(p) > 0:
			return true
	return false
