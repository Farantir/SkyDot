# SPDX-License-Identifier: GPL-3.0-or-later
#
# What AttackRules asks the data (ARCHITECTURE.md 8.2, ADR 0009): narrow questions over
# SkydotPapyrus and SkydotWorld, and no rule of its own. The facts of a hit (the weapon, the
# attacker's skill, the target's worn armor, the GMSTs of the formula), landing a hit (Health,
# killer, poison and enchantment, OnHit, death), an actor's Stamina and what it is spent on,
# a weapon's weight and the GMSTs. A native object cannot be faked, so AttackRules' unit tests
# (tests/gd/test_attack_rules.gd) extend this class and answer from dictionaries. A mod does not
# override this; it overrides AttackRules' rule methods.
class_name AttackData
extends RefCounted

var _papyrus: SkydotPapyrus
var _world: SkydotWorld
## The difficulty level, 0 (Novice) .. 5 (Legendary): the session's setting (GameConfig.difficulty).
var difficulty_level := 2


func _init(papyrus: SkydotPapyrus = null, world: SkydotWorld = null) -> void:
	_papyrus = papyrus
	_world = world


## SkydotPapyrus.hit_facts plus `settings`, the GMSTs the formula reads ({error} if the hit is
## refused: no actor, dead, a ghost).
func hit_facts(attacker: int, target: int, weapon: int, extra: int, event: String, flags: int, ammo := 0) -> Dictionary:
	var facts := _papyrus.hit_facts(attacker, target, weapon, extra, event, flags, ammo)
	if facts.has("error"):
		return facts
	var settings := {}
	for name in AttackRules.SETTINGS:
		settings[name] = _world.get_gmst_float(name, AttackRules.SETTINGS[name])
	facts["settings"] = settings
	facts["difficulty_level"] = difficulty_level
	return facts


## SkydotPapyrus.land_hit: apply `damage` and everything that follows a hit; `breakdown` is
## what the rules worked out (for the log line). {health_before, health_after, killed, poison,
## enchantment, charge, charge_max}.
func land_hit(attacker: int, target: int, weapon: int, extra: int, damage: float, event: String, flags: int,
		breakdown: Dictionary) -> Dictionary:
	return _papyrus.land_hit(attacker, target, weapon, extra, damage, event, flags, breakdown)


## A perk entry point (perks::EntryPoint id) applied to `value` for `owner`'s perks; `subjects` are the
## condition subjects of tabs 0, 1, 2 (SkydotPapyrus.perk_entry). 35, 28, 36, 37 are multipliers starting
## at 1.0, 18 and 27 start at the value the rules computed.
func perk_entry(entry_point: int, owner: int, subjects: Array, value: float) -> float:
	return _papyrus.perk_entry(entry_point, owner, subjects, value)


func stamina(actor: int) -> float:
	return _papyrus.get_actor_value(actor, "Stamina")


func spend_stamina(actor: int, amount: float) -> void:
	_papyrus.damage_actor_value(actor, "Stamina", amount)


## The weight of a weapon form; 0 for none.
func weight(form: int) -> float:
	return float(_papyrus.item_info(form).get("weight", 0.0)) if form != 0 else 0.0


func gmst_float(name: String, fallback: float) -> float:
	return _world.get_gmst_float(name, fallback)
