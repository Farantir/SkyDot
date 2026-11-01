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
#            + fArmorBaseFactor x 100 x pieces)     final = weapon x (1 - armor / 100)
#
# A script mod extends this class and overrides a rule method (`damage` or one of its parts
# `skill_factor`, `armor_percent`, `sneak_mult`, `difficulty`; `can_power_attack`, `pays_stamina`,
# `power_attack_cost`, `power_delay`, `reach`, `strike_angle`, `stagger`), or replaces the class
# whole (GameSession.attack_rules_script). It holds no state: nothing to reset.
class_name AttackRules
extends RefCounted

const PLAYER_REF := 0x14
const HIT_POWER := 1
## The GMSTs of the formula with their values in Skyrim.esm (no GMST holds the skill minimum).
const SETTINGS := {
	"fDamagePCSkillMax": 1.5, "fArmorScalingFactor": 0.12, "fMaxArmorRating": 80.0,
	"fArmorBaseFactor": 0.03, "fArmorRatingBase": 1.0, "fArmorRatingPCMax": 1.4,
	"fArmorRatingMax": 2.5, "fClothingArmorScale": 1.0,
}
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
## armor_percent, damage, then health_before, health_after, killed, poison, enchantment, charge,
## charge_max.
func hit(attacker: int, target: int, weapon: int, extra: int, event: String, flags: int, ammo := 0) -> Dictionary:
	var facts := _data.hit_facts(attacker, target, weapon, extra, event, flags, ammo)
	if facts.has("error"):
		return facts
	var out := damage(facts)
	out["attacker"] = attacker
	out["target"] = target
	out["weapon"] = weapon
	out["ammo"] = ammo
	out["event"] = String(facts.get("event", event))
	out["skill_value"] = float(facts.get("weapon_skill", 0.0))
	out.merge(_data.land_hit(attacker, target, weapon, extra, float(out["damage"]), String(out["event"]), flags, out))
	return out


## The breakdown of the damage of a hit from its `facts` (SkydotPapyrus.hit_facts and `settings`):
## {base, skill, attack, difficulty, sneak, weapon_damage (before armor), armor_rating,
## armor_percent, damage}.
func damage(facts: Dictionary) -> Dictionary:
	var settings: Dictionary = facts.get("settings", {})
	var base := float(facts.get("weapon_damage", 0.0)) \
		+ temper_bonus(int(facts.get("temper_quality", 0)), 0.5, false) + float(facts.get("ammo_damage", 0.0))
	var skill := skill_factor(facts, settings)
	var attack := float(facts.get("attack_mult", 1.0))
	var diff := difficulty(facts)
	var sneak := sneak_mult(facts)
	var weapon := base * skill * attack * diff * sneak
	var armor := armor_percent(facts, settings)
	return {"base": base, "skill": skill, "attack": attack, "difficulty": diff, "sneak": sneak,
		"weapon_damage": weapon, "armor_rating": armor["rating"], "armor_percent": armor["percent"],
		"damage": weapon * (1.0 - float(armor["percent"]) / 100.0)}


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


## PLACEHOLDER (A8, sneaking): no sneak attack bonus yet.
func sneak_mult(_facts: Dictionary) -> float:
	return 1.0


## PLACEHOLDER (difficulty setting, P-later): Adept, no GMST holds it.
func difficulty(_facts: Dictionary) -> float:
	return 1.0


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


## Whether a power attack costs `attacker` Stamina. PLACEHOLDER: only the player pays; NPCs pay
## nothing until their AI budgets Stamina.
func pays_stamina(attacker: int) -> bool:
	return attacker == PLAYER_REF


## Stamina a power attack with `weapon_form` costs: (fStaminaAttackWeaponBase + the weapon's weight
## times fStaminaAttackWeaponMult) times fPowerAttackStaminaPenalty. A light attack costs none.
func power_attack_cost(_attacker: int, weapon_form: int) -> float:
	return (_data.gmst_float(GMST_ATTACK_BASE, ATTACK_BASE_FALLBACK)
		+ _data.weight(weapon_form) * _data.gmst_float(GMST_ATTACK_MULT, ATTACK_MULT_FALLBACK)) \
		* _data.gmst_float(GMST_POWER_PENALTY, POWER_PENALTY_FALLBACK)


## Pay the Stamina of a power attack (nothing if `attacker` does not pay).
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
