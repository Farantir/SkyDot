# SPDX-License-Identifier: GPL-3.0-or-later
#
# SkillsModel (ui/skills_model.gd) over a fake session: the ring order, the header, a perk tree's stars
# (positions, ranks, required level, status), the cursor, taking a perk after a confirmation, legendary,
# and the level-up flow with banked levels.
extends "gd_test.gd"

const ONE_HANDED := 6
const HEALTH := 24
const ARMSMAN_1 := 0x100
const ARMSMAN_2 := 0x101
const STANCE := 0x102
const SAVAGE := 0x103

class FakeData extends ProgressionData:
	var bases := {}
	var prog := {"skill_xp": [], "xp": 0.0, "level": 1, "perk_points": 1, "trainings": 0, "legendary": []}
	var ranks := {}
	var perks := {
		0x100: {"name": "Armsman", "description": "20% more damage.", "next_perk": 0x101, "playable": true, "conditions": []},
		0x101: {"name": "Armsman", "description": "40% more damage.", "next_perk": 0, "playable": true,
			"conditions": [{"function": 277, "compare_to": 30.0, "param1": 6}]},
		0x102: {"name": "Fighting Stance", "description": "Power attacks cost less.", "next_perk": 0, "playable": true,
			"conditions": [{"function": 277, "compare_to": 20.0, "param1": 6}]},
		0x103: {"name": "Savage Strike", "description": "Standing power attacks do more.", "next_perk": 0,
			"playable": true, "conditions": [{"function": 277, "compare_to": 50.0, "param1": 6}]},
	}
	# y grows upward in the data: Armsman lowest, Savage Strike highest; the root has junk values.
	var tree := [
		{"index": 0, "perk": 0, "grid_x": 9, "grid_y": 9, "x": 5.0, "y": 5.0, "links": [1]},
		{"index": 1, "perk": 0x100, "grid_x": 2, "grid_y": 0, "x": 0.0, "y": 0.0, "links": [2]},
		{"index": 2, "perk": 0x102, "grid_x": 2, "grid_y": 1, "x": 0.0, "y": 1.0, "links": [3]},
		{"index": 3, "perk": 0x103, "grid_x": 2, "grid_y": 2, "x": 1.0, "y": 2.0, "links": []},
	]

	func _init() -> void:
		for i in 18:
			prog["skill_xp"].append(0.0)
			prog["legendary"].append(0)
		for av in range(6, 24):
			bases[av] = 15.0

	func gmst_float(name: String, fallback: float) -> float:
		return {"fSkillUseCurve": 1.95, "fXPLevelUpBase": 75.0, "fXPLevelUpMult": 25.0}.get(name, fallback)

	func gmst_int(name: String, fallback: int) -> int:
		return {"iAVDhmsLevelUp": 10}.get(name, fallback)

	func skill_info(av: int) -> Dictionary:
		var names := {6: "One-handed", 7: "Two-handed", 8: "Archery", 9: "Block", 10: "Smithing", 11: "Heavy Armor",
			12: "Light Armor", 13: "Pickpocket", 14: "Lockpicking", 15: "Sneak", 16: "Alchemy", 17: "Speech",
			18: "Alteration", 19: "Conjuration", 20: "Destruction", 21: "Illusion", 22: "Restoration", 23: "Enchanting"}
		return {"name": names[av], "use_mult": 1.0, "use_offset": 0.0, "improve_mult": 2.0, "improve_offset": 0.0,
			"perk_tree": tree if av == 6 else []}

	func base_av(av: int) -> float:
		return float(bases[av]) if bases.has(av) else 100.0

	func current_av(av: int) -> float:
		return base_av(av)

	func set_base_av(av: int, value: float) -> void:
		bases[av] = value

	func restore_av(_av: int) -> void:
		pass

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

	func set_trainings(count: int) -> void:
		prog["trainings"] = count

	func perk(id: int) -> Dictionary:
		return perks.get(id, {})

	func perk_rank(id: int) -> int:
		return int(ranks.get(id, 0))

	func add_perk(id: int) -> void:
		ranks[id] = 1

	func remove_perk(id: int) -> void:
		ranks.erase(id)

	func perk_conditions_met(id: int) -> bool:
		for c: Dictionary in perks[id]["conditions"]:
			if c["function"] == 277 and base_av(int(c["param1"])) < float(c["compare_to"]):
				return false
		return true

	func perk_tree(av: int) -> Array:
		return skill_info(av)["perk_tree"]

class FakeSession extends RefCounted:
	var progression: Progression

	func level_up(attribute: int) -> Dictionary:
		return progression.level_up(attribute)

	func take_perk(perk: int) -> Dictionary:
		return progression.take_perk(perk)

var d: FakeData
var session: FakeSession
var m: SkillsModel


func before_each() -> void:
	d = FakeData.new()
	session = FakeSession.new()
	session.progression = Progression.new(d)
	m = SkillsModel.new(session)


func test_the_ring_has_the_18_skills_in_arc_order() -> void:
	expect_eq(m.skills.size(), 18, "18")
	var avs: Array = m.skills.map(func(s: Dictionary) -> int: return s["av"])
	expect_eq(avs, [6, 7, 8, 9, 10, 11, 18, 19, 20, 21, 22, 23, 12, 15, 14, 13, 17, 16], "warrior, mage, thief")
	expect_eq(m.skills[0]["arc"], "Warrior", "arc")
	expect_eq(m.skills[0]["name"], "One-handed", "name")
	m.select_skill(-1)
	expect_eq(m.skills[m.selected]["av"], 16, "wraps backwards")


func test_header_counts_banked_levels() -> void:
	expect(not m.choosing_attribute(), "nothing banked")
	d.prog["xp"] = 100.0 + 125.0   # level 1 needs 100, level 2 needs 125
	m.refresh()
	expect_eq(m.header["banked"], 2, "two levels")
	expect(m.choosing_attribute(), "asks for an attribute")


func test_stars_have_positions_ranks_and_requirements() -> void:
	expect_eq(m.nodes.size(), 3, "the root is not a star")
	var armsman := m.node(1)
	expect_eq(armsman["name"], "Armsman", "name")
	expect_eq(armsman["ranks"], 2, "two ranks")
	expect_eq(armsman["rank"], 0, "none taken")
	expect_eq(armsman["status"], "available", "available with a point")
	expect(armsman["pos"].y > m.node(3)["pos"].y, "later perks are higher on the screen")
	expect_eq(m.node(1)["pos"], Vector2(0.0, 1.0), "lowest left")
	expect_eq(m.node(3)["pos"], Vector2(1.0, 0.0), "highest right")
	expect_eq(m.node(2)["required_level"], 20, "required skill from the conditions")
	expect_eq(m.node(2)["status"], "locked", "behind Armsman")
	expect_eq(m.node(2)["blocker_text"], "Take a perk it follows first", "why")
	expect_eq(m.cursor, 1, "the cursor starts on the first available star")


func test_cursor_moves_by_direction_and_links() -> void:
	m.move_cursor(Vector2.UP)
	expect_eq(m.cursor, 2, "up to the next star")
	m.move_cursor(Vector2.DOWN)
	expect_eq(m.cursor, 1, "and back")
	m.move_cursor(Vector2.LEFT)
	expect_eq(m.cursor, 1, "nothing to the left")
	m.step_link(true)
	expect_eq(m.cursor, 2, "along the link")
	m.step_link(false)
	expect_eq(m.cursor, 1, "against it")


func test_taking_a_perk_asks_first_and_updates_the_status() -> void:
	expect(m.ask_take(), "available")
	expect_eq(m.pending["kind"], "perk", "asked")
	m.cancel()
	expect_eq(d.ranks.size(), 0, "nothing taken yet")
	m.ask_take()
	var r := m.confirm()
	expect(r["ok"], "taken")
	expect_eq(d.prog["perk_points"], 0, "a point spent")
	expect_eq(m.node(1)["rank"], 1, "rank 1 of 2")
	expect_eq(m.node(1)["status"], "locked", "rank 2 needs the skill and a point")
	expect_eq(m.node(1)["blocker_text"], "No perk points", "why")
	expect_eq(m.header["perk_points"], 0, "header")


func test_a_locked_star_cannot_be_asked_for() -> void:
	m.move_cursor(Vector2.UP)
	expect(not m.ask_take(), "locked")
	expect(m.pending.is_empty(), "nothing waits")
	expect(m.note != "", "says why")


func test_level_up_flow_spends_one_banked_level_per_choice() -> void:
	d.prog["xp"] = 100.0 + 125.0
	m.refresh()
	m.choose_attribute(HEALTH)
	expect_eq(d.prog["level"], 2, "level 2")
	expect(m.choosing_attribute(), "another is waiting")
	m.choose_attribute(HEALTH)
	expect_eq(d.prog["level"], 3, "level 3")
	expect(not m.choosing_attribute(), "the trees now")
	expect_eq(d.prog["perk_points"], 3, "a point each")


func test_legendary_needs_100_and_confirms() -> void:
	expect(not m.ask_legendary(), "base 15")
	d.bases[ONE_HANDED] = 100.0
	m.refresh()
	expect(m.can_legendary(), "100")
	expect(m.ask_legendary(), "asked")
	m.confirm()
	expect_eq(d.bases[ONE_HANDED], 15.0, "reset to 15")
	expect_eq(m.current_skill()["legendary"], 1, "counted")
