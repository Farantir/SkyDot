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
const EP_MOD_SKILL_USE := 22
## The crafting skills (actor value indexes) and the skill use of recharging by soul size 1 to 5 (UESP Skyrim:Leveling).
const SMITHING := 10
const ALCHEMY := 16
const ENCHANTING := 23
const RECHARGE_USE: Array[float] = [0.05, 0.1, 0.2, 0.4, 0.6]
## The combat and magic skills (actor value indexes) and what their uses are worth (UESP Skyrim:Leveling).
const ONE_HANDED := 6
const TWO_HANDED := 7
const ARCHERY := 8
const BLOCK := 9
const HEAVY_ARMOR := 11
const LIGHT_ARMOR := 12
const SNEAK := 15
const ALTERATION := 18
const CONJURATION := 19
const DESTRUCTION := 20
const ILLUSION := 21
const RESTORATION := 22
const PLAYER_REF := 0x14
## BOOK flags (the DATA flags): teaches a skill, teaches a spell.
const BOOK_TEACHES_SKILL := 0x1
const BOOK_TEACHES_SPELL := 0x4
const EP_ADJUST_BOOK_SKILL_POINTS := 5
## Training sessions per character level (UESP Skyrim:Trainers: "up to five times per level").
const TRAININGS_PER_LEVEL := 5
## What one session costs besides the skill part (UESP: n(10b + 5n + 45), n(30b + 15n + 35), n(50b + 25n + 25) at n = 1).
const TRAINING_BASE_COST := 50
const EXPERT_FROM := 50
const MASTER_FROM := 75
const SNEAK_ATTACK_MELEE_USE := 30.0
const SNEAK_ATTACK_RANGED_USE := 2.5
## Weapon type (WEAP animation type) -> the skill its hits train; fists (0) and staffs (8) train none.
const WEAPON_SKILL := {1: ONE_HANDED, 2: ONE_HANDED, 3: ONE_HANDED, 4: ONE_HANDED, 5: TWO_HANDED, 6: TWO_HANDED,
	7: ARCHERY, 9: ARCHERY}
## SPIT type of a plain spell (powers, abilities and the like train nothing), the healing actor value.
const SPELL_TYPE_SPELL := 0
const TICK_SECONDS := 0.1
const MAX_SKILL := 100.0
const LEGENDARY_BASE := 15.0

var _data: ProgressionData


func _init(data: ProgressionData = null) -> void:
	_data = data


func is_skill(av: int) -> bool:
	return av >= ProgressionData.FIRST_SKILL and av <= ProgressionData.LAST_SKILL


func skill_name(av: int) -> String:
	return String(_data.skill_info(av).get("name", ""))


## The skill's multiplier on what use gives: Mod Skill Use 22 [player] with the advanced skill in the
## context (the skill perks' "x% faster" entries; their EPMagic_IsAdvanceSkill tab picks the skill).
func skill_use_mult(av: int) -> float:
	return _data.perk_entry(EP_MOD_SKILL_USE, [ProgressionData.PLAYER_REF, 0, 0], 1.0, {"advance_skill": av})


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


## The skill use (before the AVIF use mult) a craft earned, UESP Skyrim:Leveling. `use` is one of
## SkydotPapyrus.take_crafting_skill_uses: {kind, skill, count, value, delta_value, delta_quality, soul}.
## - create (Smithing): count * (fConstructibleSkillUseConst 25 + fConstructibleSkillUseMult 3 * value ^ fConstructibleSkilluseExp 0.65)
## - improve (Smithing): 3.8 * delta_value ^ fTemperingSkillUseItemValExp * delta_quality ^ 0.5 (the pack's exponent is 0.6;
##   UESP says 0.5), the quality being the tempering level 1 Fine to 6 Legendary
## - brew (Alchemy): the potion's value + 1
## - enchant: 1 per item; disenchant: the enchantment's value / 400; recharge: 0.05 / 0.1 / 0.2 / 0.4 / 0.6 by soul
func crafting_use_amount(use: Dictionary) -> float:
	var value := maxf(float(use.get("value", 0.0)), 0.0)
	match String(use.get("kind", "")):
		"create":
			var base := _data.gmst_float("fConstructibleSkillUseConst", 25.0) \
				+ _data.gmst_float("fConstructibleSkillUseMult", 3.0) * pow(value, _data.gmst_float("fConstructibleSkilluseExp", 0.65))
			return float(use.get("count", 1.0)) * base
		"improve":
			var dv := maxf(float(use.get("delta_value", 0.0)), 0.0)
			var dq := maxf(float(use.get("delta_quality", 0.0)), 0.0)
			return 3.8 * pow(dv, _data.gmst_float("fTemperingSkillUseItemValExp", 0.6)) * pow(dq, 0.5)
		"brew":
			return (value + 1.0) * float(use.get("count", 1.0))
		"enchant":
			return float(use.get("count", 1.0))
		"disenchant":
			return value / 400.0
		"recharge":
			return RECHARGE_USE[clampi(int(use.get("soul", 1)), 1, 5) - 1]
	return 0.0


## The skills a hit trains the player in, from AttackRules.hit's result: [{skill, use}] (skill use before the AVIF
## use mult, in the order weapon, sneak, block, armor). Nothing for hits between NPCs, and nothing for a hit on a corpse.
## - the player hits an actor with a skill weapon: `weapon_use_amount` for One-Handed, Two-Handed or Archery
##   (a friendly target counts; bashes do not exist yet; the player has no bow, so Archery waits for one)
## - a sneak attack (the result's sneak > 1): `sneak_attack_use_amount` for Sneak (A8: nobody sneaks yet)
## - a blocked hit on the player: `block_use_amount` for Block
## - any hit on the player: `armor_use_amounts` for Heavy and Light Armor
func combat_uses(result: Dictionary) -> Array[Dictionary]:
	var uses: Array[Dictionary] = []
	if result.has("error") or float(result.get("health_before", 1.0)) <= 0.0:
		return uses
	if int(result.get("attacker", 0)) == PLAYER_REF:
		var skill := weapon_skill_av(int(result.get("weapon_type", 0)))
		if skill >= 0:
			uses.append({"skill": skill, "use": weapon_use_amount(result)})
		if float(result.get("sneak", 1.0)) > 1.0:
			uses.append({"skill": SNEAK, "use": sneak_attack_use_amount(result)})
	elif int(result.get("target", 0)) == PLAYER_REF:
		if float(result.get("blocked_percent", 0.0)) > 0.0:
			uses.append({"skill": BLOCK, "use": block_use_amount(result)})
		var armor := armor_use_amounts(result)
		for skill: int in armor:
			uses.append({"skill": skill, "use": armor[skill]})
	return uses


## The skill a weapon of animation type `weapon_type` trains (-1: none).
func weapon_skill_av(weapon_type: int) -> int:
	return int(WEAPON_SKILL.get(weapon_type, -1))


## One-Handed, Two-Handed and Archery: the weapon's base damage (a bow's own, not the arrow's; not what the
## hit dealt, UESP Skyrim:Leveling "Base Weapon Damage").
func weapon_use_amount(result: Dictionary) -> float:
	return maxf(float(result.get("weapon_base", 0.0)), 0.0)


## Block: 1 per point of raw damage (before armor) of the blocked hit. A shield bash (5) waits for bashing.
func block_use_amount(result: Dictionary) -> float:
	return maxf(float(result.get("weapon_damage", 0.0)), 0.0)


## Heavy and Light Armor: 1 per point of raw damage (before armor) received, shared by the worn pieces of each
## type equally (ESTIMATE: UESP gives no split). {skill: use}; clothing and shields count no piece.
func armor_use_amounts(result: Dictionary) -> Dictionary:
	var heavy := int(result.get("heavy_pieces", 0))
	var light := int(result.get("light_pieces", 0))
	var out := {}
	if heavy + light <= 0:
		return out
	var raw := maxf(float(result.get("weapon_damage", 0.0)), 0.0)
	if heavy > 0:
		out[HEAVY_ARMOR] = raw * heavy / (heavy + light)
	if light > 0:
		out[LIGHT_ARMOR] = raw * light / (heavy + light)
	return out


## Sneak: 30 per melee sneak attack, 2.5 per ranged one. (Becoming hidden, 2.5, and seconds hidden, 0.625 per
## second, wait for the sneak and detection state, A8.)
func sneak_attack_use_amount(result: Dictionary) -> float:
	return SNEAK_ATTACK_RANGED_USE if bool(result.get("ranged", false)) else SNEAK_ATTACK_MELEE_USE


## The skill and use a landing of a player's spell earned: {skill, use}, or use 0. `landing` is Magic.spell_used:
## {info (Casting cast_info: school, base_cost, type, casting_type, is_scroll, hostile, actor_value), target (0 self),
## target_alive, healed}. Scrolls, powers and shouts give nothing (type != spell, school -1).
## - concentration: the base cost per second, per landing (TICK_SECONDS)
## - Destruction: the base cost, only on a living actor other than the caster (the damage dealt is not counted yet)
## - Restoration: the HP healed by a healing spell (instant heals only), the base cost for other spells
## - Alteration, Conjuration, Illusion: the base cost when the effect applies
func magic_use(landing: Dictionary) -> Dictionary:
	var info: Dictionary = landing.get("info", {})
	var school := int(info.get("school", -1))
	var out := {"skill": school, "use": 0.0}
	if school < ALTERATION or school > RESTORATION or bool(info.get("is_scroll", false)) \
			or int(info.get("type", SPELL_TYPE_SPELL)) != SPELL_TYPE_SPELL:
		return out
	var cost := float(info.get("base_cost", 0.0))
	if int(info.get("casting_type", 0)) == 2:
		cost *= TICK_SECONDS
	match school:
		DESTRUCTION:
			if int(landing.get("target", 0)) != 0 and bool(landing.get("target_alive", true)):
				out["use"] = cost
		RESTORATION:
			if not bool(info.get("hostile", false)) and int(info.get("actor_value", -1)) == HEALTH:
				out["use"] = maxf(float(landing.get("healed", 0.0)), 0.0)
			else:
				out["use"] = cost
		_:
			out["use"] = cost
	return out


## Award a craft's skill use: use_skill for the crafting skill with `crafting_use_amount`.
func crafting_use(use: Dictionary) -> Dictionary:
	return use_skill(int(use.get("skill", 0)), crafting_use_amount(use))


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


## Read `book` (a BOOK base the player carries), UESP Skyrim:Skill_Books and Spell_Tomes. Returns
## {ok, kind ("skill", "spell" or "none"), consumed (the tome is used up: the caller removes it), reason,
## skill (AV), spell, skill_increases, level_up_ready}; reason is "unknown" (no such book), "read" (a skill
## book this game has read: no effect, any copy), "known" (the spell is known: the tome stays).
## - a skill book raises its skill by 1 (Adjust Book Skill Points 5 [player] on the count) through
##   increment_skill, which gives the character XP, and marks the book base read
## - a spell tome teaches its spell and is used up
func read_book(book: int) -> Dictionary:
	var out := {"ok": false, "kind": "none", "consumed": false, "reason": "unknown", "skill": -1, "spell": 0,
			"skill_increases": [], "level_up_ready": false}
	var info := _data.item_info(book)
	var flags := int(info.get("book_flags", 0))
	if info.get("category", "") != "book":
		return out
	out["reason"] = ""
	if flags & BOOK_TEACHES_SKILL:
		out["kind"] = "skill"
		out["skill"] = int(info.get("book_skill", -1))
		if _data.book_read(book):
			out["reason"] = "read"
			return out
		_data.mark_book_read(book)
		var count := roundi(_data.perk_entry(EP_ADJUST_BOOK_SKILL_POINTS, [ProgressionData.PLAYER_REF, 0, 0], 1.0))
		var raised := increment_skill(int(out["skill"]), count)
		out["skill_increases"] = raised["skill_increases"]
		out["level_up_ready"] = raised["level_up_ready"]
		out["ok"] = true
	elif flags & BOOK_TEACHES_SPELL:
		out["kind"] = "spell"
		out["spell"] = int(info.get("book_spell", 0))
		if out["spell"] == 0:
			return out
		if _data.has_spell(out["spell"]):
			out["reason"] = "known"
			return out
		_data.add_spell(out["spell"])
		out["consumed"] = true
		out["ok"] = true
	return out


## One training session with `trainer` (a reference): {skill (AV, -1: teaches nothing), level (the skill's base),
## max_level (the trainer's), cost, sessions_left, gold, reason ("" if possible, else "none", "sessions" -> sCanNotTrainAnymore,
## "level" -> sCanNotTrainHigher, "gold" -> sInsufficientGoldToTrain)}.
func training_info(trainer: int) -> Dictionary:
	var clas := _data.trainer_class(trainer)
	var skill := int(clas.get("teaches", -1))
	var out := {"skill": skill, "level": 0, "max_level": int(clas.get("max_training_level", 0)), "cost": 0,
			"sessions_left": 0, "gold": _data.gold(), "reason": "none"}
	if not is_skill(skill):
		return out
	var level := int(_data.base_av(skill))
	out["level"] = level
	out["cost"] = training_cost(level)
	out["sessions_left"] = maxi(TRAININGS_PER_LEVEL - int(_data.progress()["trainings"]), 0)
	if out["sessions_left"] <= 0:
		out["reason"] = "sessions"
	elif level >= int(out["max_level"]) or level >= MAX_SKILL:
		out["reason"] = "level"
	elif out["gold"] < out["cost"]:
		out["reason"] = "gold"
	else:
		out["reason"] = ""
	return out


## The gold of one session at skill level `level` (UESP Skyrim:Trainers, one session of n(10b + 5n + 45) and its
## expert and master forms): fTrainingMultCost * b * (1, iTrainingExpertCost above 50, iTrainingMasterCost above 75) + 50.
func training_cost(level: int) -> int:
	var mult := 1
	if level > MASTER_FROM:
		mult = _data.gmst_int("iTrainingMasterCost", 5)
	elif level > EXPERT_FROM:
		mult = _data.gmst_int("iTrainingExpertCost", 3)
	return roundi(_data.gmst_float("fTrainingMultCost", 10.0) * mult * level) + TRAINING_BASE_COST


## Train once: the gold goes from the player to the trainer, the skill goes up by 1 (increment_skill: character XP
## too) and the session counts. Returns training_info's dictionary plus {ok, skill_increases, level_up_ready}.
func train(trainer: int) -> Dictionary:
	var out := training_info(trainer)
	out["ok"] = false
	out["skill_increases"] = []
	out["level_up_ready"] = false
	if out["reason"] != "":
		return out
	if not _data.pay_gold(trainer, int(out["cost"])):
		out["reason"] = "gold"
		return out
	_data.set_trainings(int(_data.progress()["trainings"]) + 1)
	var raised := increment_skill(int(out["skill"]), 1)
	out["skill_increases"] = raised["skill_increases"]
	out["level_up_ready"] = raised["level_up_ready"]
	out["ok"] = true
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
	_data.set_trainings(0)  # the sessions are per level
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
