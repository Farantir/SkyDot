# SPDX-License-Identifier: GPL-3.0-or-later
#
# The rules of a hit (ARCHITECTURE.md 8.2, ADR 0009), for the player and NPCs: the damage of a
# blow (the formula of UESP Skyrim:Damage, Armor and Smithing), the stamina of a power attack,
# the hold time that makes one, how far and in what cone a blow reaches, and how hard a blow
# staggers. It plays no clip, emits nothing and touches no node: `Combat` turns what it answers
# into swings and impacts. Data comes from an AttackData (the tests give it a fake); no signals,
# no calls to Papyrus but through it.
#
# `hit` asks the data for the facts of the hit (refusals come back as {error}), works out
# `damage(facts)` and has the data land it; it returns the breakdown merged with what landing
# gave (SkydotPapyrus.land_hit). The formula:
#   base   = weapon + tempering + ammo           weapon = base x skill x attack x difficulty x sneak
#   skill  = player with a weapon skill: lerp(1, fDamagePCSkillMax, skill / 100); otherwise 1
#   armor  = min(fMaxArmorRating, sum(ceil(rating x armor skill factor)) x fArmorScalingFactor
#            + fArmorBaseFactor x 100 x pieces)     final = weapon x (1 - armor / 100) x (1 - blocked / 100)
#   blocked: see `blocked_percent` (UESP Skyrim:Block)
#
# A script mod extends this class and overrides a rule method (`damage` or one of its parts
# `skill_factor`, `armor_percent`, `blocked_percent`, `sneak_mult`, `difficulty`; `block_stamina`, `blocked_stagger`; `can_power_attack`, `pays_stamina`,
# `power_attack_cost`, `power_delay`, `reach`, `strike_angle`, `stagger`), or replaces the class
# whole (GameSession.attack_rules_script). It holds no state: nothing to reset.
class_name AttackRules
extends RefCounted

const PLAYER_REF := 0x14
const HIT_POWER := 1
## Perk entry points (perks::EntryPoint) the rules ask.
const EP_ATTACK_DAMAGE := 35
const EP_POWER_ATTACK_DAMAGE := 28
const EP_SNEAK_ATTACK_MULT := 18
const EP_INCOMING_DAMAGE := 36
const EP_POWER_ATTACK_STAMINA := 27
## The GMSTs of the formula with their values in Skyrim.esm (no GMST holds the skill minimum).
const SETTINGS := {
	"fDamagePCSkillMax": 1.5, "fArmorScalingFactor": 0.12, "fMaxArmorRating": 80.0,
	"fArmorBaseFactor": 0.03, "fArmorRatingBase": 1.0, "fArmorRatingPCMax": 1.4,
	"fArmorRatingMax": 2.5, "fClothingArmorScale": 1.0,
	# Blocking (UESP Skyrim:Block). fBlockAmountHandToHandMult is not in Skyrim.esm: ESTIMATE, 1.
	"fShieldBaseFactor": 0.45, "fShieldScalingFactor": 0.2, "fBlockWeaponBase": 0.3,
	"fBlockWeaponScaling": 0.2, "fBlockAmountWeaponMult": 1.0, "fBlockAmountHandToHandMult": 1.0,
	"fBlockPowerAttackMult": 0.66, "fStaminaBlockBase": 0.0, "fStaminaBlockDmgMult": 0.25,
	# Sneak attacks. The bow value is UESP's (x2); fCombatSneakBowMult and fCombatSneakCrossbowMult are not
	# in Skyrim.esm and the crossbow value is an ESTIMATE.
	"fCombatSneak1HSwordMult": 3.0, "fCombatSneak1HDaggerMult": 3.0, "fCombatSneak1HAxeMult": 3.0,
	"fCombatSneak1HMaceMult": 3.0, "fCombatSneak2HSwordMult": 2.0, "fCombatSneak2HAxeMult": 2.0,
	"fCombatSneakHandMult": 2.0, "fCombatSneakBowMult": 2.0, "fCombatSneakCrossbowMult": 2.0,
	# Difficulty, Novice to Legendary: the damage the player deals and the damage he takes.
	"fDiffMultHPByPCVE": 2.0, "fDiffMultHPByPCE": 1.5, "fDiffMultHPByPCN": 1.0,
	"fDiffMultHPByPCH": 0.75, "fDiffMultHPByPCVH": 0.5, "fDiffMultHPByPCL": 0.25,
	"fDiffMultHPToPCVE": 0.5, "fDiffMultHPToPCE": 0.75, "fDiffMultHPToPCN": 1.0,
	"fDiffMultHPToPCH": 1.5, "fDiffMultHPToPCVH": 2.0, "fDiffMultHPToPCL": 3.0,
}
## The suffixes of the difficulty GMSTs by level 0 (Novice) .. 5 (Legendary); the default is 2 (Adept).
const DIFFICULTY_SUFFIX: Array[String] = ["VE", "E", "N", "H", "VH", "L"]
const DIFFICULTY_DEFAULT := 2
## Weapon type (WEAP animation type) -> the sneak attack GMST; staffs (8) have none: x1.
const SNEAK_GMST := {
	1: "fCombatSneak1HSwordMult", 2: "fCombatSneak1HDaggerMult", 3: "fCombatSneak1HAxeMult",
	4: "fCombatSneak1HMaceMult", 5: "fCombatSneak2HSwordMult", 6: "fCombatSneak2HAxeMult",  # warhammers are 6
	7: "fCombatSneakBowMult", 9: "fCombatSneakCrossbowMult",
}
const HIT_SNEAK := 2
const HIT_BLOCKED := 8
## UESP's measured cap of the blocked share, percent (the esm's fBlockMax is 0.7: not used).
const BLOCK_CAP := 85.0
## UESP's weight of the Block skill in the blocked share (the esm's fBlockSkillMult, 2.0, is not used).
const BLOCK_SKILL_FACTOR := 1.5
## ESTIMATE: the stagger magnitude of a blocked power attack on the blocker (no source for it).
const BLOCKED_POWER_STAGGER := 0.25
const DAMAGE_PC_SKILL_MIN := 1.0
const ARMOR_CLOTHING := 2
const ARMOR_HEAVY := 1
const GMST_POWER_DELAY := "fInitialPowerAttackDelay"
const POWER_DELAY_FALLBACK := 0.3
const GMST_ATTACK_BASE := "fStaminaAttackWeaponBase"
const ATTACK_BASE_FALLBACK := 20.0
const GMST_ATTACK_MULT := "fStaminaAttackWeaponMult"
const ATTACK_MULT_FALLBACK := 1.0
const GMST_POWER_PENALTY := "fPowerAttackStaminaPenalty"
const POWER_PENALTY_FALLBACK := 2.0
const GMST_COMBAT_DISTANCE := "fCombatDistance"
const COMBAT_DISTANCE_FALLBACK := 141.0
const UNARMED_REACH_FALLBACK := 96.0
const STRIKE_ANGLE_LIGHT := 50.0
const STRIKE_ANGLE_POWER := 35.0

var _data: AttackData


func _init(data: AttackData = null) -> void:
	_data = data


# ---- a hit

## One hit of `attacker` on `target` with `weapon` (0 fists; `extra` its item extra) for the race
## attack `event` ("" the standing light or power attack by `flags`' HIT_POWER) and `ammo` (an
## arrow's own damage adds to the bow's). {error} if refused; else attacker, target, weapon, ammo,
## event, base, skill_value, skill, attack, difficulty, sneak, weapon_damage, armor_rating,
## armor_percent, damage_before_block, blocked_percent, damage, power, then health_before, health_after, killed, poison, enchantment, charge,
## charge_max.
func hit(attacker: int, target: int, weapon: int, extra: int, event: String, flags: int, ammo := 0) -> Dictionary:
	var facts := _data.hit_facts(attacker, target, weapon, extra, event, flags, ammo)
	if facts.has("error"):
		return facts
	facts["power"] = (flags & HIT_POWER) != 0
	var out := damage(facts)
	out["attacker"] = attacker
	out["target"] = target
	out["weapon"] = weapon
	out["ammo"] = ammo
	out["event"] = String(facts.get("event", event))
	out["power"] = (flags & HIT_POWER) != 0
	if float(out["sneak"]) > 1.0:
		flags |= HIT_SNEAK
	if float(out["blocked_percent"]) > 0.0:
		_data.spend_stamina(target, block_stamina(out, facts.get("settings", {})))
	else:
		flags &= ~HIT_BLOCKED  # a block that stopped nothing (a weapon against an arrow) is no block
	out["skill_value"] = float(facts.get("weapon_skill", 0.0))
	out.merge(_data.land_hit(attacker, target, weapon, extra, float(out["damage"]), String(out["event"]), flags, out))
	return out


## The breakdown of the damage of a hit from its `facts` (SkydotPapyrus.hit_facts and `settings`):
## {base, skill, attack, difficulty, sneak, weapon_damage (before armor), armor_rating,
## armor_percent, damage_before_block, blocked_percent, damage}.
func damage(facts: Dictionary) -> Dictionary:
	var settings: Dictionary = facts.get("settings", {})
	var base := float(facts.get("weapon_damage", 0.0)) \
		+ temper_bonus(int(facts.get("temper_quality", 0)), 0.5, false) + float(facts.get("ammo_damage", 0.0))
	var skill := skill_factor(facts, settings)
	var attack := float(facts.get("attack_mult", 1.0))
	var diff := difficulty(facts)
	var sneak := sneak_mult(facts)
	var perks := perk_damage_mult(facts)
	var weapon := base * skill * attack * diff * sneak * perks
	var armor := armor_percent(facts, settings)
	var arriving := weapon * (1.0 - float(armor["percent"]) / 100.0)
	var blocked := blocked_percent(facts, settings)
	var final := arriving * (1.0 - blocked / 100.0) * incoming_perk_mult(facts)
	return {"base": base, "skill": skill, "attack": attack, "difficulty": diff, "sneak": sneak, "perks": perks,
		"weapon_damage": weapon, "armor_rating": armor["rating"], "armor_percent": armor["percent"],
		"damage_before_block": arriving, "blocked_percent": blocked, "damage": final}


## The attacker's perks' multiplier on the weapon damage (UESP Skyrim:Damage: with the skill factor, before
## armor): Mod Attack Damage 35 [attacker, weapon base, target], and for a power attack Mod Power Attack
## Damage 28 [attacker, weapon base, target].
func perk_damage_mult(facts: Dictionary) -> float:
	var attacker := int(facts.get("attacker", 0))
	var subjects := [attacker, int(facts.get("weapon", 0)), int(facts.get("target", 0))]
	var mult := _data.perk_entry(EP_ATTACK_DAMAGE, attacker, subjects, 1.0)
	if bool(facts.get("power", false)):
		mult *= _data.perk_entry(EP_POWER_ATTACK_DAMAGE, attacker, subjects, 1.0)
	return mult


## The victim's perks' multiplier on the damage that gets through: Mod Incoming Damage 36
## [victim, attacker, attacker's weapon base].
func incoming_perk_mult(facts: Dictionary) -> float:
	var victim := int(facts.get("target", 0))
	return _data.perk_entry(EP_INCOMING_DAMAGE, victim,
		[victim, int(facts.get("attacker", 0)), int(facts.get("weapon", 0))], 1.0)


## The share of the damage a block stops, percent (0 if the hit was not blocked): UESP Skyrim:Block.
## A shield: fShieldBaseFactor x 100 + fShieldScalingFactor x the shield's own armor rating x
## (1 + Block x BLOCK_SKILL_FACTOR / 100). Without one (a weapon or fists blocking): (fBlockWeaponBase x 100
## + fBlockWeaponScaling x the attacker's weapon base damage x the same skill term) x fBlockAmountWeaponMult,
## times fBlockAmountHandToHandMult for fists; an attacker with no weapon adds 0 damage, and an arrow is
## not stopped by a weapon. A power attack scales it by fBlockPowerAttackMult. Capped at BLOCK_CAP.
func blocked_percent(facts: Dictionary, settings: Dictionary) -> float:
	if not bool(facts.get("blocked", false)):
		return 0.0
	var skill_term := 1.0 + float(facts.get("block_skill", 0.0)) * BLOCK_SKILL_FACTOR / 100.0
	var percent := 0.0
	var shield_rating := -1.0
	for piece: Dictionary in facts.get("armor", []):
		if bool(piece.get("shield", false)):
			shield_rating = float(piece["rating"])
	if shield_rating >= 0.0:
		percent = _setting(settings, "fShieldBaseFactor") * 100.0 \
			+ _setting(settings, "fShieldScalingFactor") * shield_rating * skill_term
	else:
		if int(facts.get("ammo", 0)) != 0:
			return 0.0
		var attacker_damage := float(facts.get("weapon_damage", 0.0)) if int(facts.get("weapon", 0)) != 0 else 0.0
		percent = (_setting(settings, "fBlockWeaponBase") * 100.0
			+ _setting(settings, "fBlockWeaponScaling") * attacker_damage * skill_term) \
			* _setting(settings, "fBlockAmountWeaponMult")
		if bool(facts.get("target_unarmed", false)):
			percent *= _setting(settings, "fBlockAmountHandToHandMult")
	if bool(facts.get("power", false)):
		percent *= _setting(settings, "fBlockPowerAttackMult")
	return minf(percent, BLOCK_CAP)


## Stamina the blocker pays for the hit it stopped: fStaminaBlockBase + fStaminaBlockDmgMult x the
## damage that arrived (after armor, before the block). ESTIMATE: which damage the game takes is assumed.
func block_stamina(result: Dictionary, settings: Dictionary) -> float:
	return _setting(settings, "fStaminaBlockBase") \
		+ _setting(settings, "fStaminaBlockDmgMult") * float(result.get("damage_before_block", 0.0))


## The stagger magnitude a blocked hit gives the blocker (0 none). ESTIMATE: a blocked light attack does
## not stagger, a blocked power attack does (BLOCKED_POWER_STAGGER).
func blocked_stagger(result: Dictionary) -> float:
	if float(result.get("blocked_percent", 0.0)) > 0.0 and bool(result.get("power", false)):
		return BLOCKED_POWER_STAGGER
	return 0.0


## The player with a weapon skill: lerp(1, fDamagePCSkillMax, skill / 100); everyone else 1.
func skill_factor(facts: Dictionary, settings: Dictionary) -> float:
	if not bool(facts.get("attacker_is_player", false)) or not bool(facts.get("has_weapon_skill", true)):
		return 1.0
	return _lerp(DAMAGE_PC_SKILL_MIN, _setting(settings, "fDamagePCSkillMax"), float(facts.get("weapon_skill", 0.0)) / 100.0)


## {rating, percent}: the target's armor rating and the damage reduction in percent, capped at
## fMaxArmorRating. Clothing counts its rating at fClothingArmorScale with no skill and no piece
## bonus; other armor scales by the target's skill (the player's top is fArmorRatingPCMax, an NPC's
## fArmorRatingMax), after tempering (rounded up).
func armor_percent(facts: Dictionary, settings: Dictionary) -> Dictionary:
	var rating := 0.0
	var pieces := 0
	var top := _setting(settings, "fArmorRatingPCMax" if bool(facts.get("target_is_player", false)) else "fArmorRatingMax")
	for piece: Dictionary in facts.get("armor", []):
		if int(piece.get("armor_type", 0)) == ARMOR_CLOTHING:
			rating += float(piece["rating"]) * _setting(settings, "fClothingArmorScale")
			continue
		var skill := float(facts.get("heavy_armor_skill" if int(piece.get("armor_type", 0)) == ARMOR_HEAVY else "light_armor_skill", 0.0))
		var factor := _lerp(_setting(settings, "fArmorRatingBase"), top, skill / 100.0)
		var tempered := float(piece["rating"]) + temper_bonus(int(piece.get("quality", 0)), 1.0 if bool(piece.get("chest", false)) else 0.5, true)
		rating += ceilf(tempered * factor)
		pieces += 1
	var percent := rating * _setting(settings, "fArmorScalingFactor") + _setting(settings, "fArmorBaseFactor") * 100.0 * pieces
	return {"rating": rating, "percent": minf(_setting(settings, "fMaxArmorRating"), percent)}


## The sneak attack multiplier: the attacker is sneaking and the target does not know of him (else 1),
## the weapon type's GMST (fists x fCombatSneakHandMult; a beast's natural attacks x1, ESTIMATE; staffs
## x1) times `sneak_perk_mult`. PLACEHOLDER (A8): C++ reports nobody sneaking yet.
func sneak_mult(facts: Dictionary) -> float:
	if not bool(facts.get("attacker_sneaking", false)) or not bool(facts.get("target_unaware", false)):
		return 1.0
	var settings: Dictionary = facts.get("settings", {})
	var mult := 1.0
	if int(facts.get("weapon", 0)) == 0:
		if bool(facts.get("attacker_humanoid", true)):
			mult = _setting(settings, "fCombatSneakHandMult")
	elif SNEAK_GMST.has(int(facts.get("weapon_type", 0))):
		mult = _setting(settings, SNEAK_GMST[int(facts["weapon_type"])])
	return sneak_perk_mult(facts, mult)


## The sneak attack multiplier after the perks: Mod Sneak Attack Mult 18 [attacker, weapon base, target]
## applied to the weapon type's `mult` (Backstab sets one-handed to 6, Assassin's Blade daggers to 15, Deadly Aim bows to 3).
func sneak_perk_mult(facts: Dictionary, mult: float) -> float:
	var attacker := int(facts.get("attacker", 0))
	return _data.perk_entry(EP_SNEAK_ATTACK_MULT, attacker,
		[attacker, int(facts.get("weapon", 0)), int(facts.get("target", 0))], mult)


## What the difficulty does to the damage: the player's blows by fDiffMultHPByPC, the blows he takes by
## fDiffMultHPToPC (levels Novice..Legendary, `difficulty_level` of the facts, default Adept); between
## NPCs 1.
func difficulty(facts: Dictionary) -> float:
	var player_hits := bool(facts.get("attacker_is_player", false))
	if not player_hits and not bool(facts.get("target_is_player", false)):
		return 1.0
	var level := clampi(int(facts.get("difficulty_level", DIFFICULTY_DEFAULT)), 0, DIFFICULTY_SUFFIX.size() - 1)
	var name := ("fDiffMultHPByPC" if player_hits else "fDiffMultHPToPC") + DIFFICULTY_SUFFIX[level]
	return _setting(facts.get("settings", {}), name)


## Tempering adds, it does not multiply (UESP Skyrim:Smithing): quality level 1 (Fine) to 6
## (Legendary) gives 3.6 x ql - 1.6, times 1 for chest armor and 0.5 for weapons and other armor.
## Weapons round to the nearest, armor up.
func temper_bonus(quality: int, factor: float, round_up: bool) -> float:
	if quality <= 0:
		return 0.0
	var bonus := (3.6 * quality - 1.6) * factor
	return ceilf(bonus - 0.001) if round_up else roundf(bonus)


# ---- the power attack

## Whether `attacker` may start a power attack: it needs Stamina.
func can_power_attack(attacker: int) -> bool:
	return _data.stamina(attacker) > 0.0


## Whether a power attack costs `attacker` Stamina: it does for everyone, the player and NPCs (an
## NPC without Stamina attacks normally instead, see Combat.npc_attack).
func pays_stamina(_attacker: int) -> bool:
	return true


## Stamina a power attack with `weapon_form` costs: (fStaminaAttackWeaponBase + the weapon's weight
## times fStaminaAttackWeaponMult) times fPowerAttackStaminaPenalty. A light attack costs none.
func power_attack_cost(attacker: int, weapon_form: int) -> float:
	var cost := (_data.gmst_float(GMST_ATTACK_BASE, ATTACK_BASE_FALLBACK)
		+ _data.weight(weapon_form) * _data.gmst_float(GMST_ATTACK_MULT, ATTACK_MULT_FALLBACK)) \
		* _data.gmst_float(GMST_POWER_PENALTY, POWER_PENALTY_FALLBACK)
	# Mod Power Attack Stamina 27 [attacker, weapon base] (Stamina perks make power attacks cheaper).
	return _data.perk_entry(EP_POWER_ATTACK_STAMINA, attacker, [attacker, weapon_form, 0], cost)


## Pay the Stamina of a power attack (nothing if `attacker` does not pay, see pays_stamina).
func pay_power_attack(attacker: int, weapon_form: int) -> void:
	if pays_stamina(attacker):
		_data.spend_stamina(attacker, power_attack_cost(attacker, weapon_form))


## Seconds the attack button is held before a power attack starts (fInitialPowerAttackDelay).
func power_delay() -> float:
	return _data.gmst_float(GMST_POWER_DELAY, POWER_DELAY_FALLBACK)


# ---- reach, cone, stagger

## How far a blow reaches, metres: the weapon's reach times fCombatDistance, or for fists the
## race's unarmed reach (game units). `weapon_info` is the weapon's item info (empty: fists).
func reach(weapon_info: Dictionary, race: Dictionary) -> float:
	var units: float
	if weapon_info.is_empty():
		units = float(race.get("unarmed_reach", UNARMED_REACH_FALLBACK))
	else:
		units = float(weapon_info["reach"]) * _data.gmst_float(GMST_COMBAT_DISTANCE, COMBAT_DISTANCE_FALLBACK)
	return units * SkydotWorld.unit_scale()


## Half the cone of the race's attack of `event`, degrees.
func strike_angle(race: Dictionary, event: String, power: bool) -> float:
	for attack_data: Dictionary in race.get("attacks", []):
		if String(attack_data["event"]).to_lower() == event.to_lower():
			return float(attack_data["strike_angle"])
	return STRIKE_ANGLE_POWER if power else STRIKE_ANGLE_LIGHT


## How hard the race's attack whose event contains `kind` ("tail", "wing") staggers; `default`
## if it has none (the last match wins).
func stagger(race: Dictionary, kind: String, default: float) -> float:
	var magnitude := default
	for entry: Dictionary in race.get("attacks", []):
		if String(entry["event"]).to_lower().contains(kind.to_lower()):
			magnitude = float(entry["stagger"])
	return magnitude


# ---- helpers

static func _lerp(a: float, b: float, t: float) -> float:
	return a + (b - a) * clampf(t, 0.0, 1.0)


func _setting(settings: Dictionary, name: String) -> float:
	return float(settings.get(name, SETTINGS[name]))
