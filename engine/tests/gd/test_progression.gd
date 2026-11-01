# SPDX-License-Identifier: GPL-3.0-or-later
#
# Progression: skill use, skill increases, level-ups, perks and legendary skills over a fake
# ProgressionData. Oracles are computed by hand from the pack's GMSTs and AVSK rows (UESP
# Skyrim:Leveling): fSkillUseCurve 1.95, fXPLevelUpBase 75, fXPLevelUpMult 25, iAVDhmsLevelUp 10,
# fLevelUpCarryWeightMod 5; Lockpicking use 45 + 10, improve 0.25 * L^1.95 + 300; OneHanded use
# 6.3 + 0, improve 2 * L^1.95 + 0.
extends "gd_test.gd"

const LOCKPICKING := 14
const ONE_HANDED := 6
const HEALTH := 24
const MAGICKA := 25
const STAMINA := 26
const CARRY := 32
const NOVICE := 0x300
const ADEPT_1 := 0x301
const ADEPT_2 := 0x302
const EXPERT := 0x303
const LOOSE := 0x304   # playable, in no tree

class FakeData extends ProgressionData:
	var bases := {6: 15.0, 14: 15.0, 24: 100.0, 25: 100.0, 26: 100.0, 32: 300.0}
	var prog := {"skill_xp": [], "xp": 0.0, "level": 1, "perk_points": 0, "trainings": 0, "legendary": []}
	var ranks := {}          # perk -> rank
	var conditions := {}     # perk -> false when its prerequisites fail
	var restored: Array = []
	var perks := {
		0x300: {"name": "Novice Locks", "playable": true, "next_perk": 0},
		0x301: {"name": "Apprentice Locks 1", "playable": true, "next_perk": 0x302},
		0x302: {"name": "Apprentice Locks 2", "playable": true, "next_perk": 0},
		0x303: {"name": "Expert Locks", "playable": true, "next_perk": 0},
		0x304: {"name": "Loose", "playable": true, "next_perk": 0},
	}
	var trees := {14: [
		{"index": 0, "perk": 0, "links": [1]},
		{"index": 1, "perk": 0x300, "links": [2]},
		{"index": 2, "perk": 0x301, "links": [3]},
		{"index": 3, "perk": 0x303, "links": []},
	]}

	func _init() -> void:
		for i in 18:
			prog["skill_xp"].append(0.0)
			prog["legendary"].append(0)

	func gmst_float(name: String, fallback: float) -> float:
		return {"fSkillUseCurve": 1.95, "fXPLevelUpBase": 75.0, "fXPLevelUpMult": 25.0,
				"fLevelUpCarryWeightMod": 5.0}.get(name, fallback)

	func gmst_int(name: String, fallback: int) -> int:
		return {"iAVDhmsLevelUp": 10}.get(name, fallback)

	var skill_use_perk := {}   # skill -> Mod Skill Use multiplier

	func perk_entry(_ep: int, _subjects: Array, value: float, extra := {}) -> float:
		return value * float(skill_use_perk.get(int(extra.get("advance_skill", -1)), 1.0))

	func skill_info(av: int) -> Dictionary:
		if av == 14:
			return {"name": "Lockpicking", "use_mult": 45.0, "use_offset": 10.0,
					"improve_mult": 0.25, "improve_offset": 300.0, "perk_tree": trees[14]}
		if av == 6:
			return {"name": "One-handed", "use_mult": 6.3, "use_offset": 0.0,
					"improve_mult": 2.0, "improve_offset": 0.0, "perk_tree": []}
		return {}

	func base_av(av: int) -> float:
		return float(bases.get(av, 0.0))

	func current_av(av: int) -> float:
		return base_av(av)

	func set_base_av(av: int, value: float) -> void:
		bases[av] = value

	func restore_av(av: int) -> void:
		restored.append(av)

	func progress() -> Dictionary:
		return prog.duplicate(true)

	func set_skill_xp(av: int, xp: float) -> void:
		prog["skill_xp"][av - 6] = xp

	func set_xp(xp: float) -> void:
		prog["xp"] = xp

	func set_level(level: int) -> void:
		prog["level"] = level

	func set_perk_points(points: int) -> void:
		prog["perk_points"] = points

	func set_legendary(av: int, count: int) -> void:
		prog["legendary"][av - 6] = count

	func perk(id: int) -> Dictionary:
		return perks.get(id, {})

	func perk_rank(id: int) -> int:
		return int(ranks.get(id, 0))

	func add_perk(id: int) -> void:
		ranks[id] = 1

	func remove_perk(id: int) -> void:
		ranks.erase(id)

	func perk_conditions_met(id: int) -> bool:
		return conditions.get(id, true)

	func perk_tree(av: int) -> Array:
		return trees.get(av, [])

var d: FakeData
var pr: Progression


func before_each() -> void:
	d = FakeData.new()
	pr = Progression.new(d)


func test_lock_use_of_two_gives_100_progress() -> void:
	# 45 * 2 + 10 = 100.
	var r := pr.use_skill(LOCKPICKING, 2.0)
	expect(is_equal_approx(r["gained"], 100.0), "gained 100")
	expect(is_equal_approx(d.prog["skill_xp"][8], 100.0), "progress 100")
	expect_eq(d.bases[LOCKPICKING], 15.0, "no increase yet")
	expect_eq(r["skill_increases"].size(), 0, "no event")


func test_threshold_from_15_is_349_13() -> void:
	# 0.25 * 15^1.95 + 300 = 0.25 * 196.507 + 300 = 349.127.
	expect(absf(pr.skill_threshold(LOCKPICKING, 15.0) - 349.127) < 0.01, "lockpicking 15 -> 16")
	# OneHanded: 2 * 15^1.95 = 393.014.
	expect(absf(pr.skill_threshold(ONE_HANDED, 15.0) - 393.014) < 0.01, "one-handed 15 -> 16")


func test_progress_crossing_the_threshold_raises_the_skill_and_gives_xp() -> void:
	d.prog["skill_xp"][8] = 300.0
	var r := pr.use_skill(LOCKPICKING, 2.0)   # 300 + 100 = 400; 400 - 349.127 = 50.873
	expect_eq(d.bases[LOCKPICKING], 16.0, "skill 16")
	expect(absf(d.prog["skill_xp"][8] - 50.873) < 0.01, "carry-over")
	expect_eq(r["skill_increases"], [{"av": LOCKPICKING, "level": 16}], "event")
	expect(is_equal_approx(d.prog["xp"], 16.0), "16 XP for level 16")
	expect_eq(r["level_up_ready"], false, "16 < 100")


func test_one_handed_blow_gives_6_3() -> void:
	var r := pr.use_skill(ONE_HANDED, 1.0)   # 6.3 * 1 + 0
	expect(is_equal_approx(r["gained"], 6.3), "6.3")


func test_one_big_gain_can_raise_several_levels() -> void:
	# 15 -> 16 needs 349.127, 16 -> 17 needs 0.25 * 16^1.95 + 300 = 0.25 * 222.86 + 300 = 355.71.
	d.prog["skill_xp"][8] = 0.0
	pr.use_skill(LOCKPICKING, 16.0)   # 45 * 16 + 10 = 730 >= 349.127 + 355.715
	expect_eq(d.bases[LOCKPICKING], 17.0, "two levels")
	expect(is_equal_approx(d.prog["xp"], 33.0), "16 + 17 XP")


func test_skill_at_100_gains_nothing() -> void:
	d.bases[LOCKPICKING] = 100.0
	var r := pr.use_skill(LOCKPICKING, 2.0)
	expect_eq(r["skill_increases"].size(), 0, "no event")
	expect_eq(d.prog["skill_xp"][8], 0.0, "no progress")
	d.bases[LOCKPICKING] = 99.0
	d.prog["skill_xp"][8] = 0.0
	pr.use_skill(LOCKPICKING, 1000.0)
	expect_eq(d.bases[LOCKPICKING], 100.0, "reaches 100")
	expect_eq(d.prog["skill_xp"][8], 0.0, "and drops the rest")


func test_use_mult_hook_scales_progress() -> void:
	var p := Progression.new(d)
	var script := GDScript.new()
	script.source_code = "extends Progression\nfunc skill_use_mult(_av: int) -> float:\n\treturn 2.0\n"
	script.reload()
	p = script.new(d)
	expect(is_equal_approx(p.use_skill(ONE_HANDED, 1.0)["gained"], 12.6), "doubled")


func test_level_up_becomes_ready_once() -> void:
	# Level 1 needs 75 + 25 * 1 = 100 XP.
	d.prog["xp"] = 84.0
	d.prog["skill_xp"][8] = 349.0
	var r := pr.use_skill(LOCKPICKING, 1.0)   # 349 + 55 -> level 16 -> +16 XP = 100
	expect(is_equal_approx(d.prog["xp"], 100.0), "100 XP")
	expect_eq(r["level_up_ready"], true, "ready now")
	expect(pr.level_up_ready(), "ready")
	d.prog["skill_xp"][8] = 349.0
	expect_eq(pr.use_skill(LOCKPICKING, 1.0)["level_up_ready"], false, "already ready: not announced again")


func test_level_up_stamina() -> void:
	d.prog["xp"] = 120.0
	var r := pr.level_up(STAMINA)
	expect(r["ok"], "ok")
	expect_eq(d.prog["level"], 2, "level 2")
	expect(is_equal_approx(d.prog["xp"], 20.0), "120 - 100 left")
	expect_eq(d.bases[STAMINA], 110.0, "+10 Stamina")
	expect_eq(d.bases[CARRY], 305.0, "+5 carry weight")
	expect_eq(d.bases[HEALTH], 100.0, "Health unchanged")
	expect_eq(d.prog["perk_points"], 1, "a perk point")
	expect_eq(d.restored, [HEALTH, MAGICKA, STAMINA], "all three restored")


func test_level_up_health_has_no_carry_weight_and_needs_xp() -> void:
	expect_eq(pr.level_up(HEALTH)["reason"], "xp", "not enough XP")
	d.prog["xp"] = 100.0
	pr.level_up(HEALTH)
	expect_eq(d.bases[HEALTH], 110.0, "+10 Health")
	expect_eq(d.bases[CARRY], 300.0, "carry weight unchanged")
	expect_eq(pr.level_up(CARRY)["reason"], "attribute", "only H/M/S")


func test_banked_levels() -> void:
	# Level 1 -> 2 needs 100, level 2 -> 3 needs 125.
	d.prog["xp"] = 230.0
	expect(pr.level_up(MAGICKA)["ok"], "first")
	expect(pr.level_up_ready(), "still ready: 130 >= 125")
	expect(pr.level_up(MAGICKA)["ok"], "second")
	expect(not pr.level_up_ready(), "5 left, level 3 needs 150")
	expect_eq(d.prog["perk_points"], 2, "two points")


func test_increment_skill_gives_xp_per_level_and_keeps_the_fraction() -> void:
	d.prog["skill_xp"][8] = 174.5635   # half of 349.127
	var r := pr.increment_skill(LOCKPICKING, 3)
	expect_eq(d.bases[LOCKPICKING], 18.0, "18")
	expect(is_equal_approx(d.prog["xp"], 16.0 + 17.0 + 18.0), "51 XP")
	expect_eq(r["skill_increases"].size(), 3, "three events")
	var half := pr.skill_threshold(LOCKPICKING, 18.0) * 0.5
	expect(absf(d.prog["skill_xp"][8] - half) < 0.01, "half way to 19")


func test_increment_skill_stops_at_100() -> void:
	d.bases[LOCKPICKING] = 99.0
	var r := pr.increment_skill(LOCKPICKING, 5)
	expect_eq(d.bases[LOCKPICKING], 100.0, "100")
	expect_eq(r["skill_increases"].size(), 1, "one")


func test_perk_needs_points_tree_link_rank_and_conditions() -> void:
	expect_eq(pr.perk_blocker(NOVICE), "points", "no points")
	d.prog["perk_points"] = 3
	expect(pr.can_take_perk(NOVICE), "linked from the root")
	expect_eq(pr.perk_blocker(ADEPT_1), "link", "needs Novice first")
	expect_eq(pr.perk_blocker(LOOSE), "tree", "in no tree")
	expect_eq(pr.perk_blocker(0x999), "unknown", "unknown")
	d.conditions[NOVICE] = false
	expect_eq(pr.perk_blocker(NOVICE), "conditions", "prerequisite fails")
	d.conditions.erase(NOVICE)
	var status := pr.perk_status(NOVICE)
	expect(status["can"] and status["av"] == LOCKPICKING, "status")
	expect(pr.take_perk(NOVICE)["ok"], "take Novice")
	expect_eq(d.prog["perk_points"], 2, "a point spent")
	expect_eq(pr.perk_blocker(NOVICE), "has", "owned")
	expect(pr.can_take_perk(ADEPT_1), "now linked")
	expect_eq(pr.perk_blocker(ADEPT_2), "rank", "second rank needs the first")
	pr.take_perk(ADEPT_1)
	expect(pr.can_take_perk(ADEPT_2), "rank 2")
	expect(pr.can_take_perk(EXPERT), "Expert hangs off the Adept node, which is taken")
	d.perks[ADEPT_1]["playable"] = false
	d.perks[ADEPT_2]["playable"] = false
	expect_eq(pr.perk_blocker(ADEPT_2), "playable", "not playable")


func test_perk_with_a_taken_link_can_be_taken() -> void:
	d.prog["perk_points"] = 1
	d.ranks[ADEPT_1] = 1   # the node is taken at rank 1
	expect(pr.can_take_perk(EXPERT), "linked from the taken Adept node")
	expect(pr.take_perk(EXPERT)["ok"], "taken")
	expect_eq(pr.take_perk(EXPERT)["reason"], "has", "once")


func test_legendary_resets_and_refunds() -> void:
	d.bases[LOCKPICKING] = 100.0
	d.prog["skill_xp"][8] = 5.0
	d.ranks = {NOVICE: 1, ADEPT_1: 1, ADEPT_2: 1}
	d.prog["perk_points"] = 1
	var r := pr.make_legendary(LOCKPICKING)
	expect(r["ok"], "ok")
	expect_eq(d.bases[LOCKPICKING], 15.0, "back to 15")
	expect_eq(d.ranks.size(), 0, "tree perks removed (all ranks)")
	expect_eq(r["refunded"], 3, "three refunded")
	expect_eq(d.prog["perk_points"], 4, "1 + 3")
	expect_eq(d.prog["legendary"][8], 1, "legendary 1")
	expect_eq(d.prog["skill_xp"][8], 0.0, "progress 0")


func test_legendary_needs_100() -> void:
	expect_eq(pr.make_legendary(LOCKPICKING)["reason"], "level", "15 is not 100")
	expect_eq(d.bases[LOCKPICKING], 15.0, "unchanged")


func test_skill_use_perk_scales_only_its_skill() -> void:
	d.skill_use_perk = {LOCKPICKING: 1.5}
	# 45 * 2 * 1.5 + 10 = 145
	expect(is_equal_approx(pr.use_skill(LOCKPICKING, 2.0)["gained"], 145.0), "Lockpicking x1.5")
	expect(is_equal_approx(pr.use_skill(ONE_HANDED, 1.0)["gained"], 6.3), "other skills unchanged")


# ---- crafting use (UESP Skyrim:Leveling); the amounts before the AVIF use mult

const SMITHING_AV := 10
const ALCHEMY_AV := 16
const ENCHANTING_AV := 23


func test_creating_an_item_gives_25_plus_3_value_to_the_0_65() -> void:
	# 20 ^ 0.65 = 7.0099; 25 + 3 * 7.0099 = 46.03; two of them 92.06.
	var one := pr.crafting_use_amount({"kind": "create", "skill": SMITHING_AV, "count": 1.0, "value": 20.0})
	expect(absf(one - 46.03) < 0.01, "one dagger: %f" % one)
	var two := pr.crafting_use_amount({"kind": "create", "skill": SMITHING_AV, "count": 2.0, "value": 20.0})
	expect(absf(two - 92.06) < 0.02, "two daggers: %f" % two)


func test_improving_an_item_uses_the_pack_exponent_for_the_value() -> void:
	# 3.8 * 2 ^ 0.6 (1.5157) * 1 ^ 0.5 = 5.760; 3.8 * 8 ^ 0.6 (3.4822) * 2 ^ 0.5 = 18.71.
	var a := pr.crafting_use_amount({"kind": "improve", "delta_value": 2.0, "delta_quality": 1.0})
	expect(absf(a - 5.760) < 0.01, "value 2, quality 1: %f" % a)
	var b := pr.crafting_use_amount({"kind": "improve", "delta_value": 8.0, "delta_quality": 2.0})
	expect(absf(b - 18.71) < 0.02, "value 8, quality 2: %f" % b)
	expect_eq(pr.crafting_use_amount({"kind": "improve", "delta_value": 0.0, "delta_quality": 0.0}), 0.0, "no improvement")


func test_a_potion_gives_its_value_plus_one() -> void:
	expect_eq(pr.crafting_use_amount({"kind": "brew", "value": 100.0}), 101.0, "value 100")


func test_enchanting_disenchanting_and_recharging() -> void:
	expect_eq(pr.crafting_use_amount({"kind": "enchant", "count": 1.0}), 1.0, "1 per item")
	expect_eq(pr.crafting_use_amount({"kind": "disenchant", "value": 800.0}), 2.0, "1 per 400 gold")
	var expected := [0.05, 0.1, 0.2, 0.4, 0.6]
	for soul in range(1, 6):
		expect(is_equal_approx(pr.crafting_use_amount({"kind": "recharge", "soul": soul}), expected[soul - 1]), "soul %d" % soul)


func test_crafting_use_goes_through_the_skill_use_mult() -> void:
	# Lockpicking stands in for a crafting skill here (its fake use mult is 45, offset 10): 1 enchant = 1 use.
	var r := pr.crafting_use({"kind": "enchant", "skill": LOCKPICKING, "count": 1.0})
	expect(is_equal_approx(r["gained"], 55.0), "1 * 45 + 10")
