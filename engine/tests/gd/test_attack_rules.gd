# SPDX-License-Identifier: GPL-3.0-or-later
#
# AttackRules: the damage of a hit (ports of the C++ cases of test_combat_damage.cpp), the power
# attack, reach, strike angle and stagger, over a fake AttackData that answers from dictionaries.
extends "gd_test.gd"

const P := 0x14
const NPC := 0x1234
const SWORD := 0x12EB7

class FakeData extends AttackData:
	var facts := {}          # what hit_facts answers; {"error": ...} refuses
	var landed: Array = []   # [attacker, target, weapon, extra, damage, event, flags, breakdown]
	var stamina_of := {}     # actor -> float
	var spent: Array = []    # [actor, amount]
	var weights := {}        # form -> float
	var gmsts := {}

	func hit_facts(_attacker: int, _target: int, _weapon: int, _extra: int, _event: String, _flags: int, _ammo := 0) -> Dictionary:
		return facts

	func land_hit(attacker: int, target: int, weapon: int, extra: int, damage: float, event: String, flags: int,
			breakdown: Dictionary) -> Dictionary:
		landed.append([attacker, target, weapon, extra, damage, event, flags, breakdown])
		return {"health_before": 100.0, "health_after": 100.0 - damage, "killed": false, "poison": 0, "enchantment": 0,
			"charge": 0.0, "charge_max": 0.0}

	func stamina(actor: int) -> float:
		return float(stamina_of.get(actor, 100.0))

	func spend_stamina(actor: int, amount: float) -> void:
		spent.append([actor, amount])

	func weight(form: int) -> float:
		return float(weights.get(form, 0.0))

	func gmst_float(name: String, fallback: float) -> float:
		return float(gmsts.get(name, fallback))

var data: FakeData
var rules: AttackRules


func before_each() -> void:
	data = FakeData.new()
	rules = AttackRules.new(data)


func _near(a: float, b: float, what: String, tolerance := 0.001) -> void:
	expect(absf(a - b) < tolerance, "%s: %f vs %f" % [what, a, b])


## The Iron Sword at OneHanded 15 against heavy iron armor (cuirass 25, helmet 15, gauntlets 10, boots 10).
func _iron_sword() -> Dictionary:
	var armor: Array = []
	for rating in [25.0, 15.0, 10.0, 10.0]:
		armor.append({"rating": rating, "armor_type": 1, "quality": 0, "chest": false})
	return {"weapon_damage": 7.0, "weapon_skill": 15.0, "has_weapon_skill": true, "attacker_is_player": true,
		"target_is_player": true, "heavy_armor_skill": 15.0, "light_armor_skill": 0.0, "attack_mult": 1.0,
		"temper_quality": 0, "ammo_damage": 0.0, "armor": armor}


# ---- ports of test_combat_damage.cpp

func test_iron_sword_at_one_handed_15_against_a_heavy_iron_set() -> void:
	var r := rules.damage(_iron_sword())
	_near(r["skill"], 1.075, "skill lerp(1, 1.5, 0.15)")
	_near(r["weapon_damage"], 7.525, "weapon")
	_near(r["armor_rating"], 65.0, "27 + 16 + 11 + 11")
	_near(r["armor_percent"], 65.0 * 0.12 + 4 * 3.0, "19.8")
	_near(r["damage"], 7.525 * (1.0 - 0.198), "final")


func test_power_attack_doubles_and_an_npc_attacker_has_no_skill_factor() -> void:
	var facts := _iron_sword()
	facts["armor"] = []
	facts["attack_mult"] = 2.0
	_near(rules.damage(facts)["damage"], 7.0 * 1.075 * 2.0, "player")
	facts["attacker_is_player"] = false
	_near(rules.damage(facts)["damage"], 14.0, "npc")


func test_armor_stops_at_the_cap_and_npcs_scale_by_armor_rating_max() -> void:
	var armor: Array = []
	for i in 5:
		armor.append({"rating": 100.0, "armor_type": 1})
	var facts := {"weapon_damage": 10.0, "target_is_player": false, "heavy_armor_skill": 100.0, "armor": armor}
	var r := rules.damage(facts)
	_near(r["armor_percent"], 80.0, "capped")
	_near(r["damage"], 2.0, "final")
	facts["armor"] = [{"rating": 10.0, "armor_type": 1}]
	_near(rules.damage(facts)["armor_rating"], 25.0, "ceil(10 x 2.5)")


func test_unarmed_tempering_and_clothing() -> void:
	var facts := {"weapon_damage": 4.0, "attacker_is_player": false}
	_near(rules.damage(facts)["damage"], 4.0, "unarmed")
	facts["weapon_damage"] = 7.0
	facts["temper_quality"] = 6  # Legendary: +20 x 0.5 for a weapon
	_near(rules.damage(facts)["damage"], 17.0, "legendary")
	facts["armor"] = [{"rating": 10.0, "armor_type": 2}]  # robes: the rating counts, no piece bonus
	_near(rules.damage(facts)["armor_percent"], 1.2, "robes")


func test_tempering_rounds_weapons_to_nearest_and_armor_up_chest_counts_double() -> void:
	_near(rules.temper_bonus(1, 0.5, false), 1.0, "q1 weapon")
	_near(rules.temper_bonus(2, 0.5, false), 3.0, "q2 weapon")
	_near(rules.temper_bonus(6, 1.0, true), 20.0, "q6 chest")
	_near(rules.temper_bonus(1, 0.5, true), 1.0, "q1 armor")
	_near(rules.temper_bonus(2, 0.5, true), 3.0, "q2 armor")
	_near(rules.temper_bonus(0, 1.0, true), 0.0, "q0")
	var facts := {"weapon_damage": 10.0, "light_armor_skill": 0.0, "armor": [{"rating": 10.0, "armor_type": 0, "quality": 6, "chest": true}]}
	_near(rules.damage(facts)["armor_rating"], 30.0, "tempered chest piece")


func test_an_arrow_adds_its_damage_to_the_bows() -> void:
	var facts := {"weapon_damage": 7.0, "ammo_damage": 8.0}
	var r := rules.damage(facts)
	_near(r["base"], 15.0, "base")
	_near(r["damage"], 15.0, "final")
	facts["temper_quality"] = 5  # Epic: 8.2 rounds to 8, the bow's only
	_near(rules.damage(facts)["base"], 23.0, "tempered bow")
	facts["temper_quality"] = 0
	facts["attacker_is_player"] = true
	facts["weapon_skill"] = 100.0
	facts["attack_mult"] = 2.0
	_near(rules.damage(facts)["weapon_damage"], 15.0 * 1.5 * 2.0, "skill and attack scale the sum")


func test_a_player_without_a_weapon_skill_has_no_skill_factor() -> void:
	var facts := {"weapon_damage": 10.0, "attacker_is_player": true, "has_weapon_skill": false, "weapon_skill": 100.0}
	_near(rules.damage(facts)["skill"], 1.0, "fists")


func test_settings_come_from_the_facts() -> void:
	var facts := _iron_sword()
	facts["settings"] = {"fDamagePCSkillMax": 2.0}
	_near(rules.damage(facts)["skill"], 1.15, "lerp(1, 2, 0.15)")


# ---- hit

func test_hit_passes_an_error_through_and_lands_nothing() -> void:
	data.facts = {"error": "the target is dead"}
	var r := rules.hit(P, NPC, SWORD, 0, "attackStart", 0)
	expect_eq(r, {"error": "the target is dead"}, "unchanged")
	expect_eq(data.landed.size(), 0, "nothing landed")


func test_hit_lands_the_computed_damage_and_returns_the_old_keys() -> void:
	data.facts = _iron_sword()
	data.facts["event"] = "attackStart"
	var r := rules.hit(P, NPC, SWORD, 7, "", 0, 0)
	expect_eq(data.landed.size(), 1, "landed once")
	var call: Array = data.landed[0]
	expect_eq([call[0], call[1], call[2], call[3], call[5], call[6]], [P, NPC, SWORD, 7, "attackStart", 0], "arguments")
	_near(call[4], r["damage"], "landed the damage")
	_near(call[4], 7.525 * (1.0 - 0.198), "the formula's damage")
	expect(call[7].has("armor_percent") and call[7].has("base"), "the breakdown goes along")
	for key in ["attacker", "target", "weapon", "ammo", "event", "base", "skill_value", "skill", "attack", "difficulty",
			"sneak", "weapon_damage", "armor_rating", "armor_percent", "damage", "health_before", "health_after", "killed",
			"poison", "enchantment", "charge", "charge_max"]:
		expect(r.has(key), "result has " + key)
	_near(r["skill_value"], 15.0, "skill value")
	_near(r["health_after"], 100.0 - r["damage"], "health after")


# ---- the power attack

func test_power_attack_needs_stamina() -> void:
	data.stamina_of[P] = 0.0
	expect(not rules.can_power_attack(P), "none left")
	data.stamina_of[P] = 1.0
	expect(rules.can_power_attack(P), "some left")


func test_power_attack_cost_and_payment() -> void:
	data.weights[SWORD] = 9.0
	_near(rules.power_attack_cost(P, SWORD), (20.0 + 9.0 * 1.0) * 2.0, "fallback GMSTs")
	_near(rules.power_attack_cost(P, 0), 40.0, "fists")
	data.gmsts["fStaminaAttackWeaponBase"] = 10.0
	data.gmsts["fStaminaAttackWeaponMult"] = 2.0
	data.gmsts["fPowerAttackStaminaPenalty"] = 3.0
	_near(rules.power_attack_cost(P, SWORD), (10.0 + 18.0) * 3.0, "GMSTs")
	rules.pay_power_attack(P, SWORD)
	expect_eq(data.spent, [[P, 84.0]], "the player pays")
	rules.pay_power_attack(NPC, SWORD)
	expect_eq(data.spent, [[P, 84.0], [NPC, 84.0]], "an NPC pays too")


func test_everyone_pays_stamina() -> void:
	expect(rules.pays_stamina(P), "the player")
	expect(rules.pays_stamina(NPC), "an NPC")
	rules.pay_power_attack(NPC, 0)
	expect_eq(data.spent, [[NPC, 40.0]], "an NPC power attack with fists")


func test_power_delay() -> void:
	_near(rules.power_delay(), 0.3, "fallback")
	data.gmsts["fInitialPowerAttackDelay"] = 0.5
	_near(rules.power_delay(), 0.5, "GMST")


# ---- blocking

## A blocked hit of a sword (damage 10) on a target with Block `skill`; `shield` a rating (-1: none).
func _blocked(skill: float, shield := -1.0, sword := SWORD) -> Dictionary:
	var armor: Array = []
	if shield >= 0.0:
		armor.append({"rating": shield, "armor_type": 1, "quality": 0, "chest": false, "shield": true})
	return {"weapon_damage": 10.0, "weapon": sword, "attack_mult": 1.0, "armor": armor, "blocked": true,
		"block_skill": skill, "target_is_player": true}


func test_not_blocked_stops_nothing() -> void:
	var facts := _blocked(15.0, 20.0)
	facts["blocked"] = false
	var r := rules.damage(facts)
	_near(r["blocked_percent"], 0.0, "no block")
	_near(r["damage"], r["damage_before_block"], "no change")


func test_shield_block_formula() -> void:
	var r := rules.damage(_blocked(15.0, 20.0))
	_near(r["blocked_percent"], 45.0 + 0.2 * 20.0 * 1.225, "49.9 %")
	var weapon := 10.0
	var armor_percent: float = r["armor_percent"]
	var arriving := weapon * (1.0 - armor_percent / 100.0)
	_near(r["damage_before_block"], arriving, "arriving")
	_near(r["damage"], arriving * (1.0 - r["blocked_percent"] / 100.0), "final")


func test_weapon_block_against_a_weapon_and_a_creature() -> void:
	var r := rules.damage(_blocked(0.0))
	_near(r["blocked_percent"], 30.0 + 0.2 * 10.0, "weapon of damage 10")
	var creature := _blocked(40.0, -1.0, 0)
	_near(rules.damage(creature)["blocked_percent"], 30.0, "a creature adds no damage")
	creature["target_unarmed"] = true
	data.gmsts.clear()
	_near(rules.damage(creature)["blocked_percent"], 30.0, "fists, mult 1")
	var settings := {"fBlockAmountHandToHandMult": 0.5}
	_near(rules.blocked_percent(creature, settings), 15.0, "fists scale")


func test_power_attack_blocks_less_and_the_cap_is_85() -> void:
	var facts := _blocked(0.0)
	facts["power"] = true
	_near(rules.damage(facts)["blocked_percent"], 32.0 * 0.66, "power x 0.66")
	var strong := _blocked(100.0, 100.0)
	_near(rules.damage(strong)["blocked_percent"], 85.0, "capped")


func test_an_arrow_is_stopped_by_a_shield_only() -> void:
	var weapon := _blocked(15.0)
	weapon["ammo"] = 0x1234
	_near(rules.damage(weapon)["blocked_percent"], 0.0, "weapon block")
	var shield := _blocked(15.0, 20.0)
	shield["ammo"] = 0x1234
	_near(rules.damage(shield)["blocked_percent"], 45.0 + 0.2 * 20.0 * 1.225, "shield block")


func test_a_block_costs_the_blocker_stamina() -> void:
	data.facts = _blocked(0.0, 20.0)
	var r := rules.hit(NPC, P, SWORD, 0, "", 8)
	expect_eq(data.spent.size(), 1, "one payment")
	expect_eq(data.spent[0][0], P, "the blocker pays")
	_near(data.spent[0][1], 0.25 * r["damage_before_block"], "fStaminaBlockDmgMult x arriving")
	expect_eq(int(data.landed[0][6]) & 8, 8, "OnHit flagged blocked")


func test_a_block_that_stops_nothing_is_no_block() -> void:
	var arrow := _blocked(0.0)
	arrow["ammo"] = 1
	data.facts = arrow
	rules.hit(NPC, P, SWORD, 0, "", 8, 1)
	expect(data.spent.is_empty(), "no stamina")
	expect_eq(int(data.landed[0][6]) & 8, 0, "not flagged blocked")


func test_blocked_stagger_is_for_power_attacks() -> void:
	_near(rules.blocked_stagger({"blocked_percent": 30.0, "power": true}), AttackRules.BLOCKED_POWER_STAGGER, "power")
	_near(rules.blocked_stagger({"blocked_percent": 30.0, "power": false}), 0.0, "light")
	_near(rules.blocked_stagger({"blocked_percent": 0.0, "power": true}), 0.0, "not blocked")


# ---- sneak attacks

func _sneak(weapon_type: int, weapon := SWORD) -> Dictionary:
	return {"weapon_damage": 10.0, "attack_mult": 1.0, "weapon": weapon, "weapon_type": weapon_type,
		"attacker_sneaking": true, "target_unaware": true, "attacker_humanoid": true, "armor": []}


func test_sneak_multiplier_per_weapon_type() -> void:
	var expected := {1: 3.0, 2: 3.0, 3: 3.0, 4: 3.0, 5: 2.0, 6: 2.0, 7: 2.0, 8: 1.0, 9: 2.0}
	for type: int in expected:
		_near(rules.sneak_mult(_sneak(type)), expected[type], "type %d" % type)
	_near(rules.damage(_sneak(2))["damage"], 30.0, "a dagger x3 in the damage")


func test_sneak_fists_and_beasts() -> void:
	_near(rules.sneak_mult(_sneak(0, 0)), 2.0, "fists")
	var beast := _sneak(0, 0)
	beast["attacker_humanoid"] = false
	_near(rules.sneak_mult(beast), 1.0, "a beast's natural attack")


func test_sneak_needs_sneaking_and_an_unaware_target() -> void:
	var facts := _sneak(1)
	facts["attacker_sneaking"] = false
	_near(rules.sneak_mult(facts), 1.0, "not sneaking")
	facts = _sneak(1)
	facts["target_unaware"] = false
	_near(rules.sneak_mult(facts), 1.0, "aware target")


func test_sneak_uses_the_gmsts_and_the_perk_hook() -> void:
	var facts := _sneak(1)
	facts["settings"] = {"fCombatSneak1HSwordMult": 5.0}
	_near(rules.sneak_mult(facts), 5.0, "GMST")
	_near(rules.sneak_perk_mult(facts), 1.0, "no perks yet")


class PerkRules extends AttackRules:
	func sneak_perk_mult(_facts: Dictionary) -> float:
		return 2.0


func test_the_perk_hook_multiplies() -> void:
	_near(PerkRules.new(data).sneak_mult(_sneak(2)), 6.0, "x3 x2")


func test_a_sneak_attack_flags_the_hit() -> void:
	data.facts = _sneak(2)
	rules.hit(P, NPC, SWORD, 0, "", 0)
	expect_eq(int(data.landed[0][6]) & 2, 2, "HIT_SNEAK")
	data.facts = _sneak(2)
	data.facts["target_unaware"] = false
	rules.hit(P, NPC, SWORD, 0, "", 0)
	expect_eq(int(data.landed[1][6]) & 2, 0, "no flag")


# ---- difficulty

func _diff(attacker_player: bool, target_player: bool, level: int) -> Dictionary:
	return {"weapon_damage": 10.0, "attack_mult": 1.0, "armor": [], "attacker_is_player": attacker_player,
		"target_is_player": target_player, "difficulty_level": level, "has_weapon_skill": false}


func test_difficulty_scales_the_players_damage() -> void:
	_near(rules.damage(_diff(true, false, 2))["damage"], 10.0, "Adept deals x1")
	_near(rules.damage(_diff(true, false, 4))["damage"], 5.0, "Master deals x0.5")
	_near(rules.damage(_diff(true, false, 0))["damage"], 20.0, "Novice deals x2")
	_near(rules.damage(_diff(false, true, 2))["damage"], 10.0, "Adept takes x1")
	_near(rules.damage(_diff(false, true, 4))["damage"], 20.0, "Master takes x2")
	_near(rules.damage(_diff(false, true, 5))["damage"], 30.0, "Legendary takes x3")


func test_difficulty_leaves_npc_fights_alone() -> void:
	for level in 6:
		_near(rules.damage(_diff(false, false, level))["damage"], 10.0, "level %d" % level)


func test_difficulty_reads_the_gmsts() -> void:
	var facts := _diff(true, false, 3)
	facts["settings"] = {"fDiffMultHPByPCH": 0.6}
	_near(rules.damage(facts)["damage"], 6.0, "GMST")


# ---- reach, cone, stagger

func test_reach_in_metres() -> void:
	var scale := SkydotWorld.unit_scale()
	_near(rules.reach({"reach": 1.0}, {}), 141.0 * scale, "weapon reach x fCombatDistance")
	data.gmsts["fCombatDistance"] = 100.0
	_near(rules.reach({"reach": 1.3}, {}), 130.0 * scale, "GMST")
	_near(rules.reach({}, {"unarmed_reach": 64.0}), 64.0 * scale, "the race's fists")
	_near(rules.reach({}, {}), 96.0 * scale, "fists without a race")


func test_strike_angle() -> void:
	var race := {"attacks": [{"event": "AttackLeft", "strike_angle": 20.0}]}
	_near(rules.strike_angle(race, "attackleft", false), 20.0, "the race's, any case")
	_near(rules.strike_angle(race, "attackStart", false), 50.0, "light default")
	_near(rules.strike_angle({}, "attackPowerStart", true), 35.0, "power default")


func test_stagger_lookup() -> void:
	var race := {"attacks": [{"event": "attackTail", "stagger": 0.8}, {"event": "attackLeftWing", "stagger": 0.6},
		{"event": "attackRightWing", "stagger": 0.7}]}
	_near(rules.stagger(race, "tail", 0.5), 0.8, "tail")
	_near(rules.stagger(race, "wing", 0.5), 0.7, "the last wing wins")
	_near(rules.stagger({}, "tail", 0.5), 0.5, "default")


func test_a_mod_overrides_a_rule() -> void:
	var mod := ModRules.new(data)
	data.facts = {"weapon_damage": 10.0}
	_near(mod.hit(P, NPC, 0, 0, "", 0)["damage"], 20.0, "double sneak")


class ModRules extends AttackRules:
	func sneak_mult(_facts: Dictionary) -> float:
		return 2.0
